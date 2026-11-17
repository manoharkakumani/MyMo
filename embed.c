// embed.c — implementation of the embedding API in include/mymo.h.

#include "include/mymo.h"
#include "compiler.h"
#include "utils.h"
#include "gc.h"
#include "datatypes/datatypes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void defineBuiltInFunction(MVM *vm, const char *name, BuiltInfunction function);
MyMoFunction *runFile(MVM *vm, char *path);

MVM *mymo_new(void)
{
    return initVM();
}

void mymo_free(MVM *vm)
{
    freeVM(vm);
}

const char *mymo_last_error(MVM *vm)
{
    return vm->lastError;
}

static MyMoResult toResult(I_Result r)
{
    return r == OK ? MYMO_OK : r == COMPILE_ERROR ? MYMO_COMPILE_ERROR : MYMO_RUNTIME_ERROR;
}

// Scripts run in the root frame, which a running script occupies.
static bool canRunScript(MVM *vm)
{
    if (vm->runDepth > 0)
    {
        snprintf(vm->lastError, sizeof(vm->lastError),
                 "mymo_run_*: the VM is already running code; use mymo_call from inside a builtin");
        return false;
    }
    return true;
}

static void ensureMainModule(MVM *vm, const char *name)
{
    if (vm->currentModule == NULL)
        vm->currentModule = newModule(vm, newString(vm, "__main__", 8),
                                      newString(vm, name, (int)strlen(name)));
}

MyMoResult mymo_run_string(MVM *vm, const char *source, const char *name)
{
    if (!canRunScript(vm))
        return MYMO_RUNTIME_ERROR;
    ensureMainModule(vm, name);
    MyMoFunction *function = compile(vm, source, name, COMPILE_SCRIPT);
    if (function == NULL)
    {
        snprintf(vm->lastError, sizeof(vm->lastError), "compile error in %s", name);
        return MYMO_COMPILE_ERROR;
    }
    return toResult(interpreter(vm, function));
}

MyMoResult mymo_run_file(MVM *vm, const char *path)
{
    if (!canRunScript(vm))
        return MYMO_RUNTIME_ERROR;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        snprintf(vm->lastError, sizeof(vm->lastError), "could not open '%s'", path);
        return MYMO_RUNTIME_ERROR;
    }
    fclose(f);
    ensureMainModule(vm, path);
    char *owned = strdup(path);
    MyMoFunction *function = runFile(vm, owned); // compiles or loads the .myc cache
    free(owned);
    if (function == NULL)
    {
        snprintf(vm->lastError, sizeof(vm->lastError), "compile error in %s", path);
        return MYMO_COMPILE_ERROR;
    }
    return toResult(interpreter(vm, function));
}

MyMoResult mymo_call(MVM *vm, Value callable, int argc, const Value *argv, Value *result)
{
    if (!V_IS_OBJ(callable))
    {
        snprintf(vm->lastError, sizeof(vm->lastError), "mymo_call: value is not callable");
        return MYMO_RUNTIME_ERROR;
    }
    MyMoFiber *fiber = vm->fiber;
    int baseStack = fiber->stack.count;
    uint baseFrames = fiber->frameCount;

    // Same stack shape as OP_CALL: [callee, args...].
    pushV(vm, callable);
    for (int i = 0; i < argc; i++)
        pushV(vm, argv[i]);

    int savedExitFrame = vm->exitFrame;
    MyMoFiber *savedExitFiber = vm->exitFiber;
    // The callee must not see keyword arguments meant for the builtin
    // that is calling back into MyMo.
    MyMoDict *savedKwargs = vm->kwargs;
    vm->kwargs = NULL;
    vm->exitFrame = (int)baseFrames;
    vm->exitFiber = fiber;

    I_Result r = caller(vm, V_AS_OBJ(callable), (u32)argc) ? OK : RUNTIME_ERROR;
    // A MyMo function pushed a frame: run it until that frame returns
    // (OP_FRET checks exitFrame). Builtins already left their result.
    if (r == OK && vm->fiber == fiber && fiber->frameCount > baseFrames)
    {
        vm->runDepth++;
        r = (I_Result)runMVM(vm);
        vm->runDepth--;
    }

    vm->exitFrame = savedExitFrame;
    vm->exitFiber = savedExitFiber;
    vm->kwargs = savedKwargs;

    if (r == OK && vm->fiber == fiber && fiber->stack.count == baseStack + 1)
    {
        Value out = popV(vm);
        if (result)
            *result = out;
        return MYMO_OK;
    }
    if (r == OK)
        snprintf(vm->lastError, sizeof(vm->lastError),
                 "mymo_call: the call switched fibers (yield across a host call is not supported)");
    // Error: discard whatever the failed call left behind. Keep the
    // in-flight exception (or the message) so that if a builtin returns
    // MYMO_ERROR because of this, an enclosing MyMo `try` receives it.
    MyMoObject *exc = vm->fiber->exception;
    vm->fiber = fiber;
    unwindFrames(vm, baseFrames);
    fiber->stack.count = baseStack;
    fiber->exception = exc ? exc : AS_OBJECT(newString(vm, vm->lastError, (int)strlen(vm->lastError)));
    return MYMO_RUNTIME_ERROR;
}

static MyMoObject *internName(MVM *vm, const char *name)
{
    return AS_OBJECT(newString(vm, name, (int)strlen(name)));
}

bool mymo_get_global(MVM *vm, const char *name, Value *out)
{
    MyMoObject *key = internName(vm, name);
    if (getEntryV(&vm->globals, key, out))
        return true;
    return getEntryV(&vm->builtins, key, out);
}

void mymo_set_global(MVM *vm, const char *name, Value value)
{
    setEntryV(vm, &vm->globals, internName(vm, name), value);
}

void mymo_define_function(MVM *vm, const char *name, BuiltInfunction fn)
{
    defineBuiltInFunction(vm, name, fn);
}

void mymo_retain(MVM *vm, Value value)
{
    if (V_IS_OBJ(value))
        writeValueArray(vm, &vm->hostRoots, value);
}

void mymo_release(MVM *vm, Value value)
{
    ValueArray *roots = &vm->hostRoots;
    for (int i = roots->count - 1; i >= 0; i--)
        if (roots->values[i] == value)
        {
            roots->values[i] = roots->values[--roots->count];
            return;
        }
}

// ---- values ---------------------------------------------------------------

Value mymo_string(MVM *vm, const char *s)
{
    return V_OBJ_VAL(AS_OBJECT(newString(vm, s, (int)strlen(s))));
}

Value mymo_list(MVM *vm)
{
    return V_OBJ_VAL(AS_OBJECT(newList(vm)));
}

void mymo_list_append(MVM *vm, Value list, Value item)
{
    if (mymo_val_is_list(list))
        writeValueArrayObject(vm, &AS_LIST(V_AS_OBJ(list))->values, valueToBoxedObject(vm, item));
}

Value mymo_dict(MVM *vm)
{
    return V_OBJ_VAL(AS_OBJECT(newDict(vm)));
}

void mymo_dict_set(MVM *vm, Value dict, const char *key, Value value)
{
    if (mymo_val_is_dict(dict))
        setEntryV(vm, AS_DICT(V_AS_OBJ(dict)), internName(vm, key), value);
}

static bool isObjType(Value v, MyMoObjectType type)
{
    return V_IS_OBJ(v) && V_AS_OBJ(v)->type == type;
}

bool mymo_val_is_nil(Value v)    { return V_IS_NIL(v) || isObjType(v, OBJ_NIL); }
bool mymo_val_is_bool(Value v)   { return V_IS_BOOL(v) || isObjType(v, OBJ_BOOL); }
bool mymo_val_is_number(Value v) { return valueLooksLikeNumber(v); }
bool mymo_val_is_string(Value v) { return isObjType(v, OBJ_STRING); }
bool mymo_val_is_list(Value v)   { return isObjType(v, OBJ_LIST); }
bool mymo_val_is_dict(Value v)   { return isObjType(v, OBJ_DICT); }

bool mymo_val_is_callable(Value v)
{
    if (!V_IS_OBJ(v))
        return false;
    switch (V_AS_OBJ(v)->type)
    {
    case OBJ_FUNCTION:
    case OBJ_BUILTIN_FUNCTION:
    case OBJ_BUILTIN_METHOD:
    case OBJ_BOUND_METHOD:
    case OBJ_CLASS:
    case OBJ_BUILTIN_CLASS:
        return true;
    default:
        return false;
    }
}

bool mymo_val_truthy(Value v)
{
    if (mymo_val_is_nil(v))
        return false;
    if (V_IS_BOOL(v))
        return V_IS_TRUE(v);
    if (isObjType(v, OBJ_BOOL))
        return AS_BOOL(V_AS_OBJ(v))->value;
    if (valueLooksLikeNumber(v))
        return valueAsNumber(v) != 0;
    return true;
}

bool mymo_val_as_long(Value v, long *out)
{
    if (!valueLooksLikeInt(v))
        return false;
    *out = valueToLong(v);
    return true;
}

bool mymo_val_as_double(Value v, double *out)
{
    if (!valueLooksLikeNumber(v))
        return false;
    *out = valueAsNumber(v);
    return true;
}

const char *mymo_val_cstring(Value v)
{
    return mymo_val_is_string(v) ? AS_STRING(V_AS_OBJ(v))->value : NULL;
}

int mymo_val_list_length(Value list)
{
    return mymo_val_is_list(list) ? AS_LIST(V_AS_OBJ(list))->values.count : -1;
}

Value mymo_val_list_get(Value list, int index)
{
    if (!mymo_val_is_list(list))
        return V_NIL_VAL;
    ValueArray *values = &AS_LIST(V_AS_OBJ(list))->values;
    if (index < 0 || index >= values->count)
        return V_NIL_VAL;
    return values->values[index];
}

bool mymo_val_dict_get(MVM *vm, Value dict, const char *key, Value *out)
{
    if (!mymo_val_is_dict(dict))
        return false;
    return getEntryV(AS_DICT(V_AS_OBJ(dict)), internName(vm, key), out);
}

void mymo_print(Value v)
{
    printValue(v);
}
