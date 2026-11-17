#ifndef __DOUBLE_H__
#define __DOUBLE_H__

#include "object.h"
#include "../value.h"  // MyMoDouble struct is fwd-declared and fully defined here

#define NEW_DOUBLE(vm, value) AS_OBJECT(newDouble(vm, value))
#define AS_DOUBLE(object) ((MyMoDouble *)object)
#define DOUBLE_VAL(object) ((MyMoDouble *)object)->value
#define IS_DOUBLE(object) (object->type == OBJ_DOUBLE)

MyMoDouble *newDouble(MVM *vm, double value);

void printDouble(MyMoDouble *object);

void defineDoubleClass(MVM *vm);

#endif