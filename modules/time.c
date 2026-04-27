// modules/time.c — built-in `time` module.
//
// Exports:
//   time.now()         -> double  (Unix epoch seconds, fractional)
//   time.monotonic()   -> double  (monotonic clock; never goes backwards)
//   time.clock()       -> double  (process CPU seconds)
//   time.sleep(secs)   -> nil     (accepts int or double, fractional sleep ok)
//   time.format(t, fmt)-> string  (strftime over a Unix epoch + format)
//   time.CLOCKS_PER_SEC -> int constant

#include "../include/mymo_module.h"
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
#endif

static MyMoObject *time_now(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "time.now", argc, 0)) return MYMO_ERROR;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return mymo_double(vm, (double)ts.tv_sec + ts.tv_nsec / 1e9);
}

static MyMoObject *time_monotonic(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "time.monotonic", argc, 0)) return MYMO_ERROR;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return mymo_double(vm, (double)ts.tv_sec + ts.tv_nsec / 1e9);
}

static MyMoObject *time_clock(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "time.clock", argc, 0)) return MYMO_ERROR;
    return mymo_double(vm, (double)clock() / CLOCKS_PER_SEC);
}

static MyMoObject *time_sleep(MVM *vm, uint argc, MyMoObject *argv[])
{
    double secs;
    if (!mymo_parse(vm, "time.sleep", argc, argv, "d", &secs)) return MYMO_ERROR;
    if (secs < 0)
    {
        runtimeError(vm, "time.sleep(): seconds must be >= 0");
        return MYMO_ERROR;
    }
#ifdef _WIN32
    Sleep((DWORD)(secs * 1000));
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
    return MYMO_NIL;
}

static MyMoObject *time_format(MVM *vm, uint argc, MyMoObject *argv[])
{
    double epoch;
    const char *fmt;
    if (!mymo_parse(vm, "time.format", argc, argv, "ds", &epoch, &fmt))
        return MYMO_ERROR;
    time_t t = (time_t)epoch;
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[256];
    size_t n = strftime(buf, sizeof(buf), fmt, &tm);
    return mymo_strn(vm, buf, (int)n);
}

MyMoObject *timeModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"now",       time_now},
        {"monotonic", time_monotonic},
        {"clock",     time_clock},
        {"sleep",     time_sleep},
        {"format",    time_format},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "time", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    MyMoObject *mod = defineBuiltInModule(vm, &def);
    mymo_set_int(vm, mod, "CLOCKS_PER_SEC", (long)CLOCKS_PER_SEC);
    return mod;
}
