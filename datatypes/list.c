#include "list.h"
#include "../builtins.h"
#include "tuple.h"
#include "nil.h"
#include "../memory.h"
#include "../vm.h"
#include "datatypes.h"
#include "../operations.h"

MyMoList *newList(MVM *vm)
{
    MyMoList *list = AllocateObject(vm, MyMoList, OBJ_LIST);
    initValueArray(vm, &list->values);
    return list;
}

void printList(MyMoList *list)
{
    printf("[");
    for (int i = 0; i < list->values.count; i++)
    {
        Value v = list->values.values[i];
        if (V_IS_OBJ(v) && V_AS_OBJ(v) == AS_OBJECT(list))
            printf("[...]");
        else
            printValue(v);
        if (i != list->values.count - 1)
            printf(", ");
    }
    printf("]");
}

Value newListMethod(MVM *vm, uint argc, Value args[])
{
    // list() -> [], list(iterable) -> its elements, list(a, b, ...) -> [a, b, ...]
    MyMoList *list = newList(vm);
    if (argc == 1)
    {
        if (!appendIterable(vm, "list", args[0], &list->values))
            return V_EMPTY_VAL;
    }
    else
    {
        for (uint i = 0; i < argc; i++)
            writeValueArray(vm, &list->values, args[i]);
    }
    for (uint i = 0; i < argc; i++)
        popV(vm);
    return objectToValue(AS_OBJECT(list));
}

Value lenListMethod(MVM *vm, uint argc, Value args[])
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
    return objectToValue(NEW_INT(vm, AS_LIST(function->self)->values.count));
}

Value appendListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "append", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    writeValueArray(vm, &AS_LIST(self)->values, args[0]);
    return V_NIL_VAL;
}

Value copyListMethod(MVM *vm, uint argc, Value args[])
{
    if (argc)
    {
        runtimeError(vm, "TypeError: copy() takes zero argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoList *copyList = newList(vm);
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, 0));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: copy() can only be applied on instance");
        return V_EMPTY_VAL;
    }
    MyMoList *list = AS_LIST(function->self);
    for (int i = 0; i < list->values.count; i++)
    {
        writeValueArray(vm, &copyList->values, list->values.values[i]);
    }
    return objectToValue(AS_OBJECT(copyList));
}

Value addListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoList *resultList = newList(vm);
    if (!(IS_LIST(peek(vm,0))) || !(IS_LIST(peek(vm,1))))
    {
        runtimeError(vm, "TypeError: can't perform + between  %s and %s", getType(peek(vm,1)), getType(peek(vm,0)));
        return V_EMPTY_VAL;
    }
    MyMoList *bList = AS_LIST(pop(vm));
    MyMoList *aList = AS_LIST(pop(vm));
    
    for (int i = 0; i < aList->values.count; i++)
    {
        writeValueArray(vm, &resultList->values, aList->values.values[i]);
    }
    for (int i = 0; i < bList->values.count; i++)
    {
        writeValueArray(vm, &resultList->values, bList->values.values[i]);
    }
    return objectToValue(AS_OBJECT(resultList));
}

// Resolve a possibly-negative index against `count`; -1 if out of range.
static int listIndex(Value index, int count)
{
    long i = valueToLong(index);
    if (i < 0)
        i += count;
    return (i < 0 || i >= count) ? -1 : (int)i;
}

static int findInArray(ValueArray *array, Value target)
{
    for (int i = 0; i < array->count; i++)
        if (valuesEqual(array->values[i], target))
            return i;
    return -1;
}

// The elements of a list or tuple method's receiver.
static ValueArray *sequenceSelf(MyMoObject *self)
{
    return self->type == OBJ_LIST ? &AS_LIST(self)->values : &AS_TUPLE(self)->values;
}

// Clamp a Python-style slice bound (negative counts from the end).
static int clampBound(long i, int count)
{
    if (i < 0)
        i += count;
    if (i < 0)
        return 0;
    return i > count ? count : (int)i;
}

// xs.index(x[, start[, end]]) for lists and tuples.
Value sequenceIndexMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "index", argc, 1, 3);
    if (!self) return V_EMPTY_VAL;
    ValueArray *values = sequenceSelf(self);
    long bounds[2] = {0, values->count};
    for (uint i = 1; i < argc; i++)
    {
        if (!valueLooksLikeInt(args[i]))
        {
            runtimeError(vm, "TypeError: index() bounds must be integers, not %s", valueTypeName(args[i]));
            return V_EMPTY_VAL;
        }
        bounds[i - 1] = valueToLong(args[i]);
    }
    int end = clampBound(bounds[1], values->count);
    for (int i = clampBound(bounds[0], values->count); i < end; i++)
        if (valuesEqual(values->values[i], args[0]))
            return V_INT_VAL(i);
    runtimeError(vm, "ValueError: %s.index(x): x not in %s", self->type == OBJ_LIST ? "list" : "tuple",
                 self->type == OBJ_LIST ? "list" : "tuple");
    return V_EMPTY_VAL;
}

// xs.count(x) for lists and tuples.
Value sequenceCountMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "count", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    ValueArray *values = sequenceSelf(self);
    int n = 0;
    for (int i = 0; i < values->count; i++)
        if (valuesEqual(values->values[i], args[0]))
            n++;
    return V_INT_VAL(n);
}

Value popListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "pop", argc, 0, 1);
    if (!self) return V_EMPTY_VAL;
    ValueArray *values = &AS_LIST(self)->values;
    if (values->count == 0)
    {
        runtimeError(vm, "IndexError: pop from empty list");
        return V_EMPTY_VAL;
    }
    int i = values->count - 1;
    if (argc)
    {
        if (!valueLooksLikeInt(args[0]) || (i = listIndex(args[0], values->count)) < 0)
        {
            runtimeError(vm, "IndexError: pop index out of range");
            return V_EMPTY_VAL;
        }
    }
    Value item = values->values[i];
    memmove(&values->values[i], &values->values[i + 1], sizeof(Value) * (values->count - i - 1));
    values->count--;
    return item;
}

Value insertListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "insert", argc, 2, 2);
    if (!self) return V_EMPTY_VAL;
    if (!valueLooksLikeInt(args[0]))
    {
        runtimeError(vm, "TypeError: insert() index must be an int");
        return V_EMPTY_VAL;
    }
    ValueArray *values = &AS_LIST(self)->values;
    long i = valueToLong(args[0]);
    if (i < 0) i += values->count;
    if (i < 0) i = 0;
    if (i > values->count) i = values->count;
    Value item = args[1];
    writeValueArray(vm, values, item); // grow by one
    memmove(&values->values[i + 1], &values->values[i], sizeof(Value) * (values->count - 1 - i));
    values->values[i] = item;
    return V_NIL_VAL;
}

Value removeListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "remove", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    ValueArray *values = &AS_LIST(self)->values;
    int i = findInArray(values, args[0]);
    if (i < 0)
    {
        runtimeError(vm, "ValueError: list.remove(x): x not in list");
        return V_EMPTY_VAL;
    }
    memmove(&values->values[i], &values->values[i + 1], sizeof(Value) * (values->count - i - 1));
    values->count--;
    return V_NIL_VAL;
}

Value containsListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "contains", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    return V_BOOL_VAL(findInArray(&AS_LIST(self)->values, args[0]) >= 0);
}

Value reverseListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "reverse", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    ValueArray *values = &AS_LIST(self)->values;
    for (int i = 0, j = values->count - 1; i < j; i++, j--)
    {
        Value tmp = values->values[i];
        values->values[i] = values->values[j];
        values->values[j] = tmp;
    }
    return V_NIL_VAL;
}

Value clearListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "clear", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    AS_LIST(self)->values.count = 0;
    return V_NIL_VAL;
}

Value extendListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "extend", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    if (!appendIterable(vm, "extend", args[0], &AS_LIST(self)->values))
        return V_EMPTY_VAL;
    return V_NIL_VAL;
}

// xs.sort([key[, reverse]]) — stable; key may be Nil.
Value sortListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "sort", argc, 0, 2);
    if (!self) return V_EMPTY_VAL;
    Value key = argc > 0 ? args[0] : V_NIL_VAL;
    bool reverse = argc > 1 && !valueIsFalsey(args[1]);
    if (!sortValues(vm, &AS_LIST(self)->values, key, reverse))
        return V_EMPTY_VAL;
    return V_NIL_VAL;
}

void defineListMethods(MVM *vm)
{
    defineMethod(vm, OBJ_LIST, "__new__", newListMethod);
    defineMethod(vm, OBJ_LIST, "__len__", lenListMethod);
    defineMethod(vm, OBJ_LIST, "+", addListMethod);
    defineMethod(vm, OBJ_LIST, "remove", removeListMethod);
    defineMethod(vm, OBJ_LIST, "append", appendListMethod);
    defineMethod(vm, OBJ_LIST, "copy", copyListMethod);
    defineMethod(vm, OBJ_LIST, "insert", insertListMethod);
    defineMethod(vm, OBJ_LIST, "extend", extendListMethod);
    defineMethod(vm, OBJ_LIST, "clear", clearListMethod);
    defineMethod(vm, OBJ_LIST, "contains", containsListMethod);
    defineMethod(vm, OBJ_LIST, "index", sequenceIndexMethod);
    defineMethod(vm, OBJ_LIST, "count", sequenceCountMethod);
    defineMethod(vm, OBJ_LIST, "pop", popListMethod);
    defineMethod(vm, OBJ_LIST, "reverse", reverseListMethod);
    defineMethod(vm, OBJ_LIST, "sort", sortListMethod);
}

void defineListClass(MVM *vm)
{
    MyMoString *name = newString(vm, "list", 4);
    MyMoBuiltInClass *listClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_LIST] = listClass;
    defineListMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(listClass));
}