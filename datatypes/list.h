#ifndef __LIST_H__
#define __LIST_H__

#include "object.h"
#include "../value.h"

#define NEW_LIST(vm) AS_OBJECT(newList(vm))
#define AS_LIST(object) ((MyMoList *)object)
#define IS_LIST(object) (object->type == OBJ_LIST)

// Elements are NaN-boxed Values: ints, doubles, nil and bools are stored
// inline, everything else as an object pointer.
typedef struct MyMoList
{
    MyMoObject object;
    ValueArray values;
} MyMoList;

MyMoList *newList(MVM *vm);

void printList(MyMoList *list);


void defineListClass(MVM *vm);

#endif