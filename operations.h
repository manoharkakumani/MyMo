#ifndef __OPERATIONS_H__
#define __OPERATIONS_H__

#include "datatypes/object.h"
#include "datatypes/string.h"
#include "value.h"

int compareStrings(MyMoString *a, MyMoString *b);

// Slow paths of + - * / on Values; V_EMPTY_VAL after raising an error.
Value addValues(MVM *vm, Value a, Value b);
Value subValues(MVM *vm, Value a, Value b);
Value mulValues(MVM *vm, Value a, Value b);
Value divValues(MVM *vm, Value a, Value b);

// `item in container` for built-in containers: V_TRUE_VAL/V_FALSE_VAL,
// or V_EMPTY_VAL after raising (container type not supported).
Value containsValue(MVM *vm, Value container, Value item);

#endif
