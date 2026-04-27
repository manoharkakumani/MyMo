// modules/random.c — built-in `random` module.
//
// Exports:
//   random.seed(n)          -> nil   (deterministic; pass time.now() for variability)
//   random.int(lo, hi)      -> int   (inclusive range)
//   random.float()          -> double in [0.0, 1.0)
//   random.choice(list)     -> any   (one element)
//
// Backed by xorshift64* — small, fast, decent quality. Not cryptographic.

#include "../include/mymo_module.h"
#include <time.h>

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static MyMoObject *random_seed(MVM *vm, uint argc, MyMoObject *argv[])
{
    long n;
    if (!mymo_parse(vm, "random.seed", argc, argv, "i", &n)) return MYMO_ERROR;
    rng_state = (uint64_t)n;
    if (rng_state == 0) rng_state = 0x9e3779b97f4a7c15ULL;
    return MYMO_NIL;
}

static MyMoObject *random_int(MVM *vm, uint argc, MyMoObject *argv[])
{
    long lo, hi;
    if (!mymo_parse(vm, "random.int", argc, argv, "ii", &lo, &hi)) return MYMO_ERROR;
    if (lo > hi) {
        runtimeError(vm, "random.int(): lo (%ld) > hi (%ld)", lo, hi);
        return MYMO_ERROR;
    }
    uint64_t span = (uint64_t)(hi - lo) + 1;
    return mymo_int(vm, lo + (long)(rng_next() % span));
}

static MyMoObject *random_float(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "random.float", argc, 0)) return MYMO_ERROR;
    // Top 53 bits → uniform double in [0,1).
    return mymo_double(vm, (double)(rng_next() >> 11) / (double)(1ULL << 53));
}

static MyMoObject *random_choice(MVM *vm, uint argc, MyMoObject *argv[])
{
    MyMoList *list;
    if (!mymo_parse(vm, "random.choice", argc, argv, "L", &list)) return MYMO_ERROR;
    if (list->values.count == 0) {
        runtimeError(vm, "random.choice(): list is empty");
        return MYMO_ERROR;
    }
    return list->values.objects[rng_next() % (uint64_t)list->values.count];
}

MyMoObject *randomModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"seed",   random_seed},
        {"int",    random_int},
        {"float",  random_float},
        {"choice", random_choice},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "random", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    // Seed from process start so unseeded calls aren't perfectly deterministic
    // across runs. Caller can override with random.seed(42) for tests.
    rng_state = (uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ULL;
    if (rng_state == 0) rng_state = 1;
    return defineBuiltInModule(vm, &def);
}
