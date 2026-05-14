#include "value.h"
#include "memory.h"
#include "datatypes/object.h"
#include "datatypes/int.h"
#include "datatypes/double.h"
#include "datatypes/nil.h"
#include "datatypes/bool.h"

MyMoObject *valueToBoxedObject(MVM *vm, Value v) {
    if (V_IS_OBJ(v))    return V_AS_OBJ(v);
    if (V_IS_INT(v))    return AS_OBJECT(newInt(vm, V_AS_INT(v)));
    if (V_IS_DOUBLE(v)) return AS_OBJECT(newDouble(vm, V_AS_DOUBLE(v)));
    // Map the singleton tags back to the heap singletons the rest of
    // the VM already keeps around (NilObject / TrueBool / FalseBool).
    // Producers push inline V_*_VAL; legacy consumers that go through
    // pop(vm) → MyMoObject* get the singleton pointer here.
    if (V_IS_NIL(v))    return (MyMoObject *)NilObject;
    if (V_IS_TRUE(v))   return (MyMoObject *)TrueBool;
    if (V_IS_FALSE(v))  return (MyMoObject *)FalseBool;
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

// True iff `v` represents the language-level Nil — either the inline
// V_NIL_VAL bit pattern or a heap NilObject reached via V_AS_OBJ.
static inline bool valueIsNilAny(Value v) {
    if (V_IS_NIL(v)) return true;
    if (V_IS_OBJ(v)) {
        MyMoObject *o = V_AS_OBJ(v);
        return o && o->type == OBJ_NIL;
    }
    return false;
}

// Same idea for True/False — inline tag OR boxed singleton.
static inline bool valueIsBoolAny(Value v) {
    if (V_IS_BOOL(v)) return true;
    if (V_IS_OBJ(v)) {
        MyMoObject *o = V_AS_OBJ(v);
        return o && o->type == OBJ_BOOL;
    }
    return false;
}

static inline bool valueAsBoolBit(Value v) {
    if (V_IS_TRUE(v))  return true;
    if (V_IS_FALSE(v)) return false;
    return ((MyMoBool *)V_AS_OBJ(v))->value;
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
    // Same migration story for nil/bool: one side may be the inline
    // V_*_VAL tag, the other may be a NilObject / TrueBool / FalseBool
    // pointer that came from a dict or built-in. After step 1.7
    // collapses both forms these branches become dead too.
    if (valueIsNilAny(a) && valueIsNilAny(b)) return true;
    if (valueIsBoolAny(a) && valueIsBoolAny(b)) {
        return valueAsBoolBit(a) == valueAsBoolBit(b);
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
