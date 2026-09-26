#include "memory.h"
#include "dict.h"
#include "../vm.h"
#include "nil.h"
#include "bool.h"
#include "string.h"

#define TABLE_MAX_LOAD 0.75

MyMoDict *newDict(MVM *vm)
{
    MyMoDict *dict = AllocateObject(vm, MyMoDict, OBJ_DICT);
    initDict(dict);
    return dict;
}

void initDict(MyMoDict *dict)
{
    dict->count = 0;
    dict->capacity = -1;
    dict->entries = NULL;
    dict->modifyCount = 0;
    dict->tombstones = 0;
    dict->head = -1;
    dict->tail = -1;
    dict->object.type = OBJ_DICT;
}

void freeDict(MVM *vm, MyMoDict *dict)
{
    FreeArray(vm, Entry, dict->entries, dict->count);
    initDict(dict);
}

void freeDictionary(MVM *vm, MyMoDict *dict)
{
    FreeArray(vm, Entry, dict->entries, dict->count);
    Free(vm, MyMoDict, dict);
}

u32 hasher(char *key, size_t len)
{
    u32 hash = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= (u8)key[i];
        hash *= 16777619;
    }
    return hash;
}

Entry *findEntry(Entry *entries, int capacity, MyMoObject *key)
{
    u32 index = key->hash & capacity;
    Entry *tombstone = NULL;
    for (;;)
    {
        Entry *entry = &entries[index];
        if (entry->key == NULL)
        {
            // V_NIL_VAL marker => empty bucket; anything else => tombstone.
            if (V_IS_NIL(entry->value))
            {
                return tombstone != NULL ? tombstone : entry;
            }
            else
            {
                if (tombstone == NULL)
                    tombstone = entry;
            }
        }
        else if (entry->key == key)
        {
            // Pointer identity implies hash equality (interned keys).
            return entry;
        }
        index = (index + 1) & capacity;
    }
}

MyMoObject *getEntry(MVM *vm, MyMoDict *dict, MyMoObject *key)
{
    if (dict->count == 0) return NULL;
    Entry *entry = findEntry(dict->entries, dict->capacity, key);
    if (entry->key == NULL) return NULL;
    Value v = entry->value;
    if (V_IS_OBJ(v)) return V_AS_OBJ(v);
    // Inline value: box for legacy caller. Requires a vm; callers that pass
    // NULL only ever store heap objects, so this branch is only reachable
    // for vm-bearing callers in practice.
    return valueToBoxedObject(vm, v);
}

bool getEntryV(MyMoDict *dict, MyMoObject *key, Value *out)
{
    if (dict->count == 0) return false;
    Entry *entry = findEntry(dict->entries, dict->capacity, key);
    if (entry->key == NULL) return false;
    *out = entry->value;
    return true;
}

// Append the entry in `slot` to the insertion-order list.
static void linkEntry(MyMoDict *dict, int slot)
{
    Entry *entry = &dict->entries[slot];
    entry->prev = dict->tail;
    entry->next = -1;
    if (dict->tail >= 0)
        dict->entries[dict->tail].next = slot;
    else
        dict->head = slot;
    dict->tail = slot;
}

static void unlinkEntry(MyMoDict *dict, int slot)
{
    Entry *entry = &dict->entries[slot];
    if (entry->prev >= 0)
        dict->entries[entry->prev].next = entry->next;
    else
        dict->head = entry->next;
    if (entry->next >= 0)
        dict->entries[entry->next].prev = entry->prev;
    else
        dict->tail = entry->prev;
    entry->prev = entry->next = -1;
}

void adjustCapacity(MVM *vm, MyMoDict *dict, int capacity)
{
    Entry *entries = Allocate(vm, Entry, capacity + 1);
    for (int i = 0; i <= capacity; i++)
    {
        entries[i].key = NULL;
        entries[i].value = V_NIL_VAL;
        entries[i].prev = entries[i].next = -1;
    }
    Entry *oldEntries = dict->entries;
    int oldCapacity = dict->capacity;
    int oldHead = dict->head;
    dict->count = 0;
    dict->tombstones = 0; // rehashing drops them
    dict->head = dict->tail = -1;
    dict->entries = entries;
    dict->capacity = capacity;
    // Re-insert in insertion order so the new table keeps it.
    for (int i = oldHead; i >= 0; i = oldEntries[i].next)
        setEntryV(vm, dict, oldEntries[i].key, oldEntries[i].value);
    FreeArray(vm, Entry, oldEntries, oldCapacity + 1);
    // The entry array moved — every cached entry index from before this call
    // now points into freed memory or a different slot. Bump modifyCount to
    // invalidate IC sites globally.
    dict->modifyCount++;
}

// Legacy object API: heap nil/bool singletons are stored inline.
bool setEntry(MVM *vm, MyMoDict *dict, MyMoObject *key, MyMoObject *value)
{
    return setEntryV(vm, dict, key, objectToValue(value));
}

bool setEntryV(MVM *vm, MyMoDict *dict, MyMoObject *key, Value value)
{
    if (dict->count + dict->tombstones + 1 > (dict->capacity + 1) * TABLE_MAX_LOAD)
    {
        // Mostly tombstones: rehash in place instead of growing.
        bool mostlyDeleted = dict->count + 1 <= (dict->capacity + 1) * TABLE_MAX_LOAD / 2;
        int capacity = mostlyDeleted ? dict->capacity : ResizeCapacity(dict->capacity + 1) - 1;
        adjustCapacity(vm, dict, capacity);
    }
    Entry *entry = findEntry(dict->entries, dict->capacity, key);
    bool isNewKey = entry->key == NULL;
    if (isNewKey && !V_IS_NIL(entry->value))
        dict->tombstones--; // reusing a deleted slot
    entry->key = key;
    entry->value = value;
    if (isNewKey)
    {
        linkEntry(dict, (int)(entry - dict->entries));
        dict->count++;
        dict->modifyCount++;  // structural change: invalidate caches
    }
    return isNewKey;
}

bool deleteEntry(MVM *vm, MyMoDict *dict, MyMoObject *key)
{
    if (dict->count == 0)
        return false;
    Entry *entry = findEntry(dict->entries, dict->capacity, key);
    if (entry->key == NULL)
        return false;
    dict->count--;
    unlinkEntry(dict, (int)(entry - dict->entries));
    entry->key = NULL;
    // Tombstone marker (anything not V_NIL_VAL). Use V_FALSE_VAL so the
    // probe loop stops looking once it can; any non-nil sentinel works.
    entry->value = V_FALSE_VAL;
    dict->tombstones++;
    dict->modifyCount++;
    return true;
}

void copyDict(MVM *vm, MyMoDict *from, MyMoDict *to)
{
    Entry *entry;
    DICT_FOREACH(from, entry)
        setEntryV(vm, to, entry->key, entry->value);
}

MyMoObject *findKey(MyMoDict *dict, u32 hash)
{
    if (dict->count == 0)
        return NULL;
    u32 index = hash & dict->capacity;
    for (;;)
    {
        Entry *entry = &dict->entries[index];
        if (entry->key == NULL)
        {
            if (V_IS_NIL(entry->value))
                return NULL;
        }
        else if (entry->key->hash == hash)
        {
            return entry->key;
        }
        index = (index + 1) & dict->capacity;
    }
}

void printDict(MyMoDict *dict)
{
    printf("{");
    Entry *entry;
    bool first = true;
    DICT_FOREACH(dict, entry)
    {
        if (!first)
            printf(", ");
        first = false;
        printObject(entry->key);
        printf(": ");
        if (V_IS_OBJ(entry->value) && V_AS_OBJ(entry->value) == AS_OBJECT(dict))
            printf("{...}");
        else
            printValue(entry->value);
    }
    printf("}");
}

MyMoString *findString(MyMoDict *dict, const char *chars, int length, u32 hash)
{
    if (dict->entries == NULL)
        return NULL;
    u32 index = hash & dict->capacity;
    for (;;)
    {
        Entry *entry = &dict->entries[index];
        MyMoString *key = AS_STRING(entry->key);
        if (key == NULL)
            return NULL;
        if (key->length == length &&  entry->key->hash == hash && memcmp(key->value, chars, length) == 0)
        {
            return key;
        }
        index = (index + 1) & dict->capacity;
    }
}

MyMoInt *findInt(MyMoDict *dict, long value, int length, u32 hash)
{
    if (dict->entries == NULL)
        return NULL;
    u32 index = hash & dict->capacity;
    for (;;)
    {
        Entry *entry = &dict->entries[index];
        MyMoInt *key = AS_INT(entry->key);
        if (key == NULL)
            return NULL;
        if (key->length == length  &&  entry->key->hash == hash && key->value == value)
        {
            return key;
        }
        index = (index + 1) & dict->capacity;
    }
}

MyMoDouble *findDouble(MyMoDict *dict, double value, int length, u32 hash)
{
    if (dict->entries == NULL)
        return NULL;
    u32 index = hash & dict->capacity;
    for (;;)
    {
        Entry *entry = &dict->entries[index];
        MyMoDouble *key = AS_DOUBLE(entry->key);
        if (key == NULL)
            return NULL;
        if (key->length == length &&  entry->key->hash == hash  && key->value == value)
        {
            return key;
        }
        index = (index + 1) & dict->capacity;
    }
}

// ─── Built-in dict methods ──────────────────────────────────────────
// All methods read the receiver via peek(argc) — the dispatch loop
// has placed the bound-method object under the args, with its `self`
// pointer set to the dict instance. (See OP_GETP in vm.c which now
// allocates a fresh bound copy per lookup, so concurrent method
// references don't alias `self`.)

#include "../include/mymo_module.h"
#include "list.h"
#include "tuple.h"
#include "../repr.h"
#include "../builtins.h"

static MyMoDict *dictSelf(MVM *vm, const char *fn, int self_idx)
{
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, self_idx));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: %s() can only be applied on a dict", fn);
        return NULL;
    }
    return AS_DICT(function->self);
}

// ─── Dict keys ──────────────────────────────────────────────────────
// Dicts compare keys by identity. That is correct because every key
// type is interned: strings, ints and doubles by their constructors, Nil
// and bools are singletons, and tuples are interned here when they are
// used as a key: equal tuples map to one canonical tuple in
// vm->tupleKeys (a weak table, like the other intern tables).

static bool isScalarKey(MyMoObject *key)
{
    return IS_NIL(key) || IS_BOOL(key) || IS_INT(key) || IS_DOUBLE(key) || IS_STRING(key);
}

static MyMoObject *canonicalKey(MVM *vm, Value v, bool create, bool *hashable);

// The canonical tuple equal to `t` (creating it when `create`), or NULL
// when there is none yet or an element can't be a key (*hashable false).
static MyMoTuple *internTupleKey(MVM *vm, MyMoTuple *t, bool create, bool *hashable)
{
    int n = t->values.count;
    Value stackElems[16];
    Value *elems = n <= 16 ? stackElems : malloc(sizeof(Value) * (size_t)n);
    u32 hash = 2166136261u ^ (u32)n;
    MyMoTuple *result = NULL;
    for (int i = 0; i < n; i++)
    {
        MyMoObject *k = canonicalKey(vm, t->values.values[i], create, hashable);
        if (k == NULL)
            goto done;
        elems[i] = objectToValue(k);
        hash = (hash ^ k->hash) * 16777619u;
    }
    MyMoDict *table = &vm->tupleKeys;
    if (table->entries != NULL)
    {
        for (u32 index = hash & (u32)table->capacity;; index = (index + 1) & (u32)table->capacity)
        {
            MyMoObject *key = table->entries[index].key;
            if (key == NULL)
                break;
            MyMoTuple *candidate = AS_TUPLE(key);
            if (key->hash != hash || candidate->values.count != n)
                continue;
            bool same = true;
            for (int i = 0; i < n && same; i++)
                same = valueToBoxedObject(vm, candidate->values.values[i]) == valueToBoxedObject(vm, elems[i]);
            if (same)
            {
                result = candidate;
                goto done;
            }
        }
    }
    if (create)
    {
        result = newTuple(vm);
        for (int i = 0; i < n; i++)
            writeValueArray(vm, &result->values, elems[i]);
        result->object.hash = hash;
        setPrimitive(vm, table, AS_OBJECT(result));
    }
done:
    if (elems != stackElems)
        free(elems);
    return result;
}

static MyMoObject *canonicalKey(MVM *vm, Value v, bool create, bool *hashable)
{
    MyMoObject *key = valueToBoxedObject(vm, v);
    if (isScalarKey(key))
        return key;
    if (IS_TUPLE(key))
        return AS_OBJECT(internTupleKey(vm, AS_TUPLE(key), create, hashable));
    *hashable = false;
    return NULL;
}

// A Value as a key for storing: strings, numbers, bools, Nil and tuples
// of those. NULL after raising a TypeError.
MyMoObject *dictKey(MVM *vm, Value v)
{
    bool hashable = true;
    MyMoObject *key = canonicalKey(vm, v, true, &hashable);
    if (key)
        return key;
    if (V_IS_OBJ_TYPE(v, OBJ_TUPLE))
        runtimeError(vm, "TypeError: a tuple used as a dict key may only hold strings, numbers, booleans, Nil or tuples");
    else
        runtimeError(vm, "TypeError: dict keys must be strings, numbers, booleans, Nil or tuples of them, not %s",
                     valueTypeName(v));
    return NULL;
}

// A Value as a key for lookups: never creates a canonical tuple (if none
// exists, no dict can hold that key, so the returned object won't match).
MyMoObject *dictLookupKey(MVM *vm, Value v)
{
    bool hashable = true;
    MyMoObject *key = canonicalKey(vm, v, false, &hashable);
    return key ? key : valueToBoxedObject(vm, v);
}

static void keyError(MVM *vm, Value key)
{
    Value text = valueToRepr(vm, key);
    runtimeError(vm, "KeyError: %s", V_IS_EMPTY(text) ? "?" : AS_STRING(V_AS_OBJ(text))->value);
}

// d.get(key[, default])
Value dictGetMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "get", argc, 1, 2);
    if (!self) return V_EMPTY_VAL;
    Value value;
    if (getEntryV(AS_DICT(self), dictLookupKey(vm, args[0]), &value))
        return value;
    return argc == 2 ? args[1] : V_NIL_VAL;
}

// d.put(key, value) -> value
Value dictPutMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "put", argc, 2, 2);
    if (!self) return V_EMPTY_VAL;
    MyMoObject *key = dictKey(vm, args[0]);
    if (!key) return V_EMPTY_VAL;
    setEntryV(vm, AS_DICT(self), key, args[1]);
    return args[1];
}

Value dictHasMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "has", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    Value unused;
    return V_BOOL_VAL(getEntryV(AS_DICT(self), dictLookupKey(vm, args[0]), &unused));
}

// d.delete(key) -> whether it was there
Value dictDeleteMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "delete", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    return V_BOOL_VAL(deleteEntry(vm, AS_DICT(self), dictLookupKey(vm, args[0])));
}

// d.pop(key[, default]) -> the removed value (KeyError without default)
Value dictPopMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "pop", argc, 1, 2);
    if (!self) return V_EMPTY_VAL;
    MyMoObject *key = dictLookupKey(vm, args[0]);
    Value value;
    if (getEntryV(AS_DICT(self), key, &value))
    {
        deleteEntry(vm, AS_DICT(self), key);
        return value;
    }
    if (argc == 2)
        return args[1];
    keyError(vm, args[0]);
    return V_EMPTY_VAL;
}

// d.popitem() -> (key, value) of the most recently inserted entry
Value dictPopitemMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "popitem", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoDict *dict = AS_DICT(self);
    if (dict->tail < 0)
    {
        runtimeError(vm, "KeyError: popitem(): dictionary is empty");
        return V_EMPTY_VAL;
    }
    Entry *last = &dict->entries[dict->tail];
    MyMoTuple *item = newTuple(vm);
    writeValueArray(vm, &item->values, objectToValue(last->key));
    writeValueArray(vm, &item->values, last->value);
    deleteEntry(vm, dict, last->key);
    return V_OBJ_VAL(AS_OBJECT(item));
}

// d.setdefault(key[, default]) -> d[key], inserting default (Nil) first
// when the key is missing
Value dictSetdefaultMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "setdefault", argc, 1, 2);
    if (!self) return V_EMPTY_VAL;
    MyMoObject *key = dictKey(vm, args[0]);
    if (!key) return V_EMPTY_VAL;
    Value value;
    if (getEntryV(AS_DICT(self), key, &value))
        return value;
    value = argc == 2 ? args[1] : V_NIL_VAL;
    setEntryV(vm, AS_DICT(self), key, value);
    return value;
}

// d.update(other_dict | [(key, value), ...])
Value dictUpdateMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "update", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    MyMoDict *dict = AS_DICT(self);
    if (V_IS_OBJ_TYPE(args[0], OBJ_DICT))
    {
        copyDict(vm, AS_DICT(V_AS_OBJ(args[0])), dict);
        return V_NIL_VAL;
    }
    ValueArray pairs;
    initValueArray(vm, &pairs);
    if (!appendIterable(vm, "update", args[0], &pairs))
    {
        freeValueArray(vm, &pairs);
        return V_EMPTY_VAL;
    }
    for (int i = 0; i < pairs.count; i++)
    {
        Value p = pairs.values[i];
        ValueArray *kv = V_IS_OBJ_TYPE(p, OBJ_TUPLE) ? &AS_TUPLE(V_AS_OBJ(p))->values
                       : V_IS_OBJ_TYPE(p, OBJ_LIST)  ? &AS_LIST(V_AS_OBJ(p))->values
                                                     : NULL;
        MyMoObject *key = NULL;
        if (!kv || kv->count != 2)
            runtimeError(vm, "ValueError: update() element %d is not a (key, value) pair", i);
        else
            key = dictKey(vm, kv->values[0]);
        if (!key)
        {
            freeValueArray(vm, &pairs);
            return V_EMPTY_VAL;
        }
        setEntryV(vm, dict, key, kv->values[1]);
    }
    freeValueArray(vm, &pairs);
    return V_NIL_VAL;
}

Value dictClearMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "clear", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoDict *dict = AS_DICT(self);
    u32 version = dict->modifyCount;
    FreeArray(vm, Entry, dict->entries, dict->capacity + 1);
    initDict(dict);
    dict->modifyCount = version + 1; // invalidate inline caches
    return V_NIL_VAL;
}

Value dictCopyMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "copy", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoDict *copy = newDict(vm);
    copyDict(vm, AS_DICT(self), copy);
    return V_OBJ_VAL(AS_OBJECT(copy));
}

// d.items() -> [(key, value), ...] in insertion order
Value dictItemsMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "items", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoList *out = newList(vm);
    Entry *e;
    DICT_FOREACH(AS_DICT(self), e)
    {
        MyMoTuple *item = newTuple(vm);
        writeValueArray(vm, &item->values, objectToValue(e->key));
        writeValueArray(vm, &item->values, e->value);
        writeValueArray(vm, &out->values, V_OBJ_VAL(AS_OBJECT(item)));
    }
    return V_OBJ_VAL(AS_OBJECT(out));
}

Value dictKeysMethod(MVM *vm, uint argc, Value args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: keys() takes 0 arguments (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoDict *dict = dictSelf(vm, "keys", 0);
    if (!dict) return V_EMPTY_VAL;
    MyMoList *out = newList(vm);
    Entry *e;
    DICT_FOREACH(dict, e)
        writeValueArray(vm, &out->values, objectToValue(e->key));
    return objectToValue(AS_OBJECT(out));
}

Value dictValuesMethod(MVM *vm, uint argc, Value args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: values() takes 0 arguments (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoDict *dict = dictSelf(vm, "values", 0);
    if (!dict) return V_EMPTY_VAL;
    MyMoList *out = newList(vm);
    Entry *e;
    DICT_FOREACH(dict, e)
        writeValueArray(vm, &out->values, e->value);
    return objectToValue(AS_OBJECT(out));
}

Value dictLenMethod(MVM *vm, uint argc, Value args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: __len__() takes 0 arguments (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoDict *dict = dictSelf(vm, "__len__", 0);
    if (!dict) return V_EMPTY_VAL;
    return objectToValue(NEW_INT(vm, dict->count));
}

void defineDictMethods(MVM *vm)
{
    defineMethod(vm, OBJ_DICT, "get",     dictGetMethod);
    defineMethod(vm, OBJ_DICT, "put",     dictPutMethod);
    defineMethod(vm, OBJ_DICT, "has",     dictHasMethod);
    defineMethod(vm, OBJ_DICT, "delete",  dictDeleteMethod);
    defineMethod(vm, OBJ_DICT, "keys",    dictKeysMethod);
    defineMethod(vm, OBJ_DICT, "values",  dictValuesMethod);
    defineMethod(vm, OBJ_DICT, "__len__", dictLenMethod);
    defineMethod(vm, OBJ_DICT, "pop",        dictPopMethod);
    defineMethod(vm, OBJ_DICT, "popitem",    dictPopitemMethod);
    defineMethod(vm, OBJ_DICT, "setdefault", dictSetdefaultMethod);
    defineMethod(vm, OBJ_DICT, "update",     dictUpdateMethod);
    defineMethod(vm, OBJ_DICT, "clear",      dictClearMethod);
    defineMethod(vm, OBJ_DICT, "copy",       dictCopyMethod);
    defineMethod(vm, OBJ_DICT, "items",      dictItemsMethod);
}

void defineDictClass(MVM *vm)
{
    MyMoString *name = newString(vm, "dict", 4);
    MyMoBuiltInClass *dictClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_DICT] = dictClass;
    defineDictMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(dictClass));
}

void setPrimitive(MVM *vm, MyMoDict *dict, MyMoObject *key)
{
    if (dict->count + 1 > (dict->capacity + 1) * TABLE_MAX_LOAD)
    {
        int capacity = ResizeCapacity(dict->capacity + 1) - 1;
        adjustCapacity(vm, dict, capacity);
    }

    uint index = key->hash & dict->capacity;
    Entry *entry;
    for (;;) {
        entry = &dict->entries[index];
        if (entry->key == NULL) {
            break;
        } else {
            if (entry->key == key) {
                break;
            }
        }
        index = (index + 1) & dict->capacity;
    }
    if (entry->key == key)
        return; // already interned
    entry->key = key;
    entry->value = V_NIL_VAL;
    linkEntry(dict, (int)(index));
    dict->count++;
    dict->modifyCount++;
}