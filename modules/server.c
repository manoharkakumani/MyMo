// modules/server.c — built-in `server` module: minimal HTTP/1.1 server
// primitives. Single-threaded blocking; the MyMo program drives the
// accept loop.
//
// Exports:
//   server.listen(host, port, backlog)  -> int  (server fd)
//   server.accept(fd)                   -> dict {client_fd, method, path, body, headers}
//                                          on transport error returns Nil
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
  #define close_fd(fd) close(fd)
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

static MyMoObject *srv_listen(MVM *vm, uint argc, MyMoObject *argv[])
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
    return mymo_int(vm, fd);
}

// Read until we see "\r\n\r\n" or buffer fills. Returns total bytes read or -1.
static ssize_t read_headers(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    while (off < cap) {
        ssize_t n = recv(fd, buf + off, cap - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
        if (off >= 4) {
            for (size_t i = 0; i + 3 < off; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' &&
                    buf[i+2] == '\r' && buf[i+3] == '\n')
                    return (ssize_t)off;
            }
        }
    }
    return -1;
}

// Lowercase in-place.
static void lower_inplace(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) s[i] = (char)tolower((unsigned char)s[i]);
}

static MyMoObject *srv_accept(MVM *vm, uint argc, MyMoObject *argv[])
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
    // flag from the listening socket; on Linux they don't. When the
    // user puts the listen fd in non-blocking mode for a runloop-driven
    // accept loop, we still want the header-read below to block
    // briefly while the client sends — otherwise recv() races the
    // TCP handshake and returns EAGAIN, making accept() report a
    // bogus "transport error" (Nil) for every well-formed client.
    // Clear O_NONBLOCK on the accepted fd to normalize the two
    // platforms and decouple per-connection blocking from the listen
    // fd's mode. Callers that explicitly want a non-blocking client
    // fd can call runloop.nonblock(req["client_fd"]) themselves.
    int flags = fcntl(cfd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK))
        fcntl(cfd, F_SETFL, flags & ~O_NONBLOCK);
#endif

    char buf[16384];
    ssize_t n = read_headers(cfd, buf, sizeof(buf) - 1);
    if (n < 0) {
        close_fd(cfd);
        return MYMO_NIL;
    }
    buf[n] = '\0';

    // Parse request line: METHOD SP PATH SP HTTP/1.x CRLF
    char *space1 = strchr(buf, ' ');
    if (!space1) { close_fd(cfd); return MYMO_NIL; }
    *space1 = '\0';
    char *path = space1 + 1;
    char *space2 = strchr(path, ' ');
    if (!space2) { close_fd(cfd); return MYMO_NIL; }
    *space2 = '\0';
    char *eol = strstr(space2 + 1, "\r\n");
    if (!eol) { close_fd(cfd); return MYMO_NIL; }

    // Parse headers into a dict. Lowercase header names.
    MyMoDict *headers = newDict(vm);
    long content_length = 0;
    char *p = eol + 2;
    while (p < buf + n) {
        if (p[0] == '\r' && p[1] == '\n') { p += 2; break; }
        char *line_end = strstr(p, "\r\n");
        if (!line_end) break;
        char *colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            *colon = '\0';
            lower_inplace(p, (size_t)(colon - p));
            char *vstart = colon + 1;
            while (vstart < line_end && (*vstart == ' ' || *vstart == '\t')) vstart++;
            int vlen = (int)(line_end - vstart);
            MyMoObject *k = AS_OBJECT(newString(vm, p, (int)(colon - p)));
            MyMoObject *v = mymo_strn(vm, vstart, vlen);
            setEntry(vm, headers, k, v);
            if (strcmp(p, "content-length") == 0) {
                content_length = strtol(vstart, NULL, 10);
            }
        }
        p = line_end + 2;
    }

    // Pull body: anything already in buf past p, plus whatever else the
    // connection says is coming via Content-Length.
    char *body = NULL;
    long body_len = 0;
    long already = (long)(buf + n - p);
    if (content_length > 0) {
        body = malloc((size_t)content_length + 1);
        if (!body) {
            close_fd(cfd);
            runtimeError(vm, "server.accept(): out of memory for body");
            return MYMO_ERROR;
        }
        if (already > content_length) already = content_length;
        memcpy(body, p, (size_t)already);
        body_len = already;
        while (body_len < content_length) {
            ssize_t r = recv(cfd, body + body_len, (size_t)(content_length - body_len), 0);
            if (r <= 0) break;
            body_len += r;
        }
        body[body_len] = '\0';
    }

    MyMoDict *req = newDict(vm);
    setEntry(vm, req, AS_OBJECT(newString(vm, "client_fd", 9)),
             mymo_int(vm, cfd));
    setEntry(vm, req, AS_OBJECT(newString(vm, "method", 6)),
             mymo_str(vm, buf));
    setEntry(vm, req, AS_OBJECT(newString(vm, "path", 4)),
             mymo_str(vm, path));
    setEntry(vm, req, AS_OBJECT(newString(vm, "headers", 7)),
             AS_OBJECT(headers));
    setEntry(vm, req, AS_OBJECT(newString(vm, "body", 4)),
             body ? mymo_strn(vm, body, (int)body_len) : mymo_str(vm, ""));
    free(body);
    return AS_OBJECT(req);
}

static int send_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n < 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static MyMoObject *do_respond(MVM *vm, const char *fn,
                              long cfd, long status,
                              const char *body, int blen,
                              const char *content_type)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %ld OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, content_type, blen);
    if (send_all((int)cfd, head, (size_t)n) < 0 ||
        send_all((int)cfd, body, (size_t)blen) < 0)
    {
        close_fd((int)cfd);
        runtimeError(vm, "%s(): write failed: %s", fn, strerror(errno));
        return MYMO_ERROR;
    }
    close_fd((int)cfd);
    return MYMO_NIL;
}

static MyMoObject *srv_respond(MVM *vm, uint argc, MyMoObject *argv[])
{
    long cfd, status;
    const char *body; int blen;
    if (!mymo_parse(vm, "server.respond", argc, argv, "iisn",
                    &cfd, &status, &body, &blen))
        return MYMO_ERROR;
    return do_respond(vm, "server.respond", cfd, status, body, blen,
                      "text/plain; charset=utf-8");
}

static MyMoObject *srv_respond_json(MVM *vm, uint argc, MyMoObject *argv[])
{
    long cfd, status;
    const char *body; int blen;
    if (!mymo_parse(vm, "server.respond_json", argc, argv, "iisn",
                    &cfd, &status, &body, &blen))
        return MYMO_ERROR;
    return do_respond(vm, "server.respond_json", cfd, status, body, blen,
                      "application/json");
}

static MyMoObject *srv_close(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd;
    if (!mymo_parse(vm, "server.close", argc, argv, "i", &fd)) return MYMO_ERROR;
    close_fd((int)fd);
    return MYMO_NIL;
}

MyMoObject *serverModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"listen",        srv_listen},
        {"accept",        srv_accept},
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
