#include "operations.h"
#include "memory.h"
#include "vm.h"
#include "datatypes/datatypes.h"
#include "include/mymo.h"

// Slow paths of + - * / (the dispatch loop handles plain numbers inline
// first). Operands and results are Values; errors raise and return
// V_EMPTY_VAL.
//
// Language rules kept here: two bools combine logically (+ is or, - is
// "a and not b", * and / are and); a bool mixed with a number counts as
// 0/1; str + str concatenates; list + list and tuple + tuple concatenate;
// str * int repeats.

// Bytewise lexicographic order (like strcmp, but length-aware so
// embedded NULs compare correctly): <0, 0 or >0.
int compareStrings(MyMoString *a, MyMoString *b)
{
    int n = a->length < b->length ? a->length : b->length;
    int c = memcmp(a->value, b->value, (size_t)n);
    if (c != 0)
        return c;
    return a->length - b->length;
}

// New sequence holding a's elements followed by b's (list + list,
// tuple + tuple). Operands are left untouched.
static void concatArrays(MVM *vm, ValueArray *out, ValueArray *a, ValueArray *b)
{
    for (int i = 0; i < a->count; i++)
        writeValueArray(vm, out, a->values[i]);
    for (int i = 0; i < b->count; i++)
        writeValueArray(vm, out, b->values[i]);
}

// `s * n`: s repeated n times ("" for n <= 0).
static Value repeatString(MVM *vm, MyMoString *s, long n)
{
    long length = n > 0 ? (long)s->length * n : 0;
    char *chars = New(char, length + 1);
    for (long i = 0; i < length; i++)
        chars[i] = s->value[i % s->length];
    chars[length] = '\0';
    MyMoObject *result = NEW_STRING(vm, chars, (int)length);
    free(chars);
    return V_OBJ_VAL(result);
}

// A bool used in arithmetic next to a number is 0 or 1.
static Value boolAsInt(Value v)
{
    return valueIsBool(v) ? V_INT_VAL(valueAsBool(v) ? 1 : 0) : v;
}

static Value typeError(MVM *vm, const char *op, Value a, Value b)
{
    runtimeError(vm, "TypeError: can't perform %s between  %s and %s", op, valueTypeName(a), valueTypeName(b));
    return V_EMPTY_VAL;
}

static bool isType(Value v, MyMoObjectType t) { return V_IS_OBJ_TYPE(v, t); }

Value addValues(MVM *vm, Value a, Value b)
{
    if (valueIsBool(a) && valueIsBool(b))
        return V_BOOL_VAL(valueAsBool(a) || valueAsBool(b));
    a = boolAsInt(a);
    b = boolAsInt(b);
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
        return valueFromLong(vm, valueToLong(a) + valueToLong(b));
    if (valueLooksLikeNumber(a) && valueLooksLikeNumber(b))
        return V_DOUBLE_VAL(valueAsNumber(a) + valueAsNumber(b));
    if (isType(a, OBJ_STRING) && isType(b, OBJ_STRING))
    {
        MyMoString *x = AS_STRING(V_AS_OBJ(a)), *y = AS_STRING(V_AS_OBJ(b));
        int length = x->length + y->length;
        char *chars = New(char, length + 1);
        memcpy(chars, x->value, (size_t)x->length);
        memcpy(chars + x->length, y->value, (size_t)y->length);
        chars[length] = '\0';
        MyMoObject *result = NEW_STRING(vm, chars, length);
        free(chars);
        return V_OBJ_VAL(result);
    }
    if (isType(a, OBJ_LIST) && isType(b, OBJ_LIST))
    {
        MyMoList *out = newList(vm);
        concatArrays(vm, &out->values, &AS_LIST(V_AS_OBJ(a))->values, &AS_LIST(V_AS_OBJ(b))->values);
        return V_OBJ_VAL(AS_OBJECT(out));
    }
    if (isType(a, OBJ_TUPLE) && isType(b, OBJ_TUPLE))
    {
        MyMoTuple *out = newTuple(vm);
        concatArrays(vm, &out->values, &AS_TUPLE(V_AS_OBJ(a))->values, &AS_TUPLE(V_AS_OBJ(b))->values);
        return V_OBJ_VAL(AS_OBJECT(out));
    }
    return typeError(vm, "+", a, b);
}

Value subValues(MVM *vm, Value a, Value b)
{
    if (valueIsBool(a) && valueIsBool(b))
        return V_BOOL_VAL(valueAsBool(a) && !valueAsBool(b));
    a = boolAsInt(a);
    b = boolAsInt(b);
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
        return valueFromLong(vm, valueToLong(a) - valueToLong(b));
    if (valueLooksLikeNumber(a) && valueLooksLikeNumber(b))
        return V_DOUBLE_VAL(valueAsNumber(a) - valueAsNumber(b));
    if (isType(a, OBJ_SET) && isType(b, OBJ_SET))
        return V_OBJ_VAL(AS_OBJECT(setCombine(vm, AS_SET(V_AS_OBJ(a)), AS_SET(V_AS_OBJ(b)), '-')));
    return typeError(vm, "-", a, b);
}

Value mulValues(MVM *vm, Value a, Value b)
{
    if (valueIsBool(a) && valueIsBool(b))
        return V_BOOL_VAL(valueAsBool(a) && valueAsBool(b));
    a = boolAsInt(a);
    b = boolAsInt(b);
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
    {
        long r;
        if (!__builtin_mul_overflow(valueToLong(a), valueToLong(b), &r))
            return valueFromLong(vm, r);
        return V_DOUBLE_VAL(valueAsNumber(a) * valueAsNumber(b));
    }
    if (valueLooksLikeNumber(a) && valueLooksLikeNumber(b))
        return V_DOUBLE_VAL(valueAsNumber(a) * valueAsNumber(b));
    if (isType(a, OBJ_STRING) && valueLooksLikeInt(b))
        return repeatString(vm, AS_STRING(V_AS_OBJ(a)), valueToLong(b));
    if (valueLooksLikeInt(a) && isType(b, OBJ_STRING))
        return repeatString(vm, AS_STRING(V_AS_OBJ(b)), valueToLong(a));
    return typeError(vm, "*", a, b);
}

Value divValues(MVM *vm, Value a, Value b)
{
    if (valueIsBool(a) && valueIsBool(b))
        return V_BOOL_VAL(valueAsBool(a) && valueAsBool(b));
    a = boolAsInt(a);
    b = boolAsInt(b);
    if (valueLooksLikeNumber(a) && valueLooksLikeNumber(b))
    {
        double d = valueAsNumber(b);
        if (d == 0)
        {
            runtimeError(vm, "ZeroDivisionError: division by zero.");
            return V_EMPTY_VAL;
        }
        double r = valueAsNumber(a) / d;
        // int / int stays an int when the result is whole (6 / 2 == 3).
        if (valueLooksLikeInt(a) && valueLooksLikeInt(b) && r == (long)r)
            return valueFromLong(vm, (long)r);
        return V_DOUBLE_VAL(r);
    }
    return typeError(vm, "/", a, b);
}

Value containsValue(MVM *vm, Value container, Value item)
{
    if (V_IS_OBJ(container))
    {
        MyMoObject *c = V_AS_OBJ(container);
        switch (c->type)
        {
        case OBJ_LIST:
        case OBJ_TUPLE:
        {
            ValueArray *values = c->type == OBJ_LIST ? &AS_LIST(c)->values : &AS_TUPLE(c)->values;
            for (int i = 0; i < values->count; i++)
                if (valuesEqual(values->values[i], item))
                    return V_TRUE_VAL;
            return V_FALSE_VAL;
        }
        case OBJ_STRING:
        {
            if (!isType(item, OBJ_STRING))
            {
                runtimeError(vm, "TypeError: 'in <string>' requires a string, not %s", valueTypeName(item));
                return V_EMPTY_VAL;
            }
            MyMoString *hay = AS_STRING(c), *needle = AS_STRING(V_AS_OBJ(item));
            if (needle->length == 0)
                return V_TRUE_VAL;
            for (int i = 0; i + needle->length <= hay->length; i++)
                if (memcmp(hay->value + i, needle->value, (size_t)needle->length) == 0)
                    return V_TRUE_VAL;
            return V_FALSE_VAL;
        }
        case OBJ_SET:
            return V_BOOL_VAL(setHas(vm, AS_SET(c), item));
        case OBJ_RANGE:
            if (!valueLooksLikeInt(item))
                return V_FALSE_VAL;
            return V_BOOL_VAL(rangeContains(AS_RANGE(c), valueToLong(item)));
        case OBJ_DICT:
        {
            // Keys are interned objects; box inline keys to find them.
            Value unused;
            return V_BOOL_VAL(getEntryV(AS_DICT(c), dictLookupKey(vm, item), &unused));
        }
        default:
            break;
        }
    }
    runtimeError(vm, "TypeError: argument of type %s is not iterable", valueTypeName(container));
    return V_EMPTY_VAL;
}

static bool isSequence(Value v)
{
    return V_IS_OBJ_TYPE(v, OBJ_LIST) || V_IS_OBJ_TYPE(v, OBJ_TUPLE);
}

static ValueArray *sequenceValues(Value v)
{
    MyMoObject *o = V_AS_OBJ(v);
    return o->type == OBJ_LIST ? &AS_LIST(o)->values : &AS_TUPLE(o)->values;
}

bool lessThan(MVM *vm, Value a, Value b, bool *out)
{
    a = boolAsInt(a);
    b = boolAsInt(b);
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
    {
        *out = valueToLong(a) < valueToLong(b);
        return true;
    }
    if (valueLooksLikeNumber(a) && valueLooksLikeNumber(b))
    {
        *out = valueAsNumber(a) < valueAsNumber(b);
        return true;
    }
    if (isType(a, OBJ_STRING) && isType(b, OBJ_STRING))
    {
        *out = compareStrings(AS_STRING(V_AS_OBJ(a)), AS_STRING(V_AS_OBJ(b))) < 0;
        return true;
    }
    if (isSequence(a) && isSequence(b) && V_AS_OBJ(a)->type == V_AS_OBJ(b)->type)
    {
        // Lexicographic: the first unequal pair decides, else the shorter.
        ValueArray *x = sequenceValues(a), *y = sequenceValues(b);
        for (int i = 0; i < x->count && i < y->count; i++)
            if (!valuesEqual(x->values[i], y->values[i]))
                return lessThan(vm, x->values[i], y->values[i], out);
        *out = x->count < y->count;
        return true;
    }
    if (isType(a, OBJ_INSTANCE))
    {
        MyMoObject *method = getMethod(vm, V_AS_OBJ(a), "<");
        if (!IS_EMPTY(method))
        {
            Value args[2] = {a, b}, result;
            if (mymo_call(vm, V_OBJ_VAL(method), 2, args, &result) != MYMO_OK)
                return false;
            *out = !valueIsFalsey(result);
            return true;
        }
    }
    runtimeError(vm, "TypeError: '<' not supported between %s and %s", valueTypeName(a), valueTypeName(b));
    return false;
}
