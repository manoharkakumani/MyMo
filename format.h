#ifndef __FORMAT_H__
#define __FORMAT_H__

#include "common.h"
#include "value.h"
#include "repr.h"
#include "datatypes/string.h"
#include "datatypes/dict.h"

// Python-style format specs ("08.3f", ">10", ",d", "#x", ...); see
// format.c. False / V_EMPTY_VAL after raising a ValueError.
bool formatWithSpec(MVM *vm, StrBuf *out, Value v, const char *spec, int specLen);
Value formatValueToString(MVM *vm, Value v, const char *spec, int specLen);

// str.format(): fills "{}", "{0}", "{name}", "{x!r}", "{:spec}" fields.
Value formatTemplate(MVM *vm, MyMoString *pattern, int argc, Value *args, MyMoDict *keywords);

// printf-style "fmt % args": args is a tuple, a single value, or a dict
// for %(name)s fields.
Value percentFormat(MVM *vm, MyMoString *fmt, Value args);

#endif
