// modules/server.c — built-in `server` module: minimal HTTP/1.1 server
// primitives. The MyMo program drives the accept loop, either blocking
// (accept) or event-driven (accept_nb + read_request, as mono does).
//
// Exports:
//   server.listen(host, port, backlog)  -> int  (server fd)
//   server.accept(fd)                   -> dict {client_fd, method, path, body, headers}
//                                          on transport error returns Nil
//   server.accept_nb(fd)                -> int client fd (non-blocking), or Nil
//                                          if no client is waiting
//   server.read_request(client_fd)      -> request dict when complete, False
//                                          if more data is needed, Nil if the
//                                          client closed/misbehaved (fd closed)
//   respond / respond_json return True when the connection was kept open
//   (HTTP keep-alive, accept_nb connections only), False when it was closed.
//   Request dicts carry "keep_alive".
//   server.respond(client_fd, status, body)        -> nil
//   server.respond_json(client_fd, status, json)   -> nil
//   server.close(fd)                    -> nil
//
// Typical usage:
//   from "server" use listen, accept, respond
//   srv = listen("0.0.0.0", 8080, 16)
//   while True:
//       req = accept(srv)
//       respond(req["client_fd"], 200, "ok\n")

#include "../include/mymo_module.h"
#include "../datatypes/dict.h"
#include "../datatypes/string.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define close_fd(fd) closesocket(fd)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <strings.h>
  #define close_fd(fd) close(fd)
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

static Value srv_listen(MVM *vm, uint argc, Value argv[])
{
    const char *host;
    long port, backlog;
    if (!mymo_parse(vm, "server.listen", argc, argv, "sii",
                    &host, &port, &backlog))
        return MYMO_ERROR;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        runtimeError(vm, "server.listen(): socket(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (host[0] == '\0' || strcmp(host, "0.0.0.0") == 0) {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close_fd(fd);
        runtimeError(vm, "server.listen(): bad bind addr '%s'", host);
        return MYMO_ERROR;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close_fd(fd);
        runtimeError(vm, "server.listen(): bind(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    if (listen(fd, (int)backlog) != 0) {
        close_fd(fd);
        runtimeError(vm, "server.listen(): listen(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    return objectToValue(mymo_int(vm, fd));
}

// ---- request parsing (shared by the blocking and non-blocking paths) ----

#define MAX_HEADER_BYTES (16 * 1024)
#define MAX_BODY_BYTES   (8 * 1024 * 1024)

// Offset just past the "\r\n\r\n" that ends the headers, or -1.
static long header_end(const char *buf, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (long)(i + 4);
    return -1;
}

// Content-Length from the header block (0 if absent, -1 if invalid).
static long content_length(const char *buf, long hdr_len)
{
    const char *p = buf;
    const char *end = buf + hdr_len;
    while (p < end)
    {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        if ((size_t)(eol - p) > 15 && strncasecmp(p, "content-length:", 15) == 0)
        {
            char *stop;
            long v = strtol(p + 15, &stop, 10);
            return v < 0 ? -1 : v;
        }
        p = eol + 1;
    }
    return 0;
}

// Lowercase in-place.
static void lower_inplace(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) s[i] = (char)tolower((unsigned char)s[i]);
}

// Build the request dict from a complete request in `buf` (modified in
// place): headers are buf[0..hdr_len), the body follows. NULL if the
// request line or headers are malformed.
// Whether the client wants the connection kept open after this request:
// HTTP/1.1 unless it sends `Connection: close`; HTTP/1.0 only with
// `Connection: keep-alive`. Must run before build_request edits `buf`.
static bool wants_keep_alive(const char *buf, long hdr_len)
{
    const char *eol = memchr(buf, '\r', (size_t)hdr_len);
    bool http11 = eol && eol - buf >= 8 && memcmp(eol - 8, "HTTP/1.1", 8) == 0;
    const char *p = buf;
    const char *end = buf + hdr_len;
    while (p < end)
    {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) break;
        if ((size_t)(line_end - p) > 11 && strncasecmp(p, "connection:", 11) == 0)
        {
            const char *v = p + 11;
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            if (strncasecmp(v, "close", 5) == 0) return false;
            if (strncasecmp(v, "keep-alive", 10) == 0) return true;
        }
        p = line_end + 1;
    }
    return http11;
}

static MyMoObject *build_request(MVM *vm, int cfd, char *buf, long hdr_len,
                                 const char *body, long body_len)
{
    bool keep_alive = wants_keep_alive(buf, hdr_len);
    buf[hdr_len - 1] = '\0';
    // Request line: METHOD SP PATH SP HTTP/1.x CRLF
    char *space1 = strchr(buf, ' ');
    if (!space1) return NULL;
    *space1 = '\0';
    char *path = space1 + 1;
    char *space2 = strchr(path, ' ');
    if (!space2) return NULL;
    *space2 = '\0';
    char *eol = strstr(space2 + 1, "\r\n");
    if (!eol) return NULL;

    // Headers into a dict with lowercased names.
    MyMoDict *headers = newDict(vm);
    char *p = eol + 2;
    char *hend = buf + hdr_len - 1;
    while (p < hend)
    {
        char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end == p) break;
        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon)
        {
            lower_inplace(p, (size_t)(colon - p));
            char *vstart = colon + 1;
            while (vstart < line_end && (*vstart == ' ' || *vstart == '\t')) vstart++;
            MyMoObject *k = AS_OBJECT(newString(vm, p, (int)(colon - p)));
            setEntry(vm, headers, k, mymo_strn(vm, vstart, (int)(line_end - vstart)));
        }
        p = line_end + 2;
    }

    MyMoDict *req = newDict(vm);
    setEntry(vm, req, AS_OBJECT(newString(vm, "client_fd", 9)), mymo_int(vm, cfd));
    setEntry(vm, req, AS_OBJECT(newString(vm, "method", 6)), mymo_str(vm, buf));
    setEntry(vm, req, AS_OBJECT(newString(vm, "path", 4)), mymo_str(vm, path));
    setEntry(vm, req, AS_OBJECT(newString(vm, "headers", 7)), AS_OBJECT(headers));
    setEntry(vm, req, AS_OBJECT(newString(vm, "body", 4)), mymo_strn(vm, body ? body : "", (int)body_len));
    setEntryV(vm, req, AS_OBJECT(newString(vm, "keep_alive", 10)), V_BOOL_VAL(keep_alive));
    return AS_OBJECT(req);
}

// Wait up to `ms` for fd to become readable/writable (non-blocking sockets).
static int wait_fd(int fd, bool write, int ms)
{
#ifndef _WIN32
    struct pollfd pfd = {fd, (short)(write ? POLLOUT : POLLIN), 0};
    return poll(&pfd, 1, ms);
#else
    (void)fd; (void)write; (void)ms;
    return 1;
#endif
}

static bool would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

// ---- blocking accept ------------------------------------------------------

static Value srv_accept(MVM *vm, uint argc, Value argv[])
{
    long sfd;
    if (!mymo_parse(vm, "server.accept", argc, argv, "i", &sfd))
        return MYMO_ERROR;

    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int cfd = accept((int)sfd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        runtimeError(vm, "server.accept(): %s", strerror(errno));
        return MYMO_ERROR;
    }

#ifndef _WIN32
    // On BSD (macOS, FreeBSD) accepted sockets INHERIT the O_NONBLOCK
    // flag from the listening socket; on Linux they don't. Clear it so
    // this blocking accept reads the request with blocking recv() on
    // both platforms. Use accept_nb/read_request for non-blocking I/O.
    int flags = fcntl(cfd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK))
        fcntl(cfd, F_SETFL, flags & ~O_NONBLOCK);
#endif

    // Read until the headers are complete, then the rest of the body.
    size_t cap = MAX_HEADER_BYTES, n = 0;
    char *buf = malloc(cap);
    long hdr = -1;
    while (hdr < 0)
    {
        if (n == cap) { free(buf); close_fd(cfd); return MYMO_NIL; }
        ssize_t r = recv(cfd, buf + n, cap - n, 0);
        if (r <= 0) { free(buf); close_fd(cfd); return MYMO_NIL; }
        n += (size_t)r;
        hdr = header_end(buf, n);
    }
    long clen = content_length(buf, hdr);
    if (clen < 0 || clen > MAX_BODY_BYTES) { free(buf); close_fd(cfd); return MYMO_NIL; }
    if ((size_t)(hdr + clen) > cap)
    {
        cap = (size_t)(hdr + clen);
        buf = realloc(buf, cap);
    }
    while (n < (size_t)(hdr + clen))
    {
        ssize_t r = recv(cfd, buf + n, (size_t)(hdr + clen) - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    long body_len = (long)n - hdr < clen ? (long)n - hdr : clen;
    MyMoObject *req = build_request(vm, cfd, buf, hdr, buf + hdr, body_len);
    free(buf);
    if (!req) { close_fd(cfd); return MYMO_NIL; }
    return objectToValue(req);
}

// ---- non-blocking connections --------------------------------------------
//
// accept_nb(listen_fd) accepts one pending client (or returns Nil), and
// read_request(fd) reads whatever has arrived without blocking, buffering
// partial requests per fd until one is complete. mono's event loop uses
// these to serve many slow clients at once.

// Keep-alive: only connections accepted with accept_nb are kept open (the
// event loop re-polls them); blocking accept() loops always close.
#define MAX_REQUESTS_PER_CONN 100

typedef struct
{
    char *buf;
    size_t len, cap;
    bool nonblocking; // accepted via accept_nb
    bool keep_alive;  // the current request asked to keep the connection
    int served;       // requests answered on this connection
} ConnBuf;

static ConnBuf *g_conns = NULL;
static int g_conns_cap = 0;

static ConnBuf *conn_get(int fd)
{
    if (fd >= g_conns_cap)
    {
        int cap = g_conns_cap ? g_conns_cap : 64;
        while (cap <= fd) cap *= 2;
        g_conns = realloc(g_conns, sizeof(ConnBuf) * (size_t)cap);
        memset(g_conns + g_conns_cap, 0, sizeof(ConnBuf) * (size_t)(cap - g_conns_cap));
        g_conns_cap = cap;
    }
    return &g_conns[fd];
}

static void conn_drop(int fd)
{
    if (fd >= 0 && fd < g_conns_cap)
    {
        free(g_conns[fd].buf);
        memset(&g_conns[fd], 0, sizeof(ConnBuf));
    }
}

static Value srv_accept_nb(MVM *vm, uint argc, Value argv[])
{
    long sfd;
    if (!mymo_parse(vm, "server.accept_nb", argc, argv, "i", &sfd))
        return MYMO_ERROR;
#ifdef _WIN32
    runtimeError(vm, "server.accept_nb(): not supported on Windows yet");
    return MYMO_ERROR;
#else
    int cfd = accept((int)sfd, NULL, NULL);
    if (cfd < 0)
    {
        if (would_block() || errno == ECONNABORTED)
            return MYMO_NIL;
        runtimeError(vm, "server.accept_nb(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    int flags = fcntl(cfd, F_GETFL, 0);
    fcntl(cfd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
    conn_drop(cfd); // a recycled fd number must start with an empty buffer
    conn_get(cfd)->nonblocking = true;
    return objectToValue(mymo_int(vm, cfd));
#endif
}

static Value srv_read_request(MVM *vm, uint argc, Value argv[])
{
    long fd;
    if (!mymo_parse(vm, "server.read_request", argc, argv, "i", &fd))
        return MYMO_ERROR;
    ConnBuf *c = conn_get((int)fd);
    bool eof = false;
    for (;;)
    {
        if (c->len == c->cap)
        {
            c->cap = c->cap ? c->cap * 2 : 4096;
            if (c->cap > MAX_HEADER_BYTES + MAX_BODY_BYTES) { eof = true; break; }
            c->buf = realloc(c->buf, c->cap);
        }
        ssize_t r = recv((int)fd, c->buf + c->len, c->cap - c->len, 0);
        if (r > 0) { c->len += (size_t)r; continue; }
        if (r < 0 && would_block()) break;
        eof = true; // closed or failed
        break;
    }
    long hdr = header_end(c->buf, c->len);
    if (hdr < 0 && c->len > MAX_HEADER_BYTES) eof = true;
    long clen = hdr < 0 ? 0 : content_length(c->buf, hdr);
    if (clen < 0 || clen > MAX_BODY_BYTES) { hdr = -1; eof = true; }
    if (hdr >= 0 && c->len >= (size_t)(hdr + clen))
    {
        c->keep_alive = wants_keep_alive(c->buf, hdr) && c->served + 1 < MAX_REQUESTS_PER_CONN;
        MyMoObject *req = build_request(vm, (int)fd, c->buf, hdr, c->buf + hdr, clen);
        if (!req)
        {
            conn_drop((int)fd);
            close_fd((int)fd);
            return MYMO_NIL;
        }
        // Keep any bytes after this request (a pipelined next request).
        size_t used = (size_t)(hdr + clen);
        memmove(c->buf, c->buf + used, c->len - used);
        c->len -= used;
        return objectToValue(req);
    }
    if (eof)
    {
        conn_drop((int)fd);
        close_fd((int)fd);
        return MYMO_NIL;
    }
    return MYMO_FALSE; // incomplete: call again when fd is readable
}

// ---- responses ------------------------------------------------------------

// Works on blocking and non-blocking sockets (waits for writability).
static int send_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (would_block() && wait_fd(fd, true, 10000) > 0) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static const char *reason_phrase(long status)
{
    switch (status)
    {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return status < 400 ? "OK" : "Error";
    }
}

static Value do_respond(MVM *vm, const char *fn,
                        long cfd, long status,
                        const char *body, int blen,
                        const char *content_type)
{
    ConnBuf *c = (cfd >= 0 && cfd < g_conns_cap) ? &g_conns[cfd] : NULL;
    bool keep = c && c->nonblocking && c->keep_alive;
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %ld %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, reason_phrase(status), content_type, blen,
                     keep ? "keep-alive" : "close");
    int failed = send_all((int)cfd, head, (size_t)n) < 0 ||
                 send_all((int)cfd, body, (size_t)blen) < 0;
    int err = errno;
    if (failed || !keep)
    {
        conn_drop((int)cfd);
        close_fd((int)cfd);
    }
    else
    {
        c->served++;
        c->keep_alive = false; // decided afresh for the next request
    }
    if (failed)
    {
        runtimeError(vm, "%s(): write failed: %s", fn, strerror(err));
        return MYMO_ERROR;
    }
    // True: the connection stays open for another request (keep-alive).
    return MYMO_BOOL(keep);
}

static Value srv_respond(MVM *vm, uint argc, Value argv[])
{
    long cfd, status;
    const char *body; int blen;
    if (!mymo_parse(vm, "server.respond", argc, argv, "iisn",
                    &cfd, &status, &body, &blen))
        return MYMO_ERROR;
    return do_respond(vm, "server.respond", cfd, status, body, blen,
                      "text/plain; charset=utf-8");
}

static Value srv_respond_json(MVM *vm, uint argc, Value argv[])
{
    long cfd, status;
    const char *body; int blen;
    if (!mymo_parse(vm, "server.respond_json", argc, argv, "iisn",
                    &cfd, &status, &body, &blen))
        return MYMO_ERROR;
    return do_respond(vm, "server.respond_json", cfd, status, body, blen,
                      "application/json");
}

static Value srv_close(MVM *vm, uint argc, Value argv[])
{
    long fd;
    if (!mymo_parse(vm, "server.close", argc, argv, "i", &fd)) return MYMO_ERROR;
    conn_drop((int)fd);
    close_fd((int)fd);
    return MYMO_NIL;
}

MyMoObject *serverModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"listen",        srv_listen},
        {"accept",        srv_accept},
        {"accept_nb",     srv_accept_nb},
        {"read_request",  srv_read_request},
        {"respond",       srv_respond},
        {"respond_json",  srv_respond_json},
        {"close",         srv_close},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "server", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
