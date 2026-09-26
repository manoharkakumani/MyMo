#include "int.h"
#include "../memory.h"
#include "../vm.h"
#include "nil.h"

MyMoInt *newInt(MVM *vm, long number)
{
    char x[1000];
    sprintf(x, "%ld", number);
    size_t length = strlen(x);
    u32 hash = hasher(x, length);
    MyMoInt *interned = findInt(&vm->integers, number, length, hash);
    if (interned != NULL)
    {
        return interned;
    }
    MyMoInt *longNumber = AllocateObject(vm, MyMoInt, OBJ_INT);
    longNumber->value = number;
    longNumber->object.hash = hash;
    longNumber->length = length;
    setPrimitive(vm, &vm->integers, (MyMoObject *)longNumber);
    return longNumber;
}

void printInt(MyMoInt *number)
{
    printf("%ld", number->value);
}

Value newIntMethod(MVM *vm, uint argc, Value args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: int() takes 1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    if (argc == 0)
        return V_INT_VAL(0);
    Value v = args[0];
    popV(vm);
    if (valueLooksLikeInt(v))
        return v;
    if (valueLooksLikeDouble(v))
        return valueFromLong(vm, (long)valueToDouble(v)); // truncates, like Python
    if (valueIsBool(v))
        return V_INT_VAL(valueAsBool(v) ? 1 : 0);
    if (V_IS_OBJ_TYPE(v, OBJ_STRING))
    {
        char *str = AS_STRING(V_AS_OBJ(v))->value;
        char *end;
        long value = strtol(str, &end, 10);
        if (*str == '\0' || *end != '\0')
        {
            runtimeError(vm, "ValueError: invalid literal for int(): '%s'", str);
            return V_EMPTY_VAL;
        }
        return valueFromLong(vm, value);
    }
    runtimeError(vm, "TypeError: int() can't convert %s", valueTypeName(v));
    return V_EMPTY_VAL;
}
void defineIntMethods(MVM *vm)
{
    defineMethod(vm, OBJ_INT, "__new__", newIntMethod);
}

void defineIntClass(MVM *vm)
{
    MyMoString *name = newString(vm, "int", 3);
    MyMoBuiltInClass *intClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_INT] = intClass;
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(intClass));
    defineIntMethods(vm);
}