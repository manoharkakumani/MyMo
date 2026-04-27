// modules/socket.c — built-in `socket` module: blocking TCP/IPv4 client
// + server primitives. POSIX-only today (Windows path stubbed out).
//
// Exports:
//   socket.connect(host, port)        -> int (fd)
//   socket.listen(host, port, backlog)-> int (fd)
//   socket.accept(fd)                 -> int (client fd)
//   socket.send(fd, data)             -> int (bytes sent)
//   socket.recv(fd, max_bytes)        -> string (may be shorter; "" on EOF)
//   socket.close(fd)                  -> nil
//   socket.gethostname()              -> string
//   socket.resolve(host)              -> string (first IPv4 address)

#include "../include/mymo_module.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  #define close_fd(fd) closesocket(fd)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #define close_fd(fd) close(fd)
#endif

#include <string.h>
#include <errno.h>

static int resolve_ipv4(const char *host, struct in_addr *out)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static MyMoObject *sock_connect(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *host;
    long port;
    if (!mymo_parse(vm, "socket.connect", argc, argv, "si", &host, &port))
        return MYMO_ERROR;

    struct in_addr addr;
    if (resolve_ipv4(host, &addr) != 0) {
        runtimeError(vm, "socket.connect(): could not resolve '%s'", host);
        return MYMO_ERROR;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        runtimeError(vm, "socket.connect(): socket() failed: %s", strerror(errno));
        return MYMO_ERROR;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr   = addr;
    sa.sin_port   = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close_fd(fd);
        runtimeError(vm, "socket.connect(): connect() failed: %s", strerror(errno));
        return MYMO_ERROR;
    }
    return mymo_int(vm, fd);
}

static MyMoObject *sock_listen(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *host;
    long port, backlog;
    if (!mymo_parse(vm, "socket.listen", argc, argv, "sii",
                    &host, &port, &backlog))
        return MYMO_ERROR;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        runtimeError(vm, "socket.listen(): socket() failed: %s", strerror(errno));
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
        runtimeError(vm, "socket.listen(): invalid bind address '%s'", host);
        return MYMO_ERROR;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close_fd(fd);
        runtimeError(vm, "socket.listen(): bind() failed: %s", strerror(errno));
        return MYMO_ERROR;
    }
    if (listen(fd, (int)backlog) != 0) {
        close_fd(fd);
        runtimeError(vm, "socket.listen(): listen() failed: %s", strerror(errno));
        return MYMO_ERROR;
    }
    return mymo_int(vm, fd);
}

static MyMoObject *sock_accept(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd;
    if (!mymo_parse(vm, "socket.accept", argc, argv, "i", &fd))
        return MYMO_ERROR;
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int cfd = accept((int)fd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        runtimeError(vm, "socket.accept(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    return mymo_int(vm, cfd);
}

static MyMoObject *sock_send(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd;
    const char *data; int dlen;
    if (!mymo_parse(vm, "socket.send", argc, argv, "isn", &fd, &data, &dlen))
        return MYMO_ERROR;
    ssize_t n = send((int)fd, data, (size_t)dlen, 0);
    if (n < 0) {
        runtimeError(vm, "socket.send(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    return mymo_int(vm, (long)n);
}

static MyMoObject *sock_recv(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd, max_bytes;
    if (!mymo_parse(vm, "socket.recv", argc, argv, "ii", &fd, &max_bytes))
        return MYMO_ERROR;
    if (max_bytes <= 0 || max_bytes > (1 << 22)) {
        runtimeError(vm, "socket.recv(): max_bytes must be 1..4194304");
        return MYMO_ERROR;
    }
    char *buf = malloc((size_t)max_bytes);
    if (!buf) {
        runtimeError(vm, "socket.recv(): out of memory");
        return MYMO_ERROR;
    }
    ssize_t n = recv((int)fd, buf, (size_t)max_bytes, 0);
    if (n < 0) {
        free(buf);
        runtimeError(vm, "socket.recv(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    MyMoObject *s = mymo_strn(vm, buf, (int)n);
    free(buf);
    return s;
}

static MyMoObject *sock_close(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd;
    if (!mymo_parse(vm, "socket.close", argc, argv, "i", &fd))
        return MYMO_ERROR;
    close_fd((int)fd);
    return MYMO_NIL;
}

static MyMoObject *sock_gethostname(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "socket.gethostname", argc, 0)) return MYMO_ERROR;
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) {
        runtimeError(vm, "socket.gethostname(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    buf[sizeof(buf) - 1] = '\0';
    return mymo_str(vm, buf);
}

static MyMoObject *sock_resolve(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *host;
    if (!mymo_parse(vm, "socket.resolve", argc, argv, "s", &host))
        return MYMO_ERROR;
    struct in_addr addr;
    if (resolve_ipv4(host, &addr) != 0) {
        runtimeError(vm, "socket.resolve(): could not resolve '%s'", host);
        return MYMO_ERROR;
    }
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return mymo_str(vm, buf);
}

MyMoObject *socketModule(MVM *vm)
{
#ifdef _WIN32
    static int wsa_inited = 0;
    if (!wsa_inited) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_inited = 1;
    }
#endif
    static MyMoModuleFunction fns[] = {
        {"connect",     sock_connect},
        {"listen",      sock_listen},
        {"accept",      sock_accept},
        {"send",        sock_send},
        {"recv",        sock_recv},
        {"close",       sock_close},
        {"gethostname", sock_gethostname},
        {"resolve",     sock_resolve},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "socket", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
