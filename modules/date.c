// modules/date.c — built-in `date` module: calendar arithmetic over
// Unix epoch seconds (the `time` module gives you the epoch; `date`
// breaks it into pieces and formats it).
//
// Exports:
//   date.year(epoch)       -> int   (4-digit year, e.g. 2026)
//   date.month(epoch)      -> int   (1..12)
//   date.day(epoch)        -> int   (1..31, day of month)
//   date.hour(epoch)       -> int   (0..23, local time)
//   date.minute(epoch)     -> int
//   date.second(epoch)     -> int
//   date.weekday(epoch)    -> int   (0=Sunday .. 6=Saturday)
//   date.yearday(epoch)    -> int   (1..366, day of year)
//   date.iso(epoch)        -> string  ("2026-04-27T14:33:09Z" — UTC)
//   date.parse(s, fmt)     -> int   (epoch; uses strptime)
//   date.make(y,m,d,H,M,S) -> int   (build epoch from components, local TZ)

#include "../include/mymo_module.h"
#include <time.h>

static struct tm to_local(time_t t)
{
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

static struct tm to_utc(time_t t)
{
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

#define COMPONENT_FN(NAME, EXPR)                                            \
    static MyMoObject *date_##NAME(MVM *vm, uint argc, MyMoObject *argv[])  \
    {                                                                       \
        double epoch;                                                       \
        if (!mymo_parse(vm, "date." #NAME, argc, argv, "d", &epoch))        \
            return MYMO_ERROR;                                              \
        struct tm tm = to_local((time_t)epoch);                             \
        return mymo_int(vm, (long)(EXPR));                                  \
    }

COMPONENT_FN(year,    tm.tm_year + 1900)
COMPONENT_FN(month,   tm.tm_mon + 1)
COMPONENT_FN(day,     tm.tm_mday)
COMPONENT_FN(hour,    tm.tm_hour)
COMPONENT_FN(minute,  tm.tm_min)
COMPONENT_FN(second,  tm.tm_sec)
COMPONENT_FN(weekday, tm.tm_wday)
COMPONENT_FN(yearday, tm.tm_yday + 1)

#undef COMPONENT_FN

static MyMoObject *date_iso(MVM *vm, uint argc, MyMoObject *argv[])
{
    double epoch;
    if (!mymo_parse(vm, "date.iso", argc, argv, "d", &epoch)) return MYMO_ERROR;
    struct tm tm = to_utc((time_t)epoch);
    char buf[32];
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return mymo_strn(vm, buf, (int)n);
}

static MyMoObject *date_parse(MVM *vm, uint argc, MyMoObject *argv[])
{
#ifdef _WIN32
    runtimeError(vm, "date.parse(): not implemented on Windows");
    return MYMO_ERROR;
#else
    const char *s, *fmt;
    if (!mymo_parse(vm, "date.parse", argc, argv, "ss", &s, &fmt))
        return MYMO_ERROR;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    extern char *strptime(const char *, const char *, struct tm *);
    if (!strptime(s, fmt, &tm))
    {
        runtimeError(vm, "date.parse(): could not parse '%s' with '%s'", s, fmt);
        return MYMO_ERROR;
    }
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
    {
        runtimeError(vm, "date.parse(): mktime failed");
        return MYMO_ERROR;
    }
    return mymo_int(vm, (long)t);
#endif
}

static MyMoObject *date_make(MVM *vm, uint argc, MyMoObject *argv[])
{
    long y, mo, d, h, mi, s;
    if (!mymo_parse(vm, "date.make", argc, argv, "iiiiii",
                    &y, &mo, &d, &h, &mi, &s))
        return MYMO_ERROR;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year  = (int)y - 1900;
    tm.tm_mon   = (int)mo - 1;
    tm.tm_mday  = (int)d;
    tm.tm_hour  = (int)h;
    tm.tm_min   = (int)mi;
    tm.tm_sec   = (int)s;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
    {
        runtimeError(vm, "date.make(): invalid date");
        return MYMO_ERROR;
    }
    return mymo_int(vm, (long)t);
}

MyMoObject *dateModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"year",    date_year},
        {"month",   date_month},
        {"day",     date_day},
        {"hour",    date_hour},
        {"minute",  date_minute},
        {"second",  date_second},
        {"weekday", date_weekday},
        {"yearday", date_yearday},
        {"iso",     date_iso},
        {"parse",   date_parse},
        {"make",    date_make},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "date", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
