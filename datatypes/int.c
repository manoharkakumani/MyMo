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

Value newIntMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "int() takes  1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    else if (argc == 0)
    {
        return objectToValue(NEW_INT(vm, 0));
    }
    else
    {
        if (IS_INT(args[0]))
        {
            return objectToValue(pop(vm));
        }
        else if (IS_DOUBLE(args[0]))
        {
            return objectToValue(NEW_INT(vm, (long)DOUBLE_VAL(pop(vm))));
        }
        else if (IS_STRING(args[0]))
        {
            char *str = STRING_VAL(pop(vm));
            char *end;
            long value = strtol(str, &end, 10);
            if (*end != '\0')
            {
                runtimeError(vm, "invalid literal for int(): '%s'", str);
                return V_EMPTY_VAL;
            }
            return objectToValue(NEW_INT(vm, value));
        }
        else
        {
            char *a = getType(pop(vm));
            runtimeError(vm, "invalid literal for int(): '%s'", a);
            return V_EMPTY_VAL;
        }
    }
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