#ifndef __DICT_H__
#define __DICT_H__

#include "object.h"
#include "string.h"
#include "int.h"
#include "double.h"
#include "../value.h"

#define NEW_DICT(vm) AS_OBJECT(newDict(vm))
#define AS_DICT(object) ((MyMoDict *)object)
#define IS_DICT(object) (object->type == OBJ_DICT)

typedef struct Entry
{
    struct Entry *prev;
    struct Entry *next;
    MyMoObject *key;
    Value value;     // NaN-boxed; legacy callers go through setEntry/getEntry which wrap/unwrap
    int index;
} Entry;

typedef struct MyMoDict
{
    MyMoObject object;
    int count;
    int capacity;
    Entry *entries;
    // Bumped on every structural change (insert of a new key, delete,
    // resize). Pure value updates of an existing key do NOT bump it, so
    // inline caches keyed on (dict, modifyCount, entry_index) survive
    // `i = i + 1` style hot loops where the same key is overwritten
    // millions of times. See OP_GETV/OP_SETV in vm.c.
    u32 modifyCount;
} MyMoDict;

MyMoDict *newDict(MVM *vm);
void freeDictionary(MVM *vm, MyMoDict *dict);

void initDict(MyMoDict *dict);
void copyDict(MVM *vm, MyMoDict *from, MyMoDict *to);
void printDict(MyMoDict *dict);
void freeDict(MVM *vm, MyMoDict *dict);

// Legacy MyMoObject* API. setEntry wraps with V_OBJ_VAL; getEntry boxes
// inline values via valueToBoxedObject (ints today; doubles/nil/bool when
// they migrate). Callers without a vm in scope (rare; e.g. object.c
// equality predicate) pass NULL — those paths must only encounter
// V_IS_OBJ values.
MyMoObject *getEntry(MVM *vm, MyMoDict *dict, MyMoObject *key);
bool setEntry(MVM *vm, MyMoDict *dict, MyMoObject *key, MyMoObject *value);
bool deleteEntry(MVM *vm, MyMoDict *dict, MyMoObject *key);

// Value-native API. Hot paths use these to avoid the box/unbox round trip.
bool getEntryV(MyMoDict *dict, MyMoObject *key, Value *out);
bool setEntryV(MVM *vm, MyMoDict *dict, MyMoObject *key, Value value);

void setPrimitive(MVM *vm, MyMoDict *dict, MyMoObject *key);

void defineDictClass(MVM *vm);

MyMoString *findString(MyMoDict *dict, const char *chars, int length, u32 hash);

MyMoInt *findInt(MyMoDict *dict, long value, int length, u32 hash);

MyMoDouble *findDouble(MyMoDict *dict, double value, int length, u32 hash);

u32 hasher(char *key, size_t len);

#endif
