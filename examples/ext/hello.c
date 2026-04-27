// hello.c — minimal MyMo extension module using the high-level C API.
//
// This file demonstrates the ERGONOMIC path: no manual `argv[i]->type ==
// OBJ_STRING` checks, no MODULE() boilerplate. Compare against
// examples/ext/hello_raw.c (same module written against the raw API)
// to see what mymo_parse / MYMO_MODULE save you.
//
// Build (from this directory, with the mymo source tree at ../..):
//   cc -shared -fPIC -o hellomod.dylib hello.c -I../../include    # macOS
//   cc -shared -fPIC -o hellomod.so    hello.c -I../../include    # Linux
//
// Use:
//   $ MYMO_HOME=. ./mymo  -e 'from "hello" use greet, square; print(greet("world"));'

#include "mymo_module.h"

// greet(name) — returns "hello, <name>!" as a MyMo string.
static MyMoObject *greet(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *name;
    if (!mymo_parse(vm, "greet", argc, argv, "s", &name))
        return MYMO_ERROR;
    return mymo_strf(vm, "hello, %s!", name);
}

// square(n) — returns n * n.
static MyMoObject *square(MVM *vm, uint argc, MyMoObject *argv[])
{
    long n;
    if (!mymo_parse(vm, "square", argc, argv, "i", &n))
        return MYMO_ERROR;
    return mymo_int(vm, n * n);
}

// add(a, b) — accepts ints or doubles; returns a number.
static MyMoObject *add(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "add", argc, 2)) return MYMO_ERROR;
    if (!mymo_is_number(argv[0]) || !mymo_is_number(argv[1]))
    {
        runtimeError(vm, "add(): arguments must be numbers");
        return MYMO_ERROR;
    }
    // If both are ints, return int; otherwise widen to double.
    if (mymo_is_int(argv[0]) && mymo_is_int(argv[1]))
        return mymo_int(vm, mymo_as_int(argv[0]) + mymo_as_int(argv[1]));
    return mymo_double(vm, mymo_as_number(argv[0]) + mymo_as_number(argv[1]));
}

// One-line module declaration. The exported MyMo names default to the
// C identifiers; use MYMO_FN_AS("alias", fn) inside the list to rename.
MYMO_MODULE(hello,
    MYMO_FN(greet),
    MYMO_FN(square),
    MYMO_FN(add),
)
