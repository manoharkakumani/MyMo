#include "bool.h"
#include "../memory.h"
#include "class.h"
#include "nil.h"
#include "../vm.h"

MyMoBool *TrueBool;
MyMoBool *FalseBool;

void printBool(MyMoBool *boolean)
{
    if (boolean->value)
    {
        printf("True");
    }
    else
    {
        printf("False");
    }
}

void boolean(MVM *vm)
{
    if (TrueBool != NULL) // process-wide singletons; see nil()
        return;
    FalseBool = AllocateObject(vm, MyMoBool, OBJ_BOOL);
    FalseBool->value = false;
    TrueBool = AllocateObject(vm, MyMoBool, OBJ_BOOL);
    TrueBool->value = true;
}

Value newBoolMethod(MVM *vm, uint argc, Value args[])
{
    // bool(x): x's truthiness (Nil, False, 0, 0.0 and "" are false).
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: bool() takes 1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    if (argc == 0)
        return V_FALSE_VAL;
    Value v = args[0];
    popV(vm);
    return V_BOOL_VAL(!valueIsFalsey(v));
}

void defineBoolMethods(MVM *vm)
{
    defineMethod(vm, OBJ_BOOL, "__new__", newBoolMethod);
}

void defineBoolClass(MVM *vm)
{
    MyMoString *name = newString(vm, "bool", 4);
    MyMoBuiltInClass *boolClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_BOOL] = boolClass;
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(boolClass));
    defineBoolMethods(vm);
}
