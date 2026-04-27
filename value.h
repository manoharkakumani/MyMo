#ifndef __VALUE_H__
#define __VALUE_H__

#include "common.h"
#include "datatypes/object.h"

// NaN-boxed Value — single 64-bit slot that encodes:
//   - a 64-bit IEEE 754 double directly (any non-NaN bit pattern)
//   - a 32-bit signed int  (tagged QNaN payload)
//   - nil / true / false  (tagged QNaN payload, singleton encodings)
//   - a heap pointer to MyMoObject (sign-bit set + QNaN payload)
//
// This eliminates the heap allocation that the current MyMoInt / MyMoDouble /
// MyMoBool / MyMoNil types impose on every numeric value, and removes the
// vm->integers / vm->doubles / vm->numbers interning hash tables entirely.
//
// Macros are prefixed `V_` during the Phase 1 migration to avoid collision
// with the legacy object-pointer predicates in datatypes/object.h. Once every
// call site has migrated to Value, the prefixes will be dropped in a single
// global rename pass.

// MyMoObject already declared via "datatypes/object.h" above.

typedef uint64_t Value;

#define V_SIGN_BIT  ((uint64_t)0x8000000000000000)
#define V_QNAN      ((uint64_t)0x7ffc000000000000)

#define V_TAG_NIL   1ULL
#define V_TAG_FALSE 2ULL
#define V_TAG_TRUE  3ULL
#define V_TAG_INT   4ULL
#define V_TAG_MASK  0x7ULL

// ---- Type predicates -------------------------------------------------------

#define V_IS_DOUBLE(v) (((v) & V_QNAN) != V_QNAN)
#define V_IS_OBJ(v)    (((v) & (V_QNAN | V_SIGN_BIT)) == (V_QNAN | V_SIGN_BIT))
#define V_IS_NIL(v)    ((v) == (V_QNAN | V_TAG_NIL))
#define V_IS_FALSE(v)  ((v) == (V_QNAN | V_TAG_FALSE))
#define V_IS_TRUE(v)   ((v) == (V_QNAN | V_TAG_TRUE))
#define V_IS_BOOL(v)   (((v) | 1ULL) == (V_QNAN | V_TAG_TRUE))
#define V_IS_INT(v)    ((!V_IS_OBJ(v)) && (((v) & (V_QNAN | V_TAG_MASK)) == (V_QNAN | V_TAG_INT)))
#define V_IS_NUMBER(v) (V_IS_INT(v) || V_IS_DOUBLE(v))

// ---- Singletons ------------------------------------------------------------

#define V_NIL_VAL   ((Value)(V_QNAN | V_TAG_NIL))
#define V_FALSE_VAL ((Value)(V_QNAN | V_TAG_FALSE))
#define V_TRUE_VAL  ((Value)(V_QNAN | V_TAG_TRUE))
#define V_BOOL_VAL(b) ((b) ? V_TRUE_VAL : V_FALSE_VAL)

// ---- Int packing -----------------------------------------------------------
// 32-bit signed int packed into bits 35..4. Wider ints fall back to a heap
// MyMoObject for now; that escape hatch can be removed once we add a small-
// bignum object or commit to 32-bit-only semantics.

static inline Value V_INT_VAL_(int32_t i) {
    return V_QNAN | V_TAG_INT | (((uint64_t)(uint32_t)i) << 4);
}
static inline int32_t V_AS_INT_(Value v) {
    return (int32_t)(uint32_t)((v >> 4) & 0xffffffffULL);
}
#define V_INT_VAL(i) V_INT_VAL_((int32_t)(i))
#define V_AS_INT(v)  V_AS_INT_(v)

// ---- Double packing --------------------------------------------------------
// Type-pun via memcpy to stay strict-aliasing-clean. Compilers optimise this
// to a single mov on every relevant target.

static inline Value V_DOUBLE_VAL(double d) {
    Value v;
    memcpy(&v, &d, sizeof(double));
    return v;
}
static inline double V_AS_DOUBLE(Value v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

// ---- Object pointer packing ------------------------------------------------

#define V_OBJ_VAL(p) ((Value)(V_SIGN_BIT | V_QNAN | (uint64_t)(uintptr_t)(p)))
#define V_AS_OBJ(v)  ((MyMoObject *)(uintptr_t)((v) & ~(V_SIGN_BIT | V_QNAN)))

// Pull a number out as a double regardless of underlying tag. Useful in mixed
// arithmetic where the slow path coerces both sides.
static inline double V_AS_NUMBER(Value v) {
    return V_IS_INT(v) ? (double)V_AS_INT(v) : V_AS_DOUBLE(v);
}

// ---- Truthiness ------------------------------------------------------------
// Semantics matched to the existing MyMoBool conversion: nil and false are
// falsey, 0 / 0.0 are falsey, empty objects are object-defined (slow path).
static inline bool V_isFalsey(Value v) {
    if (V_IS_NIL(v) || V_IS_FALSE(v)) return true;
    if (V_IS_INT(v))    return V_AS_INT(v) == 0;
    if (V_IS_DOUBLE(v)) return V_AS_DOUBLE(v) == 0.0;
    return false;  // objects: defer to the object's bool conversion on slow path
}

// ---- Mixed-form int helpers ------------------------------------------------
// During the 1.3-1.6 migration, ints exist in two forms: inline V_INT_VAL
// (fast) and heap MyMoInt (legacy). Slow-path consumers (subscript,
// slicing, dict keys, etc.) need to accept both transparently. These helpers
// keep call sites readable without forcing a full migration of every
// int-consumer in this single step. They go away in step 1.6 when MyMoInt
// is deleted.

// Inlined predicates and accessors for the arithmetic OP fast path.
// `MyMoInt` is a forward-declaration here; the struct layout is in
// datatypes/int.h. We only need the `value` field offset, which the
// compiler can resolve once the full type is in scope at the use site.
typedef struct MyMoInt MyMoInt;
struct MyMoInt {
    MyMoObject object;
    long value;
    int length;
};

static inline bool valueLooksLikeInt(Value v) {
    if (V_IS_INT(v)) return true;
    if (V_IS_OBJ(v)) {
        MyMoObject *o = V_AS_OBJ(v);
        return o && o->type == OBJ_INT;
    }
    return false;
}

static inline long valueToLong(Value v) {
    if (V_IS_INT(v)) return (long)V_AS_INT(v);
    return ((MyMoInt *)V_AS_OBJ(v))->value;
}

// Allocates a MyMoInt for inline ints; legacy boxing helper.
MyMoObject *valueToBoxedObject(MVM *vm, Value v);

// ---- ValueArray (constant pools, list backing storage) ---------------------

typedef struct {
    int capacity;
    int count;
    Value *values;
} ValueArray;

void initValueArray(MVM *vm, ValueArray *arr);
void writeValueArray(MVM *vm, ValueArray *arr, Value v);
void freeValueArray(MVM *vm, ValueArray *arr);

// ---- Equality / printing ---------------------------------------------------

bool valuesEqual(Value a, Value b);
void printValue(Value v);
const char *valueTypeName(Value v);

#endif
