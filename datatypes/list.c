#include "list.h"
#include "nil.h"
#include "../memory.h"
#include "../vm.h"
#include "datatypes.h"
#include "../operations.h"

MyMoList *newList(MVM *vm)
{
    MyMoList *list = AllocateObject(vm, MyMoList, OBJ_LIST);
    initMyMoObjectArray(vm, &list->values);
    return list;
}

void printList(MyMoList *list)
{
    printf("[");
    for (int i = 0; i < list->values.count; i++)
    {
        MyMoObject *object = list->values.objects[i];
        if (object == AS_OBJECT(list))
        {
            printf("[...]");
        }
        else
        {
            printObject(object);
        }
        if (i != list->values.count - 1)
            printf(", ");
    }
    printf("]");
}

MyMoObject *getValueByIndex(MyMoList *list, uint index)
{
    if (list->values.count >= index)
    {
        printf("Error");
        return AS_OBJECT(NEW_NIL);
    }
    else
    {
        return list->values.objects[list->values.count - index - 1];
    }
}

Value newListMethod(MVM *vm, uint argc, MyMoObject *args[])
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
            writeMyMoObjectArray(vm, &list->values, args[i]);
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
                writeMyMoObjectArray(vm, &copyList->values, list->values.objects[i]);
            }
            return objectToValue(AS_OBJECT(copyList));
        }
        else if (IS_TUPLE(object))
        {
            MyMoTuple *tuple = AS_TUPLE(object);
            MyMoList *list = newList(vm);
            for (int i = 0; i < tuple->values.count; i++)
            {
                writeMyMoObjectArray(vm, &list->values, tuple->values.objects[i]);
            }
            return objectToValue(AS_OBJECT(list));
        }
        else if (IS_STRING(object))
        {
            MyMoString *string = AS_STRING(object);
            MyMoList *list = newList(vm);
            for (int i = 0; i < string->length; i++)
            {
                writeMyMoObjectArray(vm, &list->values, NEW_STRING(vm, &string->value[i], 1));
            }
            return objectToValue(AS_OBJECT(list));
        }
        else
        {
            MyMoList *list = newList(vm);
            writeMyMoObjectArray(vm, &list->values, object);
            return objectToValue(AS_OBJECT(list));
        }
    }
}

Value lenListMethod(MVM *vm, uint argc, MyMoObject *args[])
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

Value appendListMethod(MVM *vm, uint argc, MyMoObject *args[])
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
    writeMyMoObjectArray(vm, &list->values, pop(vm));
    return V_NIL_VAL;
}

Value copyListMethod(MVM *vm, uint argc, MyMoObject *args[])
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
        writeMyMoObjectArray(vm, &copyList->values, list->values.objects[i]);
    }
    return objectToValue(AS_OBJECT(copyList));
}

Value addListMethod(MVM *vm, uint argc, MyMoObject *args[])
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
        writeMyMoObjectArray(vm, &resultList->values, aList->values.objects[i]);
    }
    for (int i = 0; i < bList->values.count; i++)
    {
        writeMyMoObjectArray(vm, &resultList->values, bList->values.objects[i]);
    }
    return objectToValue(AS_OBJECT(resultList));
}

// Resolve a possibly-negative index against `count`; -1 if out of range.
static int listIndex(MyMoObject *index, int count)
{
    long i = INT_VAL(index);
    if (i < 0)
        i += count;
    return (i < 0 || i >= count) ? -1 : (int)i;
}

static int findInArray(MyMoObjectArray *array, MyMoObject *value)
{
    for (int i = 0; i < array->count; i++)
        if (isEqual(array->objects[i], value))
            return i;
    return -1;
}

Value popListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "pop", argc, 0, 1);
    if (!self) return V_EMPTY_VAL;
    MyMoObjectArray *values = &AS_LIST(self)->values;
    if (values->count == 0)
    {
        runtimeError(vm, "IndexError: pop from empty list");
        return V_EMPTY_VAL;
    }
    int i = values->count - 1;
    if (argc)
    {
        if (!IS_INT(args[0]) || (i = listIndex(args[0], values->count)) < 0)
        {
            runtimeError(vm, "IndexError: pop index out of range");
            return V_EMPTY_VAL;
        }
    }
    MyMoObject *item = values->objects[i];
    memmove(&values->objects[i], &values->objects[i + 1], sizeof(MyMoObject *) * (values->count - i - 1));
    values->count--;
    return objectToValue(item);
}

Value insertListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "insert", argc, 2, 2);
    if (!self) return V_EMPTY_VAL;
    if (!IS_INT(args[0]))
    {
        runtimeError(vm, "TypeError: insert() index must be an int");
        return V_EMPTY_VAL;
    }
    MyMoObjectArray *values = &AS_LIST(self)->values;
    long i = INT_VAL(args[0]);
    if (i < 0) i += values->count;
    if (i < 0) i = 0;
    if (i > values->count) i = values->count;
    writeMyMoObjectArray(vm, values, args[1]); // grow by one
    memmove(&values->objects[i + 1], &values->objects[i], sizeof(MyMoObject *) * (values->count - 1 - i));
    values->objects[i] = args[1];
    return V_NIL_VAL;
}

Value removeListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "remove", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    MyMoObjectArray *values = &AS_LIST(self)->values;
    int i = findInArray(values, args[0]);
    if (i < 0)
    {
        runtimeError(vm, "ValueError: list.remove(x): x not in list");
        return V_EMPTY_VAL;
    }
    memmove(&values->objects[i], &values->objects[i + 1], sizeof(MyMoObject *) * (values->count - i - 1));
    values->count--;
    return V_NIL_VAL;
}

Value indexListMethod(MVM *vm, uint argc, MyMoObject *args[])
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

Value containsListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "contains", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    return V_BOOL_VAL(findInArray(&AS_LIST(self)->values, args[0]) >= 0);
}

Value reverseListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "reverse", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoObjectArray *values = &AS_LIST(self)->values;
    for (int i = 0, j = values->count - 1; i < j; i++, j--)
    {
        MyMoObject *tmp = values->objects[i];
        values->objects[i] = values->objects[j];
        values->objects[j] = tmp;
    }
    return V_NIL_VAL;
}

Value clearListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "clear", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    AS_LIST(self)->values.count = 0;
    return V_NIL_VAL;
}

Value extendListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "extend", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    if (!IS_LIST(args[0]) && !IS_TUPLE(args[0]))
    {
        runtimeError(vm, "TypeError: extend() expects a list or tuple, got %s", getType(args[0]));
        return V_EMPTY_VAL;
    }
    MyMoObjectArray *src = IS_LIST(args[0]) ? &AS_LIST(args[0])->values : &AS_TUPLE(args[0])->values;
    int n = src->count; // snapshot: `xs.extend(xs)` must not loop forever
    for (int i = 0; i < n; i++)
        writeMyMoObjectArray(vm, &AS_LIST(self)->values, src->objects[i]);
    return V_NIL_VAL;
}

// sort(): numbers (int/double mixed) or strings, ascending. Insertion
// sort keeps it stable and lets a type mismatch abort cleanly.
static int compareForSort(MyMoObject *a, MyMoObject *b, bool *ok)
{
    if (IS_NUMBER(a) && IS_NUMBER(b))
    {
        double x = NUMBER_VAL(a), y = NUMBER_VAL(b);
        return (x > y) - (x < y);
    }
    if (IS_STRING(a) && IS_STRING(b))
        return compareStrings(AS_STRING(a), AS_STRING(b));
    *ok = false;
    return 0;
}

Value sortListMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "sort", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoObjectArray *values = &AS_LIST(self)->values;
    bool ok = true;
    for (int i = 1; i < values->count && ok; i++)
    {
        MyMoObject *key = values->objects[i];
        int j = i - 1;
        while (j >= 0 && compareForSort(values->objects[j], key, &ok) > 0 && ok)
        {
            values->objects[j + 1] = values->objects[j];
            j--;
        }
        values->objects[j + 1] = key;
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