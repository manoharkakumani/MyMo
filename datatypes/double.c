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
    number->length = 0;
    number->object.hash = hash;
    setPrimitive(vm, &vm->doubles, (MyMoObject *)number);
    return number;
}

void printDouble(MyMoDouble *number)
{
    printf("%.16g", number->value);
}

Value newDoubleMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "double() takes  1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    else if (argc == 0)
    {
        return objectToValue(NEW_DOUBLE(vm, 0));
    }
    else
    {
        if (IS_DOUBLE(args[0]))
        {
            return objectToValue(pop(vm));
        }
        else if (IS_INT(args[0]))
        {
            return objectToValue(NEW_DOUBLE(vm, (double)INT_VAL(pop(vm))));
        }
        else if (IS_STRING(args[0]))
        {
            char *str = STRING_VAL(pop(vm));
            char *end;
            double value = strtod(str, &end);
            if (*end != '\0')
            {
                runtimeError(vm, "invalid literal for double(): '%s'", str);
                return V_EMPTY_VAL;
            }
            return objectToValue(NEW_DOUBLE(vm, value));
        }
        else
        {
            runtimeError(vm, "invalid literal for double(): '%s'", getType(args[0]));
            return V_EMPTY_VAL;
        }
    }
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