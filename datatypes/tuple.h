#ifndef __TUPLE_H__
#define __TUPLE_H__

#include "object.h"

#define NEW_TUPLE(vm) newTuple(vm)
#define AS_TUPLE(object) ((MyMoTuple *)object)
#define IS_TUPLE(object) (object->type == OBJ_TUPLE)

typedef struct MyMoTuple
{
    MyMoObject object;
    MyMoObjectArray values;
} MyMoTuple;

MyMoTuple *newTuple(MVM *vm);
void printTuple(MyMoTuple *tuple);
void defineTupleClass(MVM *vm);

#endif