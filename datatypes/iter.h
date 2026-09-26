#ifndef __ITER_H__
#define __ITER_H__

#include "object.h"
#include "../value.h"

#define AS_ITER(object) ((MyMoIter *)object)
#define IS_ITER(object) object->type == OBJ_ITER


typedef struct MyMoIter
{
    MyMoObject object;
    MyMoObject *iterator;
    int index;
} MyMoIter;

MyMoIter *newIter(MVM *vm, MyMoObject *object);
Value nextIter(MVM *vm, MyMoIter *object);

void printIter(MyMoIter *object);
#endif