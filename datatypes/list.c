#include "list.h"
#include "nil.h"
#include "../memory.h"
#include "../vm.h"
#include "datatypes.h"

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

void defineListMethods(MVM *vm)
{
    defineMethod(vm, OBJ_LIST, "__new__", newListMethod);
    defineMethod(vm, OBJ_LIST, "__len__", lenListMethod);
    defineMethod(vm, OBJ_LIST, "+", addListMethod);
    // defineMethod(vm, OBJ_LIST, "remove", removeListMethod);
    defineMethod(vm, OBJ_LIST, "append", appendListMethod);
    defineMethod(vm, OBJ_LIST, "copy", copyListMethod);
    // defineMethod(vm, OBJ_LIST, "clear", clearListMethod);
    // defineMethod(vm, OBJ_LIST, "contains", containsListMethod);
    // defineMethod(vm, OBJ_LIST, "index", indexListMethod);
    // defineMethod(vm, OBJ_LIST, "pop", popListMethod);
    // defineMethod(vm, OBJ_LIST, "reverse", reverseListMethod);
    // defineMethod(vm, OBJ_LIST, "sort", sortListMethod);
}

void defineListClass(MVM *vm)
{
    MyMoString *name = newString(vm, "list", 4);
    MyMoBuiltInClass *listClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_LIST] = listClass;
    defineListMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(listClass));
}