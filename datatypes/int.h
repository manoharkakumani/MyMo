#ifndef __INT_H__
#define __INT_H__

#include "object.h"
#include "../value.h"  // MyMoInt struct is fwd-declared and fully defined here

#define NEW_INT(vm, value) AS_OBJECT(newInt(vm, value))
#define AS_INT(object) ((MyMoInt *)object)
#define INT_VAL(object) ((MyMoInt *)object)->value
#define IS_INT(object) (object->type == OBJ_INT)

#define isInteger(value) (value - ((int)value))

MyMoInt *newInt(MVM *vm, long value);

void printInt(MyMoInt *object);

void defineIntClass(MVM *vm);

#endif