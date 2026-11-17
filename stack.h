#ifndef __STACK_H__
#define __STACK_H__

#include "common.h"
#include "value.h"
#include "datatypes/object.h"

void printStack(MVM *vm);
void manipulateStack(MVM *vm, MyMoObject *value, int position);

// Legacy object-pointer API. Wraps the Value-based primitives below using
// V_OBJ_VAL / V_AS_OBJ. Safe today because every value on the stack is still
// a heap object; once OPs start pushing inline values directly via pushV,
// callers of `pop` must first migrate to popV (otherwise V_AS_OBJ would
// reinterpret an inline int's bits as a pointer).
void  push (MVM *vm, MyMoObject *object);
MyMoObject *pop (MVM *vm);
MyMoObject *peek(MVM *vm, int position);

// Value-native primitives. Used by future steps as inline ints / doubles /
// nil / bool migrate off the heap.
void  pushV (MVM *vm, Value v);
Value popV  (MVM *vm);
Value peekV (MVM *vm, int position);

#endif
