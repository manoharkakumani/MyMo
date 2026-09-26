#include "set.h"
#include "../memory.h"
#include "../vm.h"
#include "../builtins.h"
#include "../repr.h"
#include "class.h"
#include "function.h"

MyMoSet *newSet(MVM *vm)
{
    MyMoSet *set = AllocateObject(vm, MyMoSet, OBJ_SET);
    initDict(&set->items);
    return set;
}

bool setAdd(MVM *vm, MyMoSet *set, Value v)
{
    MyMoObject *key = dictKey(vm, v);
    if (!key)
        return false;
    setEntryV(vm, &set->items, key, V_NIL_VAL);
    return true;
}

bool setHas(MVM *vm, MyMoSet *set, Value v)
{
    Value unused;
    return getEntryV(&set->items, dictLookupKey(vm, v), &unused);
}

MyMoSet *setCombine(MVM *vm, MyMoSet *a, MyMoSet *b, char op)
{
    MyMoSet *out = newSet(vm);
    Value unused;
    Entry *e;
    DICT_FOREACH(&a->items, e)
    {
        bool inB = getEntryV(&b->items, e->key, &unused);
        if (op == '|' || (op == '&' && inB) || ((op == '-' || op == '^') && !inB))
            setEntryV(vm, &out->items, e->key, V_NIL_VAL);
    }
    if (op == '|' || op == '^')
    {
        DICT_FOREACH(&b->items, e)
        {
            if (op == '|' || !getEntryV(&a->items, e->key, &unused))
                setEntryV(vm, &out->items, e->key, V_NIL_VAL);
        }
    }
    return out;
}

bool setIsSubset(MyMoSet *a, MyMoSet *b)
{
    if (a->items.count > b->items.count)
        return false;
    Value unused;
    Entry *e;
    DICT_FOREACH(&a->items, e)
        if (!getEntryV(&b->items, e->key, &unused))
            return false;
    return true;
}

// Elements of any iterable as a set (the set itself when it is one).
static MyMoSet *asSet(MVM *vm, const char *fn, Value v)
{
    if (V_IS_OBJ_TYPE(v, OBJ_SET))
        return AS_SET(V_AS_OBJ(v));
    ValueArray items;
    initValueArray(vm, &items);
    MyMoSet *set = NULL;
    if (appendIterable(vm, fn, v, &items))
    {
        set = newSet(vm);
        for (int i = 0; i < items.count && set; i++)
            if (!setAdd(vm, set, items.values[i]))
                set = NULL;
    }
    freeValueArray(vm, &items);
    return set;
}

static Value setValue(MyMoSet *set) { return V_OBJ_VAL(AS_OBJECT(set)); }

// set() / set(iterable)
static Value newSetMethod(MVM *vm, uint argc, Value args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: set() takes at most 1 argument (%u given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoSet *set;
    if (argc == 0)
        set = newSet(vm);
    else if (V_IS_OBJ_TYPE(args[0], OBJ_SET))
        set = setCombine(vm, AS_SET(V_AS_OBJ(args[0])), AS_SET(V_AS_OBJ(args[0])), '|');
    else if (!(set = asSet(vm, "set", args[0])))
        return V_EMPTY_VAL;
    if (argc)
        popV(vm);
    return setValue(set);
}

static Value addSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "add", argc, 1, 1);
    if (!self || !setAdd(vm, AS_SET(self), args[0]))
        return V_EMPTY_VAL;
    return V_NIL_VAL;
}

// remove(x) raises KeyError when x is missing; discard(x) doesn't.
static Value removeSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "remove", argc, 1, 1);
    if (!self)
        return V_EMPTY_VAL;
    if (!deleteEntry(vm, &AS_SET(self)->items, dictLookupKey(vm, args[0])))
    {
        Value text = valueToRepr(vm, args[0]);
        runtimeError(vm, "KeyError: %s", V_IS_EMPTY(text) ? "?" : AS_STRING(V_AS_OBJ(text))->value);
        return V_EMPTY_VAL;
    }
    return V_NIL_VAL;
}

static Value discardSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "discard", argc, 1, 1);
    if (!self)
        return V_EMPTY_VAL;
    deleteEntry(vm, &AS_SET(self)->items, dictLookupKey(vm, args[0]));
    return V_NIL_VAL;
}

// pop() removes and returns the most recently added element.
static Value popSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "pop", argc, 0, 0);
    if (!self)
        return V_EMPTY_VAL;
    MyMoDict *items = &AS_SET(self)->items;
    if (items->tail < 0)
    {
        runtimeError(vm, "KeyError: pop from an empty set");
        return V_EMPTY_VAL;
    }
    MyMoObject *key = items->entries[items->tail].key;
    deleteEntry(vm, items, key);
    return objectToValue(key);
}

static Value clearSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "clear", argc, 0, 0);
    if (!self)
        return V_EMPTY_VAL;
    MyMoDict *items = &AS_SET(self)->items;
    FreeArray(vm, Entry, items->entries, items->capacity + 1);
    initDict(items);
    return V_NIL_VAL;
}

static Value copySetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "copy", argc, 0, 0);
    if (!self)
        return V_EMPTY_VAL;
    return setValue(setCombine(vm, AS_SET(self), AS_SET(self), '|'));
}

static Value lenSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "__len__", argc, 0, 0);
    if (!self)
        return V_EMPTY_VAL;
    return V_INT_VAL(AS_SET(self)->items.count);
}

// union/intersection/difference(*iterables) and symmetric_difference(x):
// fold the arguments into a new set with `op`.
static Value combineMethod(MVM *vm, const char *fn, uint argc, Value args[], char op, uint max)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 1, max);
    if (!self)
        return V_EMPTY_VAL;
    MyMoSet *result = AS_SET(self);
    for (uint i = 0; i < argc; i++)
    {
        MyMoSet *other = asSet(vm, fn, args[i]);
        if (!other)
            return V_EMPTY_VAL;
        result = setCombine(vm, result, other, op);
    }
    return setValue(result);
}

static Value unionSetMethod(MVM *vm, uint argc, Value args[]) { return combineMethod(vm, "union", argc, args, '|', 255); }
static Value intersectionSetMethod(MVM *vm, uint argc, Value args[]) { return combineMethod(vm, "intersection", argc, args, '&', 255); }
static Value differenceSetMethod(MVM *vm, uint argc, Value args[]) { return combineMethod(vm, "difference", argc, args, '-', 255); }
static Value symmetricDifferenceSetMethod(MVM *vm, uint argc, Value args[]) { return combineMethod(vm, "symmetric_difference", argc, args, '^', 1); }

// update(*iterables): add every element in place.
static Value updateSetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "update", argc, 1, 255);
    if (!self)
        return V_EMPTY_VAL;
    for (uint i = 0; i < argc; i++)
    {
        MyMoSet *other = asSet(vm, "update", args[i]);
        if (!other)
            return V_EMPTY_VAL;
        Entry *e;
        DICT_FOREACH(&other->items, e)
            setEntryV(vm, &AS_SET(self)->items, e->key, V_NIL_VAL);
    }
    return V_NIL_VAL;
}

static Value relation(MVM *vm, const char *fn, uint argc, Value args[], int kind)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 1, 1);
    if (!self)
        return V_EMPTY_VAL;
    MyMoSet *other = asSet(vm, fn, args[0]);
    if (!other)
        return V_EMPTY_VAL;
    MyMoSet *me = AS_SET(self);
    if (kind == 0)
        return V_BOOL_VAL(setIsSubset(me, other));
    if (kind == 1)
        return V_BOOL_VAL(setIsSubset(other, me));
    return V_BOOL_VAL(setCombine(vm, me, other, '&')->items.count == 0);
}

static Value issubsetSetMethod(MVM *vm, uint argc, Value args[]) { return relation(vm, "issubset", argc, args, 0); }
static Value issupersetSetMethod(MVM *vm, uint argc, Value args[]) { return relation(vm, "issuperset", argc, args, 1); }
static Value isdisjointSetMethod(MVM *vm, uint argc, Value args[]) { return relation(vm, "isdisjoint", argc, args, 2); }

void defineSetClass(MVM *vm)
{
    MyMoString *name = newString(vm, "set", 3);
    MyMoBuiltInClass *setClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_SET] = setClass;
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(setClass));
    defineMethod(vm, OBJ_SET, "__new__", newSetMethod);
    defineMethod(vm, OBJ_SET, "__len__", lenSetMethod);
    defineMethod(vm, OBJ_SET, "add", addSetMethod);
    defineMethod(vm, OBJ_SET, "remove", removeSetMethod);
    defineMethod(vm, OBJ_SET, "discard", discardSetMethod);
    defineMethod(vm, OBJ_SET, "pop", popSetMethod);
    defineMethod(vm, OBJ_SET, "clear", clearSetMethod);
    defineMethod(vm, OBJ_SET, "copy", copySetMethod);
    defineMethod(vm, OBJ_SET, "union", unionSetMethod);
    defineMethod(vm, OBJ_SET, "intersection", intersectionSetMethod);
    defineMethod(vm, OBJ_SET, "difference", differenceSetMethod);
    defineMethod(vm, OBJ_SET, "symmetric_difference", symmetricDifferenceSetMethod);
    defineMethod(vm, OBJ_SET, "update", updateSetMethod);
    defineMethod(vm, OBJ_SET, "issubset", issubsetSetMethod);
    defineMethod(vm, OBJ_SET, "issuperset", issupersetSetMethod);
    defineMethod(vm, OBJ_SET, "isdisjoint", isdisjointSetMethod);
}
