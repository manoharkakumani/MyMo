#ifndef __SET_H__
#define __SET_H__

#include "object.h"
#include "dict.h"
#include "../value.h"

#define AS_SET(object) ((MyMoSet *)object)
#define IS_SET(object) ((object)->type == OBJ_SET)

// set: an insertion-ordered collection of unique hashable values
// (strings, numbers, bools, Nil, tuples of those), stored as the keys of
// a dict whose values are unused.
typedef struct MyMoSet
{
    MyMoObject object;
    MyMoDict items;
} MyMoSet;

MyMoSet *newSet(MVM *vm);
// Adds v; false after raising a TypeError (v can't be a set element).
bool setAdd(MVM *vm, MyMoSet *set, Value v);
bool setHas(MVM *vm, MyMoSet *set, Value v);
// a | b, a & b, a - b or a ^ b as a new set.
MyMoSet *setCombine(MVM *vm, MyMoSet *a, MyMoSet *b, char op);
bool setIsSubset(MyMoSet *a, MyMoSet *b);
void defineSetClass(MVM *vm);

#endif
