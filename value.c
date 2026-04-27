#include "value.h"
#include "memory.h"
#include "datatypes/object.h"
#include "datatypes/int.h"

MyMoObject *valueToBoxedObject(MVM *vm, Value v) {
    if (V_IS_OBJ(v)) return V_AS_OBJ(v);
    if (V_IS_INT(v)) return AS_OBJECT(newInt(vm, V_AS_INT(v)));
    // Doubles, nil, true, false are still heap today.
    return V_AS_OBJ(v);
}

void initValueArray(MVM *vm, ValueArray *arr) {
    UNUSED(vm);
    arr->capacity = 0;
    arr->count = 0;
    arr->values = NULL;
}

void writeValueArray(MVM *vm, ValueArray *arr, Value v) {
    if (arr->capacity < arr->count + 1) {
        int oldCap = arr->capacity;
        arr->capacity = ResizeCapacity(oldCap);
        arr->values = ResizeArray(vm, Value, arr->values, oldCap, arr->capacity);
    }
    arr->values[arr->count++] = v;
}

void freeValueArray(MVM *vm, ValueArray *arr) {
    FreeArray(vm, Value, arr->values, arr->capacity);
    initValueArray(vm, arr);
}

bool valuesEqual(Value a, Value b) {
    // Fast path: identical bit patterns (covers nil, bool, same-tagged-int,
    // same heap pointer, +0.0 == +0.0). Has the IEEE-required quirk that
    // NaN != NaN, which we explicitly preserve.
    if (a == b) {
        if (V_IS_DOUBLE(a) && V_AS_DOUBLE(a) != V_AS_DOUBLE(a)) return false;
        return true;
    }
    if (V_IS_NUMBER(a) && V_IS_NUMBER(b)) {
        return V_AS_NUMBER(a) == V_AS_NUMBER(b);
    }
    // Mixed inline-int vs heap-MyMoInt comparison. Common during the
    // 1.3-1.6 migration window where one operand is inline (literal or
    // arithmetic result) and the other is heap (from a dict). After step
    // 1.6 deletes MyMoInt this branch becomes dead.
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b)) {
        return valueToLong(a) == valueToLong(b);
    }
    if (V_IS_OBJ(a) && V_IS_OBJ(b)) {
        return isEqual(V_AS_OBJ(a), V_AS_OBJ(b));
    }
    return false;
}

void printValue(Value v) {
    if (V_IS_NIL(v))    { printf("nil"); return; }
    if (V_IS_TRUE(v))   { printf("true"); return; }
    if (V_IS_FALSE(v))  { printf("false"); return; }
    if (V_IS_INT(v))    { printf("%d", V_AS_INT(v)); return; }
    if (V_IS_DOUBLE(v)) { printf("%g", V_AS_DOUBLE(v)); return; }
    if (V_IS_OBJ(v))    { printObject(V_AS_OBJ(v)); return; }
    printf("<unknown value 0x%016llx>", (unsigned long long)v);
}

const char *valueTypeName(Value v) {
    if (V_IS_NIL(v))    return "nil";
    if (V_IS_BOOL(v))   return "bool";
    if (V_IS_INT(v))    return "int";
    if (V_IS_DOUBLE(v)) return "double";
    if (V_IS_OBJ(v))    return getType(V_AS_OBJ(v));
    return "unknown";
}
