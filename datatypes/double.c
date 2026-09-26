#include "double.h"
#include "../memory.h"
#include "../vm.h"
#include "nil.h"

MyMoDouble *newDouble(MVM *vm, double value)
{
    char x[1000];
    sprintf(x, "%g", value);
    size_t length = strlen(x);
    u32 hash = hasher(x, length);
    MyMoDouble *interned = findDouble(&vm->doubles, value, length, hash);
    if (interned != NULL)
    {
        return interned;
    }
    MyMoDouble *number = AllocateObject(vm, MyMoDouble, OBJ_DOUBLE);
    number->object.type = OBJ_DOUBLE;
    number->value = value;
    number->length = length;
    number->object.hash = hash;
    setPrimitive(vm, &vm->doubles, (MyMoObject *)number);
    return number;
}

void printDouble(MyMoDouble *number)
{
    printf("%.16g", number->value);
}

Value newDoubleMethod(MVM *vm, uint argc, Value args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "double() takes  1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    if (argc == 0)
        return V_DOUBLE_VAL(0);
    Value v = args[0];
    popV(vm);
    if (valueLooksLikeNumber(v))
        return V_DOUBLE_VAL(valueAsNumber(v));
    if (valueIsBool(v))
        return V_DOUBLE_VAL(valueAsBool(v) ? 1 : 0);
    if (V_IS_OBJ_TYPE(v, OBJ_STRING))
    {
        char *str = AS_STRING(V_AS_OBJ(v))->value;
        char *end;
        double value = strtod(str, &end);
        if (*str == '\0' || *end != '\0')
        {
            runtimeError(vm, "invalid literal for double(): '%s'", str);
            return V_EMPTY_VAL;
        }
        return V_DOUBLE_VAL(value);
    }
    runtimeError(vm, "double() can't convert %s", valueTypeName(v));
    return V_EMPTY_VAL;
}

void defineDoubleMethods(MVM *vm)
{
    defineMethod(vm, OBJ_DOUBLE, "__new__", newDoubleMethod);
}

void defineDoubleClass(MVM *vm)
{
    MyMoString *name = newString(vm, "double", 6);
    MyMoBuiltInClass *doubleClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_DOUBLE] = doubleClass;
    defineDoubleMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(doubleClass));
    // `float` is the familiar name for the same type.
    setEntry(vm, &vm->builtins, AS_OBJECT(newString(vm, "float", 5)), AS_OBJECT(doubleClass));
}