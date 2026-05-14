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

void adjustCapacity(MVM *vm, MyMoDict *dict, int capacity)
{
    Entry *entries = Allocate(vm, Entry, capacity + 1);
    for (int i = 0; i <= capacity; i++)
    {
        entries[i].key = NULL;
        entries[i].value = V_NIL_VAL;
    }
    Entry *oldEntries = dict->entries;
    int oldCapacity = dict->capacity;
    dict->count = 0;
    dict->entries = entries;
    dict->capacity = capacity;
    for (int i = 0; i <= oldCapacity; i++)
    {
        Entry *entry = &oldEntries[i];
        if (entry->key == NULL)
            continue;
        setEntryV(vm, dict, entry->key, entry->value);
    }
    FreeArray(vm, Entry, oldEntries, oldCapacity + 1);
    // The entry array moved — every cached entry index from before this call
    // now points into freed memory or a different slot. Bump modifyCount to
    // invalidate IC sites globally.
    dict->modifyCount++;
}

bool setEntry(MVM *vm, MyMoDict *dict, MyMoObject *key, MyMoObject *value)
{
    return setEntryV(vm, dict, key, V_OBJ_VAL(value));
}

bool setEntryV(MVM *vm, MyMoDict *dict, MyMoObject *key, Value value)
{
    if (dict->count + 1 > (dict->capacity + 1) * TABLE_MAX_LOAD)
    {
        int capacity = ResizeCapacity(dict->capacity + 1) - 1;
        adjustCapacity(vm, dict, capacity);
    }
    Entry *entry = findEntry(dict->entries, dict->capacity, key);
    bool isNewKey = entry->key == NULL;
    entry->key = key;
    entry->value = value;
    if (isNewKey)
    {
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
    entry->key = NULL;
    // Tombstone marker (anything not V_NIL_VAL). Use V_FALSE_VAL so the
    // probe loop stops looking once it can; any non-nil sentinel works.
    entry->value = V_FALSE_VAL;
    dict->modifyCount++;
    return true;
}

void copyDict(MVM *vm, MyMoDict *from, MyMoDict *to)
{
    for (int i = 0; i <= from->capacity; i++)
    {
        Entry *entry = &from->entries[i];
        if (entry->key != NULL)
        {
            setEntryV(vm, to, entry->key, entry->value);
        }
    }
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
    for (int i = 0, j = dict->count; i <= dict->capacity; i++)
    {
        Entry *entry = &dict->entries[i];
        if (entry->key != NULL)
        {
            printObject(entry->key);
            printf(": ");
            if (V_IS_OBJ(entry->value) && V_AS_OBJ(entry->value) == AS_OBJECT(dict))
            {
                printf("{...}");
            }
            else
            {
                printValue(entry->value);
            }
            if (--j)
                printf(", ");
        }
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

MyMoObject *dictGetMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc < 1 || argc > 2)
    {
        runtimeError(vm, "TypeError: get() takes 1 or 2 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "get", (int)argc);
    if (!dict) return NEW_EMPTY;
    // Pop args in reverse: default (if present) then key.
    MyMoObject *def = (argc == 2) ? pop(vm) : NEW_NIL;
    MyMoObject *key = pop(vm);
    MyMoObject *value = getEntry(vm, dict, key);
    return value ? value : def;
}

MyMoObject *dictPutMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 2)
    {
        runtimeError(vm, "TypeError: put() takes exactly 2 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "put", 2);
    if (!dict) return NEW_EMPTY;
    MyMoObject *value = pop(vm);
    MyMoObject *key = pop(vm);
    setEntry(vm, dict, key, value);
    return value;
}

MyMoObject *dictHasMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: has() takes exactly 1 argument (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "has", 1);
    if (!dict) return NEW_EMPTY;
    MyMoObject *key = pop(vm);
    return NEW_BOOL(getEntry(vm, dict, key) != NULL);
}

MyMoObject *dictDeleteMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: delete() takes exactly 1 argument (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "delete", 1);
    if (!dict) return NEW_EMPTY;
    MyMoObject *key = pop(vm);
    bool existed = deleteEntry(vm, dict, key);
    return NEW_BOOL(existed);
}

MyMoObject *dictKeysMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: keys() takes 0 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "keys", 0);
    if (!dict) return NEW_EMPTY;
    MyMoList *out = newList(vm);
    for (int i = 0; i <= dict->capacity; i++)
    {
        Entry *e = &dict->entries[i];
        if (e->key != NULL)
            writeMyMoObjectArray(vm, &out->values, e->key);
    }
    return AS_OBJECT(out);
}

MyMoObject *dictValuesMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: values() takes 0 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "values", 0);
    if (!dict) return NEW_EMPTY;
    MyMoList *out = newList(vm);
    for (int i = 0; i <= dict->capacity; i++)
    {
        Entry *e = &dict->entries[i];
        if (e->key != NULL)
        {
            MyMoObject *v = V_IS_OBJ(e->value) ? V_AS_OBJ(e->value)
                                               : valueToBoxedObject(vm, e->value);
            writeMyMoObjectArray(vm, &out->values, v);
        }
    }
    return AS_OBJECT(out);
}

MyMoObject *dictLenMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: __len__() takes 0 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoDict *dict = dictSelf(vm, "__len__", 0);
    if (!dict) return NEW_EMPTY;
    return NEW_INT(vm, dict->count);
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
    entry->key = key;
    entry->value = V_NIL_VAL;
    dict->count++;
    dict->modifyCount++;
}