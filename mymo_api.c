// mymo_api.c — implementation of the high-level helpers declared in
// include/mymo_module.h. These wrap the low-level constructors and add
// argument-parsing/error-raising boilerplate so external module authors
// can write a typical builtin in one line of validation.

#include "include/mymo_module.h"
#include "stack.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --------------------------------------------------------------------
// Constructors
// --------------------------------------------------------------------

MyMoObject *mymo_int(MVM *vm, long n)
{
    return NEW_INT(vm, n);
}

MyMoObject *mymo_double(MVM *vm, double d)
{
    return NEW_DOUBLE(vm, d);
}

MyMoObject *mymo_str(MVM *vm, const char *s)
{
    return NEW_STRING(vm, s, (int)strlen(s));
}

MyMoObject *mymo_strn(MVM *vm, const char *s, int len)
{
    return NEW_STRING(vm, s, len);
}

MyMoObject *mymo_strf(MVM *vm, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    return NEW_STRING(vm, buf, n);
}

// --------------------------------------------------------------------
// Argument parsing
// --------------------------------------------------------------------

bool mymo_check_args(MVM *vm, const char *funcname, uint argc, uint expected)
{
    if (argc != expected)
    {
        runtimeError(vm, "%s() takes exactly %u argument%s, got %u",
                     funcname, expected, expected == 1 ? "" : "s", argc);
        return false;
    }
    // The VM's builtin dispatch leaves argc items on the operand stack and
    // expects the builtin to pop them. mymo_parse / mymo_check_args do that
    // here so individual handlers don't have to.
    for (uint i = 0; i < argc; i++) pop(vm);
    return true;
}

// Format chars (one per positional arg, in order):
//   i  long *out          int                       (OBJ_INT)
//   d  double *out        double or int (widens)    (OBJ_DOUBLE / OBJ_INT)
//   s  const char **out   string contents           (OBJ_STRING)
//   n  int *out           string length, paired with s and consumed AFTER s
//   b  int *out           bool                      (OBJ_BOOL)
//   o  MyMoObject **out   any object (no type check)
//   S  MyMoString **out
//   L  MyMoList **out
//   T  MyMoTuple **out
bool mymo_parse(MVM *vm, const char *funcname, uint argc, Value argv[],
                const char *fmt, ...)
{
    // First pass: count expected args (skip 'n' — it pairs with 's',
    // doesn't consume a positional arg of its own).
    uint expected = 0;
    for (const char *p = fmt; *p; p++)
        if (*p != 'n') expected++;

    if (argc != expected)
    {
        runtimeError(vm, "%s() takes %u argument%s, got %u",
                     funcname, expected, expected == 1 ? "" : "s", argc);
        return false;
    }

    va_list ap;
    va_start(ap, fmt);

#define ARG_ERROR(what)                                                          \
    do {                                                                         \
        va_end(ap);                                                              \
        runtimeError(vm, "%s(): argument %u must be " what, funcname, i + 1);    \
        return false;                                                            \
    } while (0)

    uint i = 0;
    for (const char *p = fmt; *p; p++)
    {
        char c = *p;

        // 'n' is a piggyback on the previous string arg — consume an
        // int* and assign the length of argv[i-1].
        if (c == 'n')
        {
            int *out = va_arg(ap, int *);
            if (i == 0 || !V_IS_OBJ_TYPE(argv[i - 1], OBJ_STRING))
            {
                va_end(ap);
                runtimeError(vm, "%s(): 'n' format must follow 's'", funcname);
                return false;
            }
            *out = AS_STRING(V_AS_OBJ(argv[i - 1]))->length;
            continue;
        }

        Value arg = argv[i];

        switch (c)
        {
        case 'i':
        {
            long *out = va_arg(ap, long *);
            if (!valueLooksLikeInt(arg)) ARG_ERROR("int");
            *out = valueToLong(arg);
            break;
        }
        case 'd':
        {
            double *out = va_arg(ap, double *);
            if (!valueLooksLikeNumber(arg)) ARG_ERROR("number");
            *out = valueAsNumber(arg);
            break;
        }
        case 's':
        {
            const char **out = va_arg(ap, const char **);
            if (!V_IS_OBJ_TYPE(arg, OBJ_STRING)) ARG_ERROR("string");
            *out = AS_STRING(V_AS_OBJ(arg))->value;
            break;
        }
        case 'b':
        {
            int *out = va_arg(ap, int *);
            if (!valueIsBool(arg)) ARG_ERROR("bool");
            *out = valueAsBool(arg) ? 1 : 0;
            break;
        }
        case 'v':
        {
            Value *out = va_arg(ap, Value *);
            *out = arg;
            break;
        }
        case 'o':
        {
            // Legacy: any value as an object (inline ints/doubles are boxed).
            MyMoObject **out = va_arg(ap, MyMoObject **);
            *out = valueToBoxedObject(vm, arg);
            break;
        }
        case 'S':
        {
            MyMoString **out = va_arg(ap, MyMoString **);
            if (!V_IS_OBJ_TYPE(arg, OBJ_STRING)) ARG_ERROR("string");
            *out = AS_STRING(V_AS_OBJ(arg));
            break;
        }
        case 'L':
        {
            MyMoList **out = va_arg(ap, MyMoList **);
            if (!V_IS_OBJ_TYPE(arg, OBJ_LIST)) ARG_ERROR("list");
            *out = AS_LIST(V_AS_OBJ(arg));
            break;
        }
        case 'T':
        {
            MyMoTuple **out = va_arg(ap, MyMoTuple **);
            if (!V_IS_OBJ_TYPE(arg, OBJ_TUPLE)) ARG_ERROR("tuple");
            *out = AS_TUPLE(V_AS_OBJ(arg));
            break;
        }
        default:
            va_end(ap);
            runtimeError(vm, "%s(): unknown format char '%c'", funcname, c);
            return false;
        }

        i++;
    }
#undef ARG_ERROR

    va_end(ap);
    // VM dispatch expects builtins to pop their argc operands. Do that
    // centrally so individual mymo_parse-based handlers don't have to.
    for (uint k = 0; k < argc; k++) popV(vm);
    return true;
}

// --------------------------------------------------------------------
// Module-variable helpers
// --------------------------------------------------------------------
//
// All of these write into `module->variables`, the same dict that holds
// exported functions. The dict is keyed by interned MyMoString — using
// newString() (which goes through vm->strings) is what makes lookup at
// `from "X" use NAME` cheap.

void mymo_set(MVM *vm, MyMoObject *module, const char *name, MyMoObject *value)
{
    MyMoModule *m = AS_MODULE(module);
    setEntry(vm, m->variables,
             AS_OBJECT(newString(vm, name, (int)strlen(name))),
             value);
}

void mymo_set_int(MVM *vm, MyMoObject *module, const char *name, long n)
{
    mymo_set(vm, module, name, mymo_int(vm, n));
}

void mymo_set_double(MVM *vm, MyMoObject *module, const char *name, double d)
{
    mymo_set(vm, module, name, mymo_double(vm, d));
}

void mymo_set_str(MVM *vm, MyMoObject *module, const char *name, const char *s)
{
    mymo_set(vm, module, name, mymo_str(vm, s));
}
