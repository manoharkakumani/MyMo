// modules/runloop.c — built-in `runloop` module: non-blocking I/O
// primitives.
//
// Phase 1 of the non-blocking story (see REDESIGN.md). Provides the
// raw kernel-event-source wrappers that user code (or higher-level
// modules in Phase 2) builds cooperative I/O on top of.
//
// Backed by kqueue on macOS/BSD and epoll on Linux. The kqueue/epoll
// fd is created lazily on first use and held in a single static
// slot, so all calls share one kernel queue. Adequate for the
// single-threaded VM today; will need per-fiber loops if/when MyMo
// gets real parallelism.
//
// Exports:
//   runloop.nonblock(fd)              -> nil
//   runloop.readable(fd, timeout_ms)  -> bool  (True if readable, False on timeout)
//   runloop.writable(fd, timeout_ms)  -> bool  (True if writable, False on timeout)
//   runloop.select(fds, timeout_ms)   -> int   (index of ready fd, or -1 on timeout)
//
// `timeout_ms` is in milliseconds. -1 means "wait forever" (blocking
// but only on this fd); 0 means "poll, return immediately".

#include "../include/mymo_module.h"
#include "../datatypes/list.h"
#include "../datatypes/int.h"
#include "../datatypes/bool.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  #define USE_KQUEUE 1
  #include <sys/event.h>
  #include <sys/time.h>
#elif defined(__linux__)
  #define USE_EPOLL 1
  #include <sys/epoll.h>
#else
  // Windows / WSL-without-epoll / other: fall back to select.
  #include <sys/select.h>
  #include <sys/time.h>
#endif

#if USE_KQUEUE || USE_EPOLL
static int g_queue_fd = -1;

static int ensure_queue(MVM *vm)
{
    if (g_queue_fd >= 0) return 0;
#if USE_KQUEUE
    g_queue_fd = kqueue();
#else
    g_queue_fd = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (g_queue_fd < 0)
    {
        runtimeError(vm, "runloop: cannot create event queue: %s", strerror(errno));
        return -1;
    }
    return 0;
}
#endif

static MyMoObject *rl_nonblock(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd;
    if (!mymo_parse(vm, "runloop.nonblock", argc, argv, "i", &fd))
        return MYMO_ERROR;
    int flags = fcntl((int)fd, F_GETFL, 0);
    if (flags < 0)
    {
        runtimeError(vm, "runloop.nonblock(): fcntl GETFL: %s", strerror(errno));
        return MYMO_ERROR;
    }
    if (fcntl((int)fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        runtimeError(vm, "runloop.nonblock(): fcntl SETFL: %s", strerror(errno));
        return MYMO_ERROR;
    }
    return MYMO_NIL;
}

// Block until `fd` has either readable or writable events (depending
// on `want`), or `timeout_ms` elapses. Returns 1 ready, 0 timeout,
// -1 error.
static int wait_one(int fd, int want_read, long timeout_ms)
{
#if USE_KQUEUE
    if (g_queue_fd < 0) return -1;
    // Use EV_DISPATCH instead of EV_ONESHOT: dispatch disables the
    // event after firing but keeps the registration, so subsequent
    // calls just re-arm it without paying a full ADD. Level-trigger
    // semantics — important so a pending accept on a listen socket
    // gets reported even if it landed between two kevent calls.
    struct kevent change;
    EV_SET(&change, fd, want_read ? EVFILT_READ : EVFILT_WRITE,
           EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, NULL);
    struct kevent event;
    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0)
    {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        tsp = &ts;
    }
    int n = kevent(g_queue_fd, &change, 1, &event, 1, tsp);
    if (n < 0) return -1;
    return n > 0 ? 1 : 0;

#elif USE_EPOLL
    if (g_queue_fd < 0) return -1;
    struct epoll_event ev;
    ev.events = (want_read ? EPOLLIN : EPOLLOUT) | EPOLLONESHOT;
    ev.data.fd = fd;
    if (epoll_ctl(g_queue_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        // ADD fails with EEXIST after the first call on the same fd
        // (because the ONESHOT didn't clear the registration when
        // the event didn't fire). Fall through to MOD which always
        // succeeds.
        if (errno != EEXIST) return -1;
        if (epoll_ctl(g_queue_fd, EPOLL_CTL_MOD, fd, &ev) < 0) return -1;
    }
    struct epoll_event ready;
    int n = epoll_wait(g_queue_fd, &ready, 1, (int)timeout_ms);
    if (n < 0) return -1;
    return n > 0 ? 1 : 0;

#else
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout_ms >= 0)
    {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }
    int n = select(fd + 1,
                   want_read ? &set : NULL,
                   want_read ? NULL : &set,
                   NULL, tvp);
    if (n < 0) return -1;
    return n > 0 ? 1 : 0;
#endif
}

static MyMoObject *rl_readable(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd, timeout_ms;
    if (!mymo_parse(vm, "runloop.readable", argc, argv, "ii",
                    &fd, &timeout_ms))
        return MYMO_ERROR;
#if USE_KQUEUE || USE_EPOLL
    if (ensure_queue(vm) < 0) return MYMO_ERROR;
#endif
    int r = wait_one((int)fd, 1, timeout_ms);
    if (r < 0)
    {
        runtimeError(vm, "runloop.readable(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    return r ? MYMO_TRUE : MYMO_FALSE;
}

static MyMoObject *rl_writable(MVM *vm, uint argc, MyMoObject *argv[])
{
    long fd, timeout_ms;
    if (!mymo_parse(vm, "runloop.writable", argc, argv, "ii",
                    &fd, &timeout_ms))
        return MYMO_ERROR;
#if USE_KQUEUE || USE_EPOLL
    if (ensure_queue(vm) < 0) return MYMO_ERROR;
#endif
    int r = wait_one((int)fd, 0, timeout_ms);
    if (r < 0)
    {
        runtimeError(vm, "runloop.writable(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    return r ? MYMO_TRUE : MYMO_FALSE;
}

// runloop.select([fd1, fd2, ...], timeout_ms) -> int
// Returns the LIST INDEX of the first fd that became readable, or
// -1 on timeout. Uses select(2) for portability — kqueue/epoll could
// be wired in but select is enough for the modest fd counts our
// example programs deal with.
static MyMoObject *rl_select(MVM *vm, uint argc, MyMoObject *argv[])
{
    MyMoList *fds;
    long timeout_ms;
    if (!mymo_parse(vm, "runloop.select", argc, argv, "Li",
                    &fds, &timeout_ms))
        return MYMO_ERROR;
    fd_set rset;
    FD_ZERO(&rset);
    int maxfd = -1;
    for (size_t i = 0; i < (size_t)fds->values.count; i++)
    {
        MyMoObject *o = fds->values.objects[i];
        if (o->type != OBJ_INT)
        {
            runtimeError(vm, "runloop.wait_any(): fds[%zu] must be int", i);
            return MYMO_ERROR;
        }
        int fd = (int)((MyMoInt *)o)->value;
        FD_SET(fd, &rset);
        if (fd > maxfd) maxfd = fd;
    }
    if (maxfd < 0) return mymo_int(vm, -1);
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout_ms >= 0)
    {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }
    int n = select(maxfd + 1, &rset, NULL, NULL, tvp);
    if (n < 0)
    {
        runtimeError(vm, "runloop.wait_any(): select(): %s", strerror(errno));
        return MYMO_ERROR;
    }
    if (n == 0) return mymo_int(vm, -1);
    for (size_t i = 0; i < (size_t)fds->values.count; i++)
    {
        int fd = (int)((MyMoInt *)fds->values.objects[i])->value;
        if (FD_ISSET(fd, &rset)) return mymo_int(vm, (long)i);
    }
    return mymo_int(vm, -1);
}

MyMoObject *runloopModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"nonblock", rl_nonblock},
        {"readable", rl_readable},
        {"writable", rl_writable},
        {"select",   rl_select},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "runloop", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
