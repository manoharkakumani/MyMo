#ifndef __REPR_H__
#define __REPR_H__

#include "common.h"
#include "value.h"

// Text form of values, shared by print(), str(), f-strings and repr().
//
// "str" form: strings are their own text. "repr" form: strings are
// quoted and escaped. Containers always show their elements in repr
// form, so print(["a", 1]) shows ["a", 1]. Instances use their class's
// __str__ (or __repr__) when it defines one.

typedef struct
{
    char *data;
    int length;
    int capacity;
} StrBuf;

void strbufInit(StrBuf *b);
void strbufFree(StrBuf *b);
void strbufAppend(StrBuf *b, const char *s, int length);
void strbufAppendC(StrBuf *b, const char *s);

// Appends v's text to b. False if a user __str__/__repr__ raised (the
// error is already reported and the exception is in flight).
bool formatValue(MVM *vm, StrBuf *b, Value v, bool repr);

// The str() / repr() of v as a string Value, or V_EMPTY_VAL on error.
Value valueToStr(MVM *vm, Value v);
Value valueToRepr(MVM *vm, Value v);

#endif
