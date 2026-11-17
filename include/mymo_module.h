// mymo_module.h — public C API for writing MyMo extension modules.
//
// Two layers in this header:
//
//   1. The LOW-LEVEL surface (defineBuiltInModule, raw struct access,
//      MODULE() macro). Use it when you need full control.
//
//   2. The HIGH-LEVEL ergonomic helpers (mymo_int, mymo_str, mymo_strf,
//      mymo_parse, ...). Use these for typical functions — the raw
//      surface is rarely worth the boilerplate.
//
// Quick start (see examples/ext/hello.c for a complete example):
//
//     #include "mymo_module.h"
//
//     static Value greet(MVM *vm, uint argc, Value argv[]) {
//         const char *name;
//         if (!mymo_parse(vm, "greet", argc, argv, "s", &name))
//             return MYMO_ERROR;
//         return mymo_strf(vm, "hello, %s!", name);
//     }
//
//     MYMO_MODULE("hello", greet)            // helpers below
//
// Build (from the directory holding your .c file, with mymo's source
// tree at $MYMO_SRC):
//
//     macOS: cc -shared -fPIC -undefined dynamic_lookup
//                -I$MYMO_SRC -I$MYMO_SRC/include
//                -o myextmod.dylib myext.c
//     Linux: cc -shared -fPIC
//                -I$MYMO_SRC -I$MYMO_SRC/include
//                -o myextmod.so myext.c
//
// With an installed MyMo (install.sh), mymo-config supplies the flags:
//     cc $(mymo-config --cflags) $(mymo-config --ext-ldflags)
//        -o $(mymo-config --moddir)/myextmod.$(mymo-config --ext) myext.c
//
// Filename MUST be `<name>mod.<ext>`. Search path at runtime:
//   $MYMO_HOME/lib/<name>mod.<ext>
//   ./<name>mod.<ext>
//   ./modules/<name>mod.<ext>
//   /opt/mymo/lib/<name>mod.<ext>
//   bare-name via OS loader

#ifndef MYMO_MODULE_H
#define MYMO_MODULE_H

#include "../common.h"
#include "../datatypes/datatypes.h"
#include "../datatypes/module.h"
#include "../datatypes/function.h"
#include "../vm.h"

// ---------------------------------------------------------------------
// SENTINELS / SINGLETONS
// ---------------------------------------------------------------------

// Returned from a function that has already raised an error via
// runtimeError(). The VM checks V_IS_EMPTY() and propagates the failure.
// Builtins return a Value: wrap object results with objectToValue(), or
// use these inline singletons / the MYMO_BOOL helper directly.
#define MYMO_ERROR  V_EMPTY_VAL
#define MYMO_NIL    V_NIL_VAL
#define MYMO_TRUE   V_TRUE_VAL
#define MYMO_FALSE  V_FALSE_VAL
#define MYMO_BOOL(b) V_BOOL_VAL(b)

// ---------------------------------------------------------------------
// TYPE PREDICATES — `v` is a Value (builtins receive `Value argv[]`)
// ---------------------------------------------------------------------

#define mymo_is_int(v)    valueLooksLikeInt(v)
#define mymo_is_double(v) valueLooksLikeDouble(v)
#define mymo_is_number(v) valueLooksLikeNumber(v)
#define mymo_is_bool(v)   valueIsBool(v)
#define mymo_is_nil(v)    valueIsNil(v)
#define mymo_is_string(v) V_IS_OBJ_TYPE(v, OBJ_STRING)
#define mymo_is_list(v)   V_IS_OBJ_TYPE(v, OBJ_LIST)
#define mymo_is_tuple(v)  V_IS_OBJ_TYPE(v, OBJ_TUPLE)
#define mymo_is_dict(v)   V_IS_OBJ_TYPE(v, OBJ_DICT)

// ---------------------------------------------------------------------
// UNCHECKED ACCESSORS — pair with predicates above
// ---------------------------------------------------------------------

#define mymo_as_int(v)    valueToLong(v)
#define mymo_as_double(v) valueToDouble(v)
#define mymo_as_bool(v)   valueAsBool(v)
#define mymo_as_string(v) (((MyMoString *)V_AS_OBJ(v))->value)
#define mymo_as_strlen(v) (((MyMoString *)V_AS_OBJ(v))->length)
#define mymo_as_object(v) V_AS_OBJ(v)  // list/tuple/dict/... pointer
// mymo_as_number coerces ints to double — useful when you want one
// uniform numeric path.
#define mymo_as_number(v) valueAsNumber(v)

// ---------------------------------------------------------------------
// CONSTRUCTORS — implemented in mymo_api.c, exported by the main binary
// ---------------------------------------------------------------------

MyMoObject *mymo_int(MVM *vm, long n);
MyMoObject *mymo_double(MVM *vm, double d);
MyMoObject *mymo_str(MVM *vm, const char *s);                      // strlen() inside
MyMoObject *mymo_strn(MVM *vm, const char *s, int len);
MyMoObject *mymo_strf(MVM *vm, const char *fmt, ...);              // printf-style; max 4096 bytes

// ---------------------------------------------------------------------
// ARGUMENT PARSING
// ---------------------------------------------------------------------
//
// mymo_check_args(vm, "name", argc, expected)
//     Verifies argc == expected. Raises runtimeError and returns false
//     if not. Use as a one-line guard.
//
// mymo_parse(vm, "name", argc, argv, fmt, ...)
//     Parses positional args according to format chars and stores into
//     out parameters. Returns false (with runtimeError already raised)
//     on type mismatch or arity mismatch.
//
//     Format characters (one per arg, in order):
//         i — long *out          (integer; accepts OBJ_INT)
//         d — double *out        (double; accepts OBJ_DOUBLE or OBJ_INT — ints widen)
//         s — const char **out   (string contents; OBJ_STRING)
//         n — int *out           (string length, paired with s; advances past)
//         b — int *out           (bool)
//         v — Value *out         (any value, unconverted — for polymorphic args)
//         o — MyMoObject **out   (any value as an object; inline ints/doubles boxed)
//         S — MyMoString **out
//         L — MyMoList **out
//         T — MyMoTuple **out
//
//     Example: parse two ints then an optional string into a length-out:
//         long a, b;  const char *s;  int slen;
//         if (!mymo_parse(vm, "do_thing", argc, argv, "iisn",
//                         &a, &b, &s, &slen)) return MYMO_ERROR;

bool mymo_check_args(MVM *vm, const char *funcname, uint argc, uint expected);
bool mymo_parse(MVM *vm, const char *funcname, uint argc, Value argv[],
                const char *fmt, ...);

// ---------------------------------------------------------------------
// EXPORTING CONSTANTS / VARIABLES
// ---------------------------------------------------------------------
//
// Module variables can't live in a static initializer because their
// values (`mymo_int`, `mymo_str`, ...) need a live `vm` to allocate.
// Instead, register them at module-init time with the helpers below.
//
//   mymo_set(vm, module, "name", value)   // any MyMoObject*
//   mymo_set_int(vm, module, "PI_x100", 314)
//   mymo_set_double(vm, module, "PI", 3.14159)
//   mymo_set_str(vm, module, "VERSION", "1.0.0")
//
// `module` is the MyMoObject* returned by defineBuiltInModule (or
// available as `_mod` inside MYMO_MODULE_EX). MyMo treats these as
// regular module attributes — `from "mymath" use PI` works, and so
// does `import "mymath"; print(mymath.PI)` (when import lands).
//
// MyMo has no const qualifier on module attrs today, so "constant" is
// a convention — `from "mymath" use PI; PI = 0` would shadow PI in the
// caller's scope without affecting the module. Capitalise constants
// (`PI`, `MAX`, `VERSION`) by convention.

void mymo_set       (MVM *vm, MyMoObject *module, const char *name, MyMoObject *value);
void mymo_set_int   (MVM *vm, MyMoObject *module, const char *name, long n);
void mymo_set_double(MVM *vm, MyMoObject *module, const char *name, double d);
void mymo_set_str   (MVM *vm, MyMoObject *module, const char *name, const char *s);

// ---------------------------------------------------------------------
// MODULE DECLARATION SUGAR
// ---------------------------------------------------------------------
//
// MYMO_MODULE(name_str, fn1, fn2, ...) — emits the boilerplate entry-
// point and tables for a module that exports up to 16 functions. The
// function names exposed to MyMo are the C identifier names; rename
// with MYMO_FN_AS("alias", fn) inside the list if needed.
//
// MYMO_MODULE_EX(name, INIT_BLOCK, fn1, fn2, ...) — same as above but
// runs INIT_BLOCK after the module is registered, with `vm` and
// `_mod` (MyMoObject*) in scope. Use it to register constants /
// variables via mymo_set / mymo_set_{int,double,str}.
//
//     MYMO_MODULE_EX(mymath,
//         {
//             mymo_set_double(vm, _mod, "PI", 3.14159265358979);
//             mymo_set_double(vm, _mod, "E",  2.71828182845905);
//             mymo_set_str   (vm, _mod, "VERSION", "1.0.0");
//         },
//         MYMO_FN(area),
//         MYMO_FN(circumference),
//     )
//
// Use the longer hand-rolled form (filling MyMoModuleFunction[] and
// calling defineBuiltInModule directly) when you need computed names
// or more than 16 functions.

#define MYMO_FN(fn)            { #fn, fn }
#define MYMO_FN_AS(name, fn)   { name, fn }

#define MYMO_MODULE(NAME, ...)                                              \
    MODULE(NAME)                                                             \
    {                                                                        \
        static MyMoModuleFunction _fns[] = { __VA_ARGS__ };                  \
        static MyMoModuleVariable _vars[] = { {0,0} };                       \
        static MyMoModuleDef _def = {                                        \
            .name = #NAME,                                                   \
            .functions = _fns,                                               \
            .variables = _vars,                                              \
            .totalfn   = sizeof(_fns) / sizeof(_fns[0]),                     \
            .totalvar  = 0,                                                  \
        };                                                                   \
        return defineBuiltInModule(vm, &_def);                               \
    }

#define MYMO_MODULE_EX(NAME, INIT, ...)                                     \
    MODULE(NAME)                                                             \
    {                                                                        \
        static MyMoModuleFunction _fns[] = { __VA_ARGS__ };                  \
        static MyMoModuleVariable _vars[] = { {0,0} };                       \
        static MyMoModuleDef _def = {                                        \
            .name = #NAME,                                                   \
            .functions = _fns,                                               \
            .variables = _vars,                                              \
            .totalfn   = sizeof(_fns) / sizeof(_fns[0]),                     \
            .totalvar  = 0,                                                  \
        };                                                                   \
        MyMoObject *_mod = defineBuiltInModule(vm, &_def);                   \
        do INIT while (0);                                                   \
        return _mod;                                                         \
    }

#endif
