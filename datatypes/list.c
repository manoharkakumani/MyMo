#include "list.h"
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

    if (argc == 0)
    {
        return objectToValue(AS_OBJECT(newList(vm)));
    }
    else if (argc > 1)
    {
        MyMoList *list = newList(vm);
        for (int i = 0; i < argc; i++)
        {
            writeValueArray(vm, &list->values, args[i]);
        }
        for (int i = 0; i < argc; i++)
        {
            pop(vm);
        }
        return objectToValue(AS_OBJECT(list));
    }
    else
    {
        MyMoObject *object = pop(vm);
        if (IS_LIST(object))
        {
            MyMoList *copyList = newList(vm);
            MyMoList *list = AS_LIST(object);
            for (int i = 0; i < list->values.count; i++)
            {
                writeValueArray(vm, &copyList->values, list->values.values[i]);
            }
            return objectToValue(AS_OBJECT(copyList));
        }
        else if (IS_TUPLE(object))
        {
            MyMoTuple *tuple = AS_TUPLE(object);
            MyMoList *list = newList(vm);
            for (int i = 0; i < tuple->values.count; i++)
            {
                writeValueArray(vm, &list->values, tuple->values.values[i]);
            }
            return objectToValue(AS_OBJECT(list));
        }
        else if (IS_STRING(object))
        {
            MyMoString *string = AS_STRING(object);
            MyMoList *list = newList(vm);
            for (int i = 0; i < string->length; i++)
            {
                writeValueArrayObject(vm, &list->values, NEW_STRING(vm, &string->value[i], 1));
            }
            return objectToValue(AS_OBJECT(list));
        }
        else
        {
            MyMoList *list = newList(vm);
            writeValueArrayObject(vm, &list->values, object);
            return objectToValue(AS_OBJECT(list));
        }
    }
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
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: append() takes exactly one argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, 1));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: append() can only be applied on instance");
        return V_EMPTY_VAL;
    }
    MyMoList *list = AS_LIST(function->self);
    writeValueArrayObject(vm, &list->values, pop(vm));
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

Value indexListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "index", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    int i = findInArray(&AS_LIST(self)->values, args[0]);
    if (i < 0)
    {
        runtimeError(vm, "ValueError: list.index(x): x not in list");
        return V_EMPTY_VAL;
    }
    return V_INT_VAL(i);
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
    if (!V_IS_OBJ_TYPE(args[0], OBJ_LIST) && !V_IS_OBJ_TYPE(args[0], OBJ_TUPLE))
    {
        runtimeError(vm, "TypeError: extend() expects a list or tuple, got %s", valueTypeName(args[0]));
        return V_EMPTY_VAL;
    }
    MyMoObject *seq = V_AS_OBJ(args[0]);
    ValueArray *src = IS_LIST(seq) ? &AS_LIST(seq)->values : &AS_TUPLE(seq)->values;
    int n = src->count; // snapshot: `xs.extend(xs)` must not loop forever
    for (int i = 0; i < n; i++)
        writeValueArray(vm, &AS_LIST(self)->values, src->values[i]);
    return V_NIL_VAL;
}

// sort(): numbers (int/double mixed) or strings, ascending. Insertion
// sort keeps it stable and lets a type mismatch abort cleanly.
static bool isStringValue(Value v)
{
    return V_IS_OBJ(v) && V_AS_OBJ(v)->type == OBJ_STRING;
}

static int compareForSort(Value a, Value b, bool *ok)
{
    if (valueLooksLikeNumber(a) && valueLooksLikeNumber(b))
    {
        double x = valueAsNumber(a), y = valueAsNumber(b);
        return (x > y) - (x < y);
    }
    if (isStringValue(a) && isStringValue(b))
        return compareStrings(AS_STRING(V_AS_OBJ(a)), AS_STRING(V_AS_OBJ(b)));
    *ok = false;
    return 0;
}

Value sortListMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "sort", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    ValueArray *values = &AS_LIST(self)->values;
    bool ok = true;
    for (int i = 1; i < values->count && ok; i++)
    {
        Value key = values->values[i];
        int j = i - 1;
        while (j >= 0 && compareForSort(values->values[j], key, &ok) > 0 && ok)
        {
            values->values[j + 1] = values->values[j];
            j--;
        }
        values->values[j + 1] = key;
    }
    if (!ok)
    {
        runtimeError(vm, "TypeError: sort() needs all numbers or all strings");
        return V_EMPTY_VAL;
    }
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
    defineMethod(vm, OBJ_LIST, "index", indexListMethod);
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