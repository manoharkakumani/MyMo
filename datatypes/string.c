#include "../memory.h"
#include "../vm.h"
#include "string.h"
#include "dict.h"
#include "nil.h"
#include "function.h"

MyMoString *newString(MVM *vm, const char *chars, int length)
{
    char *heapChars = Allocate(vm, char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    u32 hash = hasher(heapChars, length);
    MyMoString *interned = findString(&vm->strings, heapChars, length, hash);
    if (interned != NULL)
    {
        Free(vm, char, heapChars);
        return interned;
    }
    MyMoString *string = AllocateObject(vm, MyMoString, OBJ_STRING);
    string->object.hash = hash;
    string->length = length;
    string->value = heapChars;
    setPrimitive(vm, &vm->strings, (MyMoObject *)string);
    return string;
}
void printString(MyMoString *string)
{
    printf("%s", string->value);
}

Value newStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "str() takes  1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    else if (argc == 0)
    {
        return objectToValue(NEW_STRING(vm, "", 0));
    }
    else
    {
        if (IS_STRING(args[0]))
        {
            return objectToValue(pop(vm));
        }
        else if (IS_INT(args[0]))
        {
            char x[1000];
            sprintf(x, "%ld", INT_VAL(pop(vm)));
            return objectToValue(NEW_STRING(vm, x, strlen(x)));
        }
        else if (IS_DOUBLE(args[0]))
        {
            char x[1000];
            sprintf(x, "%g", DOUBLE_VAL(pop(vm)));
            return objectToValue(NEW_STRING(vm, x, strlen(x)));
        }
        else
        {
            char *a = getType(pop(vm));
            return objectToValue(NEW_STRING(vm, a, strlen(a)));
        }
    }
}

Value stringLengthMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: __len__() takes 0 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, 0));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: __len__() can only be applied on instance");
        return V_EMPTY_VAL;
    }
        
    return objectToValue(NEW_INT(vm, AS_STRING(function->self)->length));
}
void defineStringMethods(MVM *vm)
{
    defineMethod(vm, OBJ_STRING, "__new__", newStringMethod);
    defineMethod(vm, OBJ_STRING, "__len__", stringLengthMethod);
}

void defineStringClass(MVM *vm)
{
    MyMoString *name = newString(vm, "str", 3);
    MyMoBuiltInClass *stringClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_STRING] = stringClass;
    defineStringMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(stringClass));
}