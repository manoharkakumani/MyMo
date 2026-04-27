// strings.c — a slightly larger MyMo extension module.
//
// Demonstrates:
//   - mymo_parse with multiple positional args + the 'n' length sidecar
//   - mymo_strn for sub-strings
//   - returning MYMO_TRUE / MYMO_FALSE singletons
//   - MYMO_FN_AS to expose a C function under a different MyMo name
//
// Build (from this directory):
//   cc -shared -fPIC -o stringsmod.dylib strings.c -I../../include   # macOS
//   cc -shared -fPIC -o stringsmod.so    strings.c -I../../include   # Linux
//
// Use:
//   from "strings" use upper, starts_with, repeat
//   print(upper("hello"))             # HELLO
//   print(starts_with("foobar", "foo"))   # true
//   print(repeat("ab", 3))            # ababab

#include "mymo_module.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

// upper(s) — returns an upper-cased copy of s.
static MyMoObject *upper(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *s; int n;
    if (!mymo_parse(vm, "upper", argc, argv, "sn", &s, &n))
        return MYMO_ERROR;

    char buf[1024];
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    for (int i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    return mymo_strn(vm, buf, n);
}

// starts_with(haystack, needle) — bool.
static MyMoObject *starts_with(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *hay, *needle;
    int hlen, nlen;
    if (!mymo_parse(vm, "starts_with", argc, argv, "snsn",
                    &hay, &hlen, &needle, &nlen))
        return MYMO_ERROR;

    if (nlen > hlen) return MYMO_FALSE;
    return memcmp(hay, needle, (size_t)nlen) == 0 ? MYMO_TRUE : MYMO_FALSE;
}

// repeat(s, n) — string `s` repeated `n` times.
static MyMoObject *repeat(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *s; int slen; long n;
    if (!mymo_parse(vm, "repeat", argc, argv, "sni", &s, &slen, &n))
        return MYMO_ERROR;

    if (n < 0)
    {
        runtimeError(vm, "repeat(): count must be >= 0");
        return MYMO_ERROR;
    }

    long total = slen * n;
    if (total > 1 << 20)  // 1 MiB cap — stop runaway scripts cheaply
    {
        runtimeError(vm, "repeat(): result would exceed 1 MiB");
        return MYMO_ERROR;
    }

    char *buf = malloc((size_t)total + 1);
    if (!buf) { runtimeError(vm, "repeat(): out of memory"); return MYMO_ERROR; }

    for (long i = 0; i < n; i++) memcpy(buf + i * slen, s, (size_t)slen);
    MyMoObject *result = mymo_strn(vm, buf, (int)total);
    free(buf);
    return result;
}

// length(s) — returns the byte length of a string.
// Exposed under the name "len" via MYMO_FN_AS to demonstrate aliasing.
static MyMoObject *string_length(MVM *vm, uint argc, MyMoObject *argv[])
{
    MyMoString *s;
    if (!mymo_parse(vm, "len", argc, argv, "S", &s))
        return MYMO_ERROR;
    return mymo_int(vm, s->length);
}

MYMO_MODULE(strings,
    MYMO_FN(upper),
    MYMO_FN(starts_with),
    MYMO_FN(repeat),
    MYMO_FN_AS("len", string_length),
)
