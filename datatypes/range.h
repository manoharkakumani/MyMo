#ifndef __RANGE_H__
#define __RANGE_H__

#include "object.h"
#include "../value.h"

#define AS_RANGE(object) ((MyMoRange *)object)
#define IS_RANGE(object) ((object)->type == OBJ_RANGE)

// range(stop) / range(start, stop[, step]): a lazy arithmetic sequence.
// Nothing is materialised; iteration, len(), `in` and indexing compute
// elements from the three bounds.
typedef struct MyMoRange
{
    MyMoObject object;
    long start;
    long stop;
    long step; // never 0
} MyMoRange;

MyMoRange *newRange(MVM *vm, long start, long stop, long step);
long rangeLength(MyMoRange *range);
long rangeAt(MyMoRange *range, long i); // i in [0, length)
bool rangeContains(MyMoRange *range, long n);
void defineRangeClass(MVM *vm);

#endif
