#ifndef __GC_H__
#define __GC_H__

#include "common.h"
#include "datatypes/function.h"

// Mark-sweep collector; see gc.c for when it runs and what it roots.

void initGC(MVM *vm);
void collectGarbage(MVM *vm);

// A captured (closure-referenced) or module frame that has left the call
// stack. It is freed by a later collection once nothing references it.
void retireFrame(MVM *vm, CallFrame *frame);
void freeRetiredFrames(MVM *vm);

// Implemented in memory.c.
void freeObject(MVM *vm, MyMoObject *object);

#endif
