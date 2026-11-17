// mymo.h — embed the MyMo interpreter in a C program.
//
//     #include "mymo.h"
//
//     static Value host_add(MVM *vm, uint argc, Value argv[]) {
//         long a, b;
//         if (!mymo_parse(vm, "host_add", argc, argv, "ii", &a, &b))
//             return MYMO_ERROR;
//         return MYMO_INT(a + b);
//     }
//
//     int main(void) {
//         MVM *vm = mymo_new();
//         mymo_define_function(vm, "host_add", host_add);  // callable from MyMo
//         mymo_run_string(vm, "fn greet(n):\n    return f\"hi {n}\"\n", "setup");
//
//         Value greet, out;
//         mymo_get_global(vm, "greet", &greet);
//         Value arg = mymo_string(vm, "ana");
//         if (mymo_call(vm, greet, 1, &arg, &out) == MYMO_OK)
//             printf("%s\n", mymo_val_cstring(out));          // hi ana
//         mymo_free(vm);
//     }
//
// Build: `make lib` produces libmymo.a. Compile with -I<mymo>/include
// -I<mymo> and link libmymo.a -lm -lcurl -lsqlite3. See
// examples/embed/host.c (`make embed-example`).
//
// Values and the garbage collector
// --------------------------------
// A Value is a NaN-boxed 64-bit word: ints, doubles, nil and bools are
// stored inline; strings, lists, dicts, functions, ... are pointers to
// GC-managed objects. The collector only runs while MyMo code executes
// (mymo_run_* / mymo_call). A heap Value the host obtained is safe to use
// until the next such call; to keep one longer (e.g. a callback function
// stored in a C struct), mymo_retain() it and mymo_release() it when done.
// Arguments passed to mymo_call are protected for the duration of the call.
//
// Threading: an MVM is single-threaded. Use one VM per thread.

#ifndef MYMO_H
#define MYMO_H

#include "mymo_module.h" // builtin signature, mymo_parse, MYMO_* constants

typedef enum
{
    MYMO_OK = 0,
    MYMO_COMPILE_ERROR = 1,
    MYMO_RUNTIME_ERROR = 2,
} MyMoResult;

// ---- lifecycle ------------------------------------------------------------

MVM *mymo_new(void);
void mymo_free(MVM *vm);

// ---- running code ---------------------------------------------------------

// Compile and run `source` as a top-level script. `name` is used in error
// messages and to resolve relative `from "x" use ...` imports. Globals
// persist across calls, so a script can define functions the host then
// calls with mymo_call.
MyMoResult mymo_run_string(MVM *vm, const char *source, const char *name);
MyMoResult mymo_run_file(MVM *vm, const char *path);

// Call any callable (function, closure, builtin, bound method, class)
// with `argc` arguments. On MYMO_OK the return value is stored in
// *result (when non-NULL). Safe to call from inside a builtin, e.g. to
// invoke a MyMo callback. A MyMo exception raised inside the call is not
// caught by `try` blocks outside it; it returns MYMO_RUNTIME_ERROR.
MyMoResult mymo_call(MVM *vm, Value callable, int argc, const Value *argv, Value *result);

// Message of the most recent runtime error ("" if none).
const char *mymo_last_error(MVM *vm);

// ---- globals and host functions ------------------------------------------

bool mymo_get_global(MVM *vm, const char *name, Value *out);
void mymo_set_global(MVM *vm, const char *name, Value value);

// Expose a C function to MyMo code as a global builtin `name`.
void mymo_define_function(MVM *vm, const char *name, BuiltInfunction fn);

// ---- keeping values alive -------------------------------------------------

void mymo_retain(MVM *vm, Value value);
void mymo_release(MVM *vm, Value value);

// ---- building and reading values -----------------------------------------

#define MYMO_INT(n)    V_INT_VAL(n)      // 32-bit; use mymo_int for wider
#define MYMO_DOUBLE(d) V_DOUBLE_VAL(d)
// MYMO_NIL / MYMO_TRUE / MYMO_FALSE / MYMO_BOOL(b) come from mymo_module.h.

Value mymo_string(MVM *vm, const char *s);
Value mymo_list(MVM *vm);
void  mymo_list_append(MVM *vm, Value list, Value item);
Value mymo_dict(MVM *vm);
void  mymo_dict_set(MVM *vm, Value dict, const char *key, Value value);

// Readers take Values (the mymo_is_* / mymo_as_* macros in mymo_module.h
// work on MyMoObject* inside builtins; these are the Value versions).
bool mymo_val_is_nil(Value v);
bool mymo_val_is_bool(Value v);
bool mymo_val_is_number(Value v);   // int or double
bool mymo_val_is_string(Value v);
bool mymo_val_is_list(Value v);
bool mymo_val_is_dict(Value v);
bool mymo_val_is_callable(Value v);

bool        mymo_val_truthy(Value v);                  // MyMo truthiness
bool        mymo_val_as_long(Value v, long *out);      // false if not an int
bool        mymo_val_as_double(Value v, double *out);  // ints widen
const char *mymo_val_cstring(Value v);                 // NULL if not a string
int         mymo_val_list_length(Value list);          // -1 if not a list
Value       mymo_val_list_get(Value list, int index);  // MYMO_NIL if out of range
bool        mymo_val_dict_get(MVM *vm, Value dict, const char *key, Value *out);

// Print a value the way MyMo's print() would (no newline).
void mymo_print(Value v);

#endif
