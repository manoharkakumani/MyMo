#include "function.h"
#include "nil.h"
#include "../vm.h"

MyMoBuiltInFunction *newBuiltInFunction(MVM *vm, MyMoString *name, BuiltInfunction function, MyMoObjectType type)
{
    MyMoBuiltInFunction *builtInFunction = AllocateObject(vm, MyMoBuiltInFunction, type);
    builtInFunction->name = name;
    builtInFunction->function = function;
    return builtInFunction;
}

MyMoFunction *newFunction(MVM *vm)
{
    MyMoFunction *function = AllocateObject(vm, MyMoFunction, OBJ_FUNCTION);
    function->name = NULL;
    function->argc = 0;
    function->argv = New(MyMoString *, 256);
    memset(function->argv, 0, sizeof(MyMoString *) * 256);
    function->variables = newDict(vm);
    function->assiginedParameters = newDict(vm);
    function->chunk = newChunk(vm);
    function->frame = NULL;
    function->isargs = false;
    function->kwonly = 0;
    function->block = false;
    function->bound = NULL;
    function->klass = AS_OBJECT(vm->builtInClasses[OBJ_OBJECT]);
    function->proto = NULL;
    function->defaultCount = 0;
    function->defaults = NULL;
    return function;
}

// Shallow per-closure copy: shares the prototype's chunk and parameter
// names; only `frame` (set by the caller) differs between copies.
MyMoFunction *cloneFunction(MVM *vm, MyMoFunction *proto)
{
    MyMoFunction *function = AllocateObject(vm, MyMoFunction, OBJ_FUNCTION);
    memcpy((char *)function + sizeof(MyMoObject), (char *)proto + sizeof(MyMoObject),
           sizeof(MyMoFunction) - sizeof(MyMoObject));
    function->proto = proto->proto ? proto->proto : proto;
    function->bound = NULL;
    if (proto->defaultCount > 0)
    {
        function->defaults = malloc(sizeof(Value) * (size_t)proto->defaultCount);
        memcpy(function->defaults, proto->defaults, sizeof(Value) * (size_t)proto->defaultCount);
    }
    return function;
}

int missingDefaults(MyMoFunction *function, int argc)
{
    if (argc == function->argc)
        return 0;
    if (argc < function->argc && argc >= function->argc - function->defaultCount)
    {
        int fill = function->argc - argc;
        for (int i = function->defaultCount - fill; i < function->defaultCount; i++)
            if (V_IS_EMPTY(function->defaults[i]))
                return -1; // a required keyword-only parameter
        return fill;
    }
    return -1;
}

void arityError(MVM *vm, MyMoFunction *function, int argc)
{
    int required = function->argc - function->defaultCount;
    if (required == function->argc)
        runtimeError(vm, "TypeError: %s() takes %d argument%s but got %d.",
                     function->name->value, function->argc, function->argc == 1 ? "" : "s", argc);
    else
        runtimeError(vm, "TypeError: %s() takes %d to %d arguments but got %d.",
                     function->name->value, required, function->argc, argc);
}

MyMoClouser *newClouser(MVM *vm, MyMoFunction *function)
{
    MyMoClouser *clouser = AllocateObject(vm, MyMoClouser, OBJ_CLOUSER);
    clouser->function = function;
    clouser->variables = newDict(vm);
    return clouser;
}

MyMoBoundMethod *newBoundMethod(MVM *vm, MyMoObject *self, MyMoFunction *method)
{
    MyMoBoundMethod *boundMethod = AllocateObject(vm, MyMoBoundMethod, OBJ_BOUND_METHOD);
    boundMethod->self = self;
    boundMethod->method = method;
    return boundMethod;
}

void defineMethod(MVM *vm, MyMoObjectType type, const char *name, BuiltInfunction function)
{
    MyMoObject *methodName = NEW_STRING(vm, name, strlen(name));
    MyMoObject *method = AS_OBJECT(newBuiltInFunction(vm, AS_STRING(methodName), function, OBJ_BUILTIN_METHOD));
    setEntry(vm, vm->builtInClasses[type]->methods, methodName, method);
}

MyMoObject *methodEnter(MVM *vm, const char *name, uint argc, uint min, uint max)
{
    if (argc < min || argc > max)
    {
        if (min == max)
            runtimeError(vm, "TypeError: %s() takes %u argument%s (%u given)", name, min, min == 1 ? "" : "s", argc);
        else
            runtimeError(vm, "TypeError: %s() takes %u to %u arguments (%u given)", name, min, max, argc);
        return NULL;
    }
    MyMoObject *self = AS_BUILTIN_FUNCTION(peek(vm, argc))->self;
    if (self == NULL)
    {
        runtimeError(vm, "TypeError: %s() can only be applied on instance", name);
        return NULL;
    }
    for (uint i = 0; i < argc; i++)
        pop(vm);
    return self;
}

void printFunction(MyMoFunction *function)
{
    switch (function->type)
    {
    case FN_INIT:
        printf("<init method %s at %p>", function->name->value, function);
        break;
    case FN_METHOD:
        printf("<method %s at %p>", function->name->value, function);
        break;
    case FN_OPERATOR:
        printf("<operator %s at %p>", function->name->value, function);
        break;
    case FN_GEN_METHOD:
    case FN_GENERATOR:
        printf("<generator %s at %p>", function->name->value, function);
        break;
    case FN_FUNCTION:
        printf("<function %s at %p>", function->name->value, function);
        break;
    case FN_ARROWFN:
        printf("<annonymos function at %p>", function);
        break;
    case FN_COMPILED:
        printf("<Script %s>", function->name->value);
        break;
    case FN_SCRIPT:
        printf("<Script at %p>", function);
        break;
    case FN_MODULE:
        printf("<Module at %p>", function);
        break;
    default:
        printf("<unknown function at %p>", function);
        break;
    }
}

void printClouser(MyMoClouser *clouser)
{
    printf("<clouser of function %s at %p>", clouser->function->name->value, clouser);
}

void printBuiltInFunction(MyMoBuiltInFunction *builtInFunction)
{
    printf("<built-in function %s>", builtInFunction->name->value);
}

void printBuiltInMethod(MyMoBuiltInFunction *builtInFunction)
{
    printf("<built-in method %s>", builtInFunction->name->value);
}

void printBoundMethod(MyMoBoundMethod *boundMethod)
{
    printf("<bound method %s.%s at %p>", AS_INSTANCE(boundMethod->self)->klass->name->value, boundMethod->method->name->value, boundMethod);
}