#ifndef __BUILTINS_H__
#define __BUILTINS_H__

#include "common.h"
#include "value.h"
#include "datatypes/function.h"

// min, max, sum, sorted, map, filter, zip, enumerate, isinstance, ...
// and the tuple()/dict()/float() constructors (builtins.c).
void defineBuiltInHelpers(MVM *vm);

// Appends an iterable's elements (list, tuple, string characters, dict
// keys, range) to `out`; false after raising a TypeError naming `fn`.
bool appendIterable(MVM *vm, const char *fn, Value v, ValueArray *out);

// Stable in-place sort. `key` is a callable or Nil; `reverse` flips the
// order. False after raising (incomparable elements, or key raised).
bool sortValues(MVM *vm, ValueArray *values, Value key, bool reverse);

void defineBuiltInFunction(MVM *vm, const char *name, BuiltInfunction function);

// Takes the keyword argument `name` passed to the builtin being called
// (f(x, name=value)): true and *out set if it was given. Keywords a
// builtin doesn't take are reported as errors after it returns.
bool takeKeyword(MVM *vm, const char *name, Value *out);
// Takes every remaining keyword argument: a dict of them, or NULL.
MyMoDict *takeAllKeywords(MVM *vm);

#endif
