#include "tuple.h"
#include "../memory.h"
#include "../vm.h"
#include "class.h"
#include "function.h"
#include "int.h"

MyMoTuple *newTuple(MVM *vm)
{
    MyMoTuple *tuple = AllocateObject(vm, MyMoTuple, OBJ_TUPLE);
    initValueArray(vm, &tuple->values);
    return tuple;
}

void printTuple(MyMoTuple *tuple)
{
    printf("(");
    for (int i = 0; i < tuple->values.count; i++)
    {
        printValue(tuple->values.values[i]);
        if (i != tuple->values.count - 1)
            printf(", ");
    }
    printf(tuple->values.count == 1 ? ",)" : ")");
}

Value lenTupleMethod(MVM *vm, uint argc, Value args[])
{
    if (argc)
    {
        runtimeError(vm, "TypeError: __len__() takes no arguments (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, 0));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: __len__() can only be applied on instance");
        return V_EMPTY_VAL;
    }
    return V_INT_VAL(AS_TUPLE(function->self)->values.count);
}

// Method table only: no `tuple` builtin name is registered, since there
// is no `__new__` to construct one (tuples come from literals).
void defineTupleClass(MVM *vm)
{
    MyMoString *name = newString(vm, "tuple", 5);
    vm->builtInClasses[OBJ_TUPLE] = newBuiltInClass(vm, name);
    defineMethod(vm, OBJ_TUPLE, "__len__", lenTupleMethod);
}
