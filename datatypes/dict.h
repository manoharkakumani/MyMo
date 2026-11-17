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
    MyMoObject *key;
    Value value;     // NaN-boxed; legacy callers go through setEntry/getEntry which wrap/unwrap
    // Insertion order: slot indices of the neighbouring live entries
    // (-1 at either end). Slots never move except on rehash, which
    // rebuilds the list in the same order.
    int prev;
    int next;
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
    // Deleted slots still in the table. They keep probe chains intact but
    // occupy space, so the resize check counts them; otherwise a dict with
    // insert/delete churn fills with tombstones and findEntry never finds
    // an empty slot (infinite probe loop).
    int tombstones;
    // First and last live entries in insertion order (-1 when empty).
    int head;
    int tail;
} MyMoDict;

// Visit a dict's live entries in insertion order:
//     Entry *e;
//     DICT_FOREACH(dict, e) { ... e->key, e->value ... }
// Don't insert or delete keys inside the loop.
#define DICT_FOREACH(dict, e)                                          \
    for (int _slot = (dict)->head;                                      \
         _slot >= 0 && ((e) = &(dict)->entries[_slot], true);           \
         _slot = (e)->next)

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

// A Value as a dict key for storing (strings, numbers, bools, Nil and
// tuples of them; tuples are interned so equal tuples are one key). NULL
// after raising a TypeError for anything else.
MyMoObject *dictKey(MVM *vm, Value v);
// The same for lookups (d[k], k in d, get/has/pop/delete): never raises.
MyMoObject *dictLookupKey(MVM *vm, Value v);

MyMoString *findString(MyMoDict *dict, const char *chars, int length, u32 hash);

MyMoInt *findInt(MyMoDict *dict, long value, int length, u32 hash);

MyMoDouble *findDouble(MyMoDict *dict, double value, int length, u32 hash);

u32 hasher(char *key, size_t len);

#endif
