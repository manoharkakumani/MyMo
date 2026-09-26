#ifndef __TUPLE_H__
#define __TUPLE_H__

#include "object.h"
#include "../value.h"

#define NEW_TUPLE(vm) newTuple(vm)
#define AS_TUPLE(object) ((MyMoTuple *)object)
#define IS_TUPLE(object) (object->type == OBJ_TUPLE)

// Same element representation as MyMoList (inline Values).
typedef struct MyMoTuple
{
    MyMoObject object;
    ValueArray values;
} MyMoTuple;

MyMoTuple *newTuple(MVM *vm);
void printTuple(MyMoTuple *tuple);
void defineTupleClass(MVM *vm);

#endif