// builtins.c — general-purpose builtin functions: iteration helpers
// (range lives in datatypes/range.c), numeric helpers, conversions and
// type checks. print/len/type/input and friends are in utils.c.
//
// Every function follows the builtin contract: it receives its
// arguments in argv, pops them from the operand stack once it has
// succeeded, and returns the result (V_EMPTY_VAL after raising).
// Callbacks (key functions, map/filter predicates) run through
// mymo_call; the collector never runs inside a nested call, so values
// held in C arrays here stay valid.

#include "builtins.h"
#include "vm.h"
#include "memory.h"
#include "operations.h"
#include "format.h"
#include "include/mymo.h"
#include "datatypes/datatypes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void popArgs(MVM *vm, uint argc)
{
    for (uint i = 0; i < argc; i++)
        popV(vm);
}

static bool arity(MVM *vm, const char *fn, uint argc, uint min, uint max)
{
    if (argc >= min && argc <= max)
        return true;
    if (min == max)
        runtimeError(vm, "TypeError: %s() takes %u argument%s (%u given)", fn, min, min == 1 ? "" : "s", argc);
    else
        runtimeError(vm, "TypeError: %s() takes %u to %u arguments (%u given)", fn, min, max, argc);
    return false;
}

static bool needInt(MVM *vm, const char *fn, Value v, long *out)
{
    if (valueLooksLikeInt(v))
    {
        *out = valueToLong(v);
        return true;
    }
    if (valueIsBool(v))
    {
        *out = valueAsBool(v);
        return true;
    }
    runtimeError(vm, "TypeError: %s() expects an integer, got %s", fn, valueTypeName(v));
    return false;
}

static bool needNumber(MVM *vm, const char *fn, Value v)
{
    if (valueLooksLikeNumber(v))
        return true;
    runtimeError(vm, "TypeError: %s() expects a number, got %s", fn, valueTypeName(v));
    return false;
}

static Value listValue(MyMoList *list) { return V_OBJ_VAL(AS_OBJECT(list)); }

MyMoDict *takeAllKeywords(MVM *vm)
{
    MyMoDict *keywords = vm->kwargs;
    if (keywords == NULL || keywords->count == 0)
        return NULL;
    MyMoDict *taken = newDict(vm);
    copyDict(vm, keywords, taken);
    FreeArray(vm, Entry, keywords->entries, keywords->capacity + 1);
    initDict(keywords);
    return taken;
}

bool takeKeyword(MVM *vm, const char *name, Value *out)
{
    if (vm->kwargs == NULL || vm->kwargs->count == 0)
        return false;
    MyMoObject *key = AS_OBJECT(newString(vm, name, (int)strlen(name)));
    if (!getEntryV(vm->kwargs, key, out))
        return false;
    deleteEntry(vm, vm->kwargs, key);
    return true;
}

// Appends the elements of an iterable (list, tuple, string characters,
// dict keys, range) to `out`.
bool appendIterable(MVM *vm, const char *fn, Value v, ValueArray *out)
{
    if (V_IS_OBJ(v))
    {
        MyMoObject *o = V_AS_OBJ(v);
        switch (o->type)
        {
        case OBJ_LIST:
        case OBJ_TUPLE:
        {
            ValueArray *values = o->type == OBJ_LIST ? &AS_LIST(o)->values : &AS_TUPLE(o)->values;
            int count = values->count; // `out` may be `values` (xs.extend(xs))
            for (int i = 0; i < count; i++)
                writeValueArray(vm, out, values->values[i]);
            return true;
        }
        case OBJ_STRING:
        {
            MyMoString *s = AS_STRING(o);
            for (int i = 0; i < s->length; i++)
                writeValueArray(vm, out, V_OBJ_VAL(AS_OBJECT(newString(vm, s->value + i, 1))));
            return true;
        }
        case OBJ_DICT:
        case OBJ_SET:
        {
            Entry *e;
            DICT_FOREACH(o->type == OBJ_SET ? &AS_SET(o)->items : AS_DICT(o), e)
                writeValueArray(vm, out, objectToValue(e->key));
            return true;
        }
        case OBJ_RANGE:
        {
            MyMoRange *r = AS_RANGE(o);
            long n = rangeLength(r);
            for (long i = 0; i < n; i++)
                writeValueArray(vm, out, valueFromLong(vm, rangeAt(r, i)));
            return true;
        }
        default:
            break;
        }
    }
    runtimeError(vm, "TypeError: %s() expects an iterable, got %s", fn, valueTypeName(v));
    return false;
}

// A new list holding the iterable's elements, or NULL after raising.
static MyMoList *iterableToList(MVM *vm, const char *fn, Value v)
{
    MyMoList *list = newList(vm);
    if (!appendIterable(vm, fn, v, &list->values))
        return NULL;
    return list;
}

static bool call1(MVM *vm, Value fn, Value arg, Value *out)
{
    return mymo_call(vm, fn, 1, &arg, out) == MYMO_OK;
}

// ---------------------------------------------------------------- sorting

typedef struct
{
    Value key;
    Value value;
} SortItem;

// Stable merge sort on keys. `reverse` flips the order but keeps equal
// elements in their original order (like Python).
static bool mergeSort(MVM *vm, SortItem *items, SortItem *tmp, int n, bool reverse)
{
    if (n < 2)
        return true;
    int mid = n / 2;
    if (!mergeSort(vm, items, tmp, mid, reverse) || !mergeSort(vm, items + mid, tmp, n - mid, reverse))
        return false;
    int i = 0, j = mid, k = 0;
    while (i < mid && j < n)
    {
        bool takeRight;
        bool ok = reverse ? lessThan(vm, items[i].key, items[j].key, &takeRight)
                          : lessThan(vm, items[j].key, items[i].key, &takeRight);
        if (!ok)
            return false;
        tmp[k++] = takeRight ? items[j++] : items[i++];
    }
    while (i < mid)
        tmp[k++] = items[i++];
    while (j < n)
        tmp[k++] = items[j++];
    memcpy(items, tmp, sizeof(SortItem) * (size_t)n);
    return true;
}

bool sortValues(MVM *vm, ValueArray *values, Value key, bool reverse)
{
    int n = values->count;
    if (n < 2)
        return true;
    SortItem *items = malloc(sizeof(SortItem) * (size_t)n * 2);
    if (items == NULL)
    {
        runtimeError(vm, "MemoryError: out of memory in sort");
        return false;
    }
    bool hasKey = !valueIsNil(key);
    for (int i = 0; i < n; i++)
    {
        items[i].value = values->values[i];
        items[i].key = values->values[i];
        if (hasKey && !call1(vm, key, values->values[i], &items[i].key))
        {
            free(items);
            return false;
        }
    }
    bool ok = mergeSort(vm, items, items + n, n, reverse);
    if (ok)
    {
        // The key function may have resized the list; write back what fits.
        for (int i = 0; i < n && i < values->count; i++)
            values->values[i] = items[i].value;
    }
    free(items);
    return ok;
}

// sorted(iterable[, key[, reverse]]) — key may be Nil; key= and
// reverse= may also be given by name.
static Value sortedfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "sorted", argc, 1, 3))
        return V_EMPTY_VAL;
    Value key = argc > 1 ? argv[1] : V_NIL_VAL;
    Value reverseV = argc > 2 ? argv[2] : V_FALSE_VAL;
    takeKeyword(vm, "key", &key);
    takeKeyword(vm, "reverse", &reverseV);
    bool reverse = !valueIsFalsey(reverseV);
    MyMoList *list = iterableToList(vm, "sorted", argv[0]);
    if (!list)
        return V_EMPTY_VAL;
    pushV(vm, listValue(list)); // keep it rooted while key functions run
    bool ok = sortValues(vm, &list->values, key, reverse);
    popV(vm);
    if (!ok)
        return V_EMPTY_VAL;
    popArgs(vm, argc);
    return listValue(list);
}

// ------------------------------------------------------------ aggregates

// min/max over either the arguments or a single iterable argument.
// key= compares key(x) instead of x; default= is returned for an empty
// iterable instead of raising.
static Value extremum(MVM *vm, const char *fn, uint argc, Value argv[], bool wantMax)
{
    Value key = V_NIL_VAL, fallback = V_EMPTY_VAL;
    takeKeyword(vm, "key", &key);
    bool hasDefault = takeKeyword(vm, "default", &fallback);
    if (argc == 0)
    {
        runtimeError(vm, "TypeError: %s() expects at least 1 argument", fn);
        return V_EMPTY_VAL;
    }
    ValueArray items;
    MyMoList *list = NULL;
    if (argc == 1)
    {
        list = iterableToList(vm, fn, argv[0]);
        if (!list)
            return V_EMPTY_VAL;
        items = list->values;
    }
    else
    {
        items.values = argv;
        items.count = (int)argc;
    }
    if (items.count == 0)
    {
        if (hasDefault)
        {
            popArgs(vm, argc);
            return fallback;
        }
        runtimeError(vm, "ValueError: %s() of an empty sequence", fn);
        return V_EMPTY_VAL;
    }
    bool hasKey = !valueIsNil(key);
    Value best = items.values[0], bestKey = best;
    if (hasKey && !call1(vm, key, best, &bestKey))
        return V_EMPTY_VAL;
    for (int i = 1; i < items.count; i++)
    {
        Value candidateKey = items.values[i];
        if (hasKey && !call1(vm, key, items.values[i], &candidateKey))
            return V_EMPTY_VAL;
        bool better;
        bool ok = wantMax ? lessThan(vm, bestKey, candidateKey, &better)
                          : lessThan(vm, candidateKey, bestKey, &better);
        if (!ok)
            return V_EMPTY_VAL;
        if (better)
        {
            best = items.values[i];
            bestKey = candidateKey;
        }
    }
    popArgs(vm, argc);
    return best;
}

static Value minfn(MVM *vm, uint argc, Value argv[]) { return extremum(vm, "min", argc, argv, false); }
static Value maxfn(MVM *vm, uint argc, Value argv[]) { return extremum(vm, "max", argc, argv, true); }

// sum(iterable[, start])
static Value sumfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "sum", argc, 1, 2))
        return V_EMPTY_VAL;
    MyMoList *list = iterableToList(vm, "sum", argv[0]);
    if (!list)
        return V_EMPTY_VAL;
    Value total = argc > 1 ? argv[1] : V_INT_VAL(0);
    for (int i = 0; i < list->values.count; i++)
    {
        Value x = list->values.values[i];
        if (V_IS_INT(total) && V_IS_INT(x))
            total = valueFromLong(vm, (long)V_AS_INT(total) + V_AS_INT(x));
        else
            total = addValues(vm, total, x);
        if (V_IS_EMPTY(total))
            return V_EMPTY_VAL;
    }
    popArgs(vm, argc);
    return total;
}

static Value truthTest(MVM *vm, const char *fn, uint argc, Value argv[], bool wantAll)
{
    if (!arity(vm, fn, argc, 1, 1))
        return V_EMPTY_VAL;
    MyMoList *list = iterableToList(vm, fn, argv[0]);
    if (!list)
        return V_EMPTY_VAL;
    bool result = wantAll;
    for (int i = 0; i < list->values.count; i++)
        if (valueIsFalsey(list->values.values[i]) == wantAll)
        {
            result = !wantAll;
            break;
        }
    popArgs(vm, argc);
    return V_BOOL_VAL(result);
}

static Value anyfn(MVM *vm, uint argc, Value argv[]) { return truthTest(vm, "any", argc, argv, false); }
static Value allfn(MVM *vm, uint argc, Value argv[]) { return truthTest(vm, "all", argc, argv, true); }

// ------------------------------------------------- sequence transformers

static Value reversedfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "reversed", argc, 1, 1))
        return V_EMPTY_VAL;
    MyMoList *list = iterableToList(vm, "reversed", argv[0]);
    if (!list)
        return V_EMPTY_VAL;
    Value *v = list->values.values;
    for (int i = 0, j = list->values.count - 1; i < j; i++, j--)
    {
        Value t = v[i];
        v[i] = v[j];
        v[j] = t;
    }
    popArgs(vm, argc);
    return listValue(list);
}

static MyMoTuple *pair(MVM *vm, Value a, Value b)
{
    MyMoTuple *t = newTuple(vm);
    writeValueArray(vm, &t->values, a);
    writeValueArray(vm, &t->values, b);
    return t;
}

// enumerate(iterable[, start]) -> [(start, x0), (start + 1, x1), ...]
static Value enumeratefn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "enumerate", argc, 1, 2))
        return V_EMPTY_VAL;
    long start = 0;
    Value startV = argc > 1 ? argv[1] : V_INT_VAL(0);
    takeKeyword(vm, "start", &startV);
    if (!needInt(vm, "enumerate", startV, &start))
        return V_EMPTY_VAL;
    MyMoList *list = iterableToList(vm, "enumerate", argv[0]);
    if (!list)
        return V_EMPTY_VAL;
    for (int i = 0; i < list->values.count; i++)
    {
        Value x = list->values.values[i];
        list->values.values[i] = V_OBJ_VAL(AS_OBJECT(pair(vm, valueFromLong(vm, start + i), x)));
    }
    popArgs(vm, argc);
    return listValue(list);
}

// zip(a, b, ...) -> [(a0, b0, ...), ...], as long as the shortest input.
static Value zipfn(MVM *vm, uint argc, Value argv[])
{
    MyMoList *out = newList(vm);
    if (argc == 0)
        return listValue(out);
    MyMoList **inputs = malloc(sizeof(MyMoList *) * argc);
    int shortest = -1;
    for (uint i = 0; i < argc; i++)
    {
        inputs[i] = iterableToList(vm, "zip", argv[i]);
        if (!inputs[i])
        {
            free(inputs);
            return V_EMPTY_VAL;
        }
        if (shortest < 0 || inputs[i]->values.count < shortest)
            shortest = inputs[i]->values.count;
    }
    for (int row = 0; row < shortest; row++)
    {
        MyMoTuple *t = newTuple(vm);
        for (uint i = 0; i < argc; i++)
            writeValueArray(vm, &t->values, inputs[i]->values.values[row]);
        writeValueArray(vm, &out->values, V_OBJ_VAL(AS_OBJECT(t)));
    }
    free(inputs);
    popArgs(vm, argc);
    return listValue(out);
}

// map(fn, iterable, ...) -> [fn(a0, b0, ...), ...]
static Value mapfn(MVM *vm, uint argc, Value argv[])
{
    if (argc < 2)
    {
        runtimeError(vm, "TypeError: map() takes a function and at least 1 iterable (%u given)", argc);
        return V_EMPTY_VAL;
    }
    uint n = argc - 1;
    MyMoList **inputs = malloc(sizeof(MyMoList *) * n);
    Value *args = malloc(sizeof(Value) * n);
    int shortest = -1;
    for (uint i = 0; i < n; i++)
    {
        inputs[i] = iterableToList(vm, "map", argv[i + 1]);
        if (!inputs[i])
            goto fail;
        if (shortest < 0 || inputs[i]->values.count < shortest)
            shortest = inputs[i]->values.count;
    }
    MyMoList *out = newList(vm);
    for (int row = 0; row < shortest; row++)
    {
        for (uint i = 0; i < n; i++)
            args[i] = inputs[i]->values.values[row];
        Value result;
        if (mymo_call(vm, argv[0], (int)n, args, &result) != MYMO_OK)
            goto fail;
        writeValueArray(vm, &out->values, result);
    }
    free(inputs);
    free(args);
    popArgs(vm, argc);
    return listValue(out);
fail:
    free(inputs);
    free(args);
    return V_EMPTY_VAL;
}

// filter(fn, iterable) -> the elements for which fn(x) is truthy
// (fn may be Nil: keep the truthy elements themselves).
static Value filterfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "filter", argc, 2, 2))
        return V_EMPTY_VAL;
    MyMoList *items = iterableToList(vm, "filter", argv[1]);
    if (!items)
        return V_EMPTY_VAL;
    MyMoList *out = newList(vm);
    for (int i = 0; i < items->values.count; i++)
    {
        Value x = items->values.values[i], keep = x;
        if (!valueIsNil(argv[0]) && !call1(vm, argv[0], x, &keep))
            return V_EMPTY_VAL;
        if (!valueIsFalsey(keep))
            writeValueArray(vm, &out->values, x);
    }
    popArgs(vm, argc);
    return listValue(out);
}

// ---------------------------------------------------------------- numbers

static Value absfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "abs", argc, 1, 1) || !needNumber(vm, "abs", argv[0]))
        return V_EMPTY_VAL;
    Value x = argv[0];
    popArgs(vm, argc);
    if (valueLooksLikeInt(x))
    {
        long n = valueToLong(x);
        return valueFromLong(vm, n < 0 ? -n : n);
    }
    return V_DOUBLE_VAL(fabs(valueAsNumber(x)));
}

// round(x) -> nearest int (ties to even, like Python);
// round(x, digits) -> a double rounded to that many decimals.
static Value roundfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "round", argc, 1, 2) || !needNumber(vm, "round", argv[0]))
        return V_EMPTY_VAL;
    long digits = 0;
    if (argc == 2 && !needInt(vm, "round", argv[1], &digits))
        return V_EMPTY_VAL;
    Value x = argv[0];
    popArgs(vm, argc);
    if (valueLooksLikeInt(x))
        return x;
    double d = valueAsNumber(x);
    if (argc == 1)
    {
        double r = nearbyint(d);
        if (!isfinite(r) || fabs(r) > 9.2e18)
        {
            runtimeError(vm, "OverflowError: cannot round %g to an integer", d);
            return V_EMPTY_VAL;
        }
        return valueFromLong(vm, (long)r);
    }
    if (digits >= 0 && digits < 300)
    {
        // printf rounds the exact binary value correctly (2.675 is
        // really 2.67499999..., so it rounds to 2.67 like Python).
        char buf[400];
        snprintf(buf, sizeof(buf), "%.*f", (int)digits, d);
        return V_DOUBLE_VAL(strtod(buf, NULL));
    }
    double scale = pow(10.0, (double)digits);
    return V_DOUBLE_VAL(nearbyint(d * scale) / scale);
}

// Floor division and modulo (the sign of the result follows the divisor).
static Value divmodfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "divmod", argc, 2, 2) || !needNumber(vm, "divmod", argv[0]) || !needNumber(vm, "divmod", argv[1]))
        return V_EMPTY_VAL;
    Value a = argv[0], b = argv[1];
    if (valueAsNumber(b) == 0)
    {
        runtimeError(vm, "ZeroDivisionError: divmod() by zero");
        return V_EMPTY_VAL;
    }
    MyMoTuple *result;
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
    {
        long x = valueToLong(a), y = valueToLong(b);
        long q = x / y, r = x % y;
        if (r != 0 && ((r < 0) != (y < 0)))
        {
            q -= 1;
            r += y;
        }
        result = pair(vm, valueFromLong(vm, q), valueFromLong(vm, r));
    }
    else
    {
        double x = valueAsNumber(a), y = valueAsNumber(b);
        double q = floor(x / y), r = x - q * y;
        result = pair(vm, V_DOUBLE_VAL(q), V_DOUBLE_VAL(r));
    }
    popArgs(vm, argc);
    return V_OBJ_VAL(AS_OBJECT(result));
}

// pow(a, b[, mod])
static Value powfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "pow", argc, 2, 3) || !needNumber(vm, "pow", argv[0]) || !needNumber(vm, "pow", argv[1]))
        return V_EMPTY_VAL;
    Value a = argv[0], b = argv[1];
    if (argc == 3)
    {
        long base, exp, mod;
        if (!needInt(vm, "pow", a, &base) || !needInt(vm, "pow", b, &exp) || !needInt(vm, "pow", argv[2], &mod))
            return V_EMPTY_VAL;
        if (mod == 0 || exp < 0)
        {
            runtimeError(vm, "ValueError: pow() with a modulus needs a non-zero modulus and exponent >= 0");
            return V_EMPTY_VAL;
        }
        __int128 result = 1, x = ((base % mod) + mod) % mod;
        while (exp > 0)
        {
            if (exp & 1)
                result = result * x % mod;
            x = x * x % mod;
            exp >>= 1;
        }
        popArgs(vm, argc);
        return valueFromLong(vm, (long)result);
    }
    popArgs(vm, argc);
    if (valueLooksLikeInt(a) && valueLooksLikeInt(b) && valueToLong(b) >= 0)
    {
        long base = valueToLong(a), exp = valueToLong(b), result = 1;
        bool overflow = false;
        while (exp > 0 && !overflow)
        {
            if (exp & 1)
                overflow |= __builtin_mul_overflow(result, base, &result);
            exp >>= 1;
            if (exp)
                overflow |= __builtin_mul_overflow(base, base, &base);
        }
        if (!overflow)
            return valueFromLong(vm, result);
    }
    return V_DOUBLE_VAL(pow(valueAsNumber(a), valueAsNumber(b)));
}

// hex/oct/bin: "0x1f", "-0o17", "0b101"
static Value radixString(MVM *vm, const char *fn, uint argc, Value argv[], int base, const char *prefix)
{
    long n;
    if (!arity(vm, fn, argc, 1, 1) || !needInt(vm, fn, argv[0], &n))
        return V_EMPTY_VAL;
    popArgs(vm, argc);
    char digits[80];
    int len = 0;
    unsigned long u = n < 0 ? 0UL - (unsigned long)n : (unsigned long)n;
    do
    {
        digits[len++] = "0123456789abcdef"[u % (unsigned long)base];
        u /= (unsigned long)base;
    } while (u);
    char out[96];
    int k = 0;
    if (n < 0)
        out[k++] = '-';
    for (const char *p = prefix; *p; p++)
        out[k++] = *p;
    while (len)
        out[k++] = digits[--len];
    return V_OBJ_VAL(AS_OBJECT(newString(vm, out, k)));
}

static Value hexfn(MVM *vm, uint argc, Value argv[]) { return radixString(vm, "hex", argc, argv, 16, "0x"); }
static Value octfn(MVM *vm, uint argc, Value argv[]) { return radixString(vm, "oct", argc, argv, 8, "0o"); }
static Value binfn(MVM *vm, uint argc, Value argv[]) { return radixString(vm, "bin", argc, argv, 2, "0b"); }

// ------------------------------------------------------------ characters

// ord("A") -> 65; a multi-byte UTF-8 character gives its code point.
static Value ordfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "ord", argc, 1, 1))
        return V_EMPTY_VAL;
    if (!V_IS_OBJ_TYPE(argv[0], OBJ_STRING))
    {
        runtimeError(vm, "TypeError: ord() expects a string, got %s", valueTypeName(argv[0]));
        return V_EMPTY_VAL;
    }
    MyMoString *s = AS_STRING(V_AS_OBJ(argv[0]));
    const unsigned char *p = (const unsigned char *)s->value;
    int need = s->length == 0 ? 0 : p[0] < 0x80 ? 1 : (p[0] >> 5) == 6 ? 2 : (p[0] >> 4) == 14 ? 3 : (p[0] >> 3) == 30 ? 4 : 0;
    if (need == 0 || need != s->length)
    {
        runtimeError(vm, "TypeError: ord() expects a single character, got a string of length %d", s->length);
        return V_EMPTY_VAL;
    }
    long cp = need == 1 ? p[0] : p[0] & (0x7f >> need);
    for (int i = 1; i < need; i++)
        cp = (cp << 6) | (p[i] & 0x3f);
    popArgs(vm, argc);
    return valueFromLong(vm, cp);
}

// chr(65) -> "A"; code points above 127 are encoded as UTF-8.
static Value chrfn(MVM *vm, uint argc, Value argv[])
{
    long cp;
    if (!arity(vm, "chr", argc, 1, 1) || !needInt(vm, "chr", argv[0], &cp))
        return V_EMPTY_VAL;
    if (cp < 0 || cp > 0x10FFFF)
    {
        runtimeError(vm, "ValueError: chr() argument %ld is not a valid code point", cp);
        return V_EMPTY_VAL;
    }
    char buf[4];
    int n;
    if (cp < 0x80)
    {
        buf[0] = (char)cp;
        n = 1;
    }
    else if (cp < 0x800)
    {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    }
    else if (cp < 0x10000)
    {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    }
    else
    {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    popArgs(vm, argc);
    return V_OBJ_VAL(AS_OBJECT(newString(vm, buf, n)));
}

// ------------------------------------------------------------ type checks

static MyMoObjectType valueObjType(Value v)
{
    if (valueIsNil(v))
        return OBJ_NIL;
    if (valueIsBool(v))
        return OBJ_BOOL;
    if (valueLooksLikeInt(v))
        return OBJ_INT;
    if (valueLooksLikeDouble(v))
        return OBJ_DOUBLE;
    return V_AS_OBJ(v)->type;
}

static bool classInherits(MyMoClass *klass, MyMoObject *target)
{
    if (AS_OBJECT(klass) == target)
        return true;
    for (int i = 0; i < klass->superClasses.count; i++)
        if (classInherits(AS_CLASS(klass->superClasses.objects[i]), target))
            return true;
    return false;
}

bool isInstanceOf(MVM *vm, Value x, Value cls)
{
    if (!V_IS_OBJ(cls))
        return false;
    MyMoObject *c = V_AS_OBJ(cls);
    if (c->type == OBJ_TUPLE)
    {
        ValueArray *options = &AS_TUPLE(c)->values;
        for (int i = 0; i < options->count; i++)
            if (isInstanceOf(vm, x, options->values[i]))
                return true;
        return false;
    }
    if (c->type == OBJ_CLASS)
        return V_IS_OBJ_TYPE(x, OBJ_INSTANCE) && classInherits(AS_INSTANCE(V_AS_OBJ(x))->klass, c);
    if (c->type == OBJ_BUILTIN_CLASS)
    {
        MyMoObjectType t = valueObjType(x);
        if (AS_OBJECT(vm->builtInClasses[t]) == c)
            return true;
        // Every value is an object; a bool also counts as an int.
        return (t == OBJ_BOOL && AS_OBJECT(vm->builtInClasses[OBJ_INT]) == c) ||
               AS_OBJECT(vm->builtInClasses[OBJ_OBJECT]) == c;
    }
    return false;
}

// isinstance(x, cls) — cls is a class, a builtin type (int, str, list,
// ...) or a tuple of them.
static Value isinstancefn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "isinstance", argc, 2, 2))
        return V_EMPTY_VAL;
    bool result = isInstanceOf(vm, argv[0], argv[1]);
    popArgs(vm, argc);
    return V_BOOL_VAL(result);
}

// typename(x): the class name of an instance ("Point"), or the builtin
// type's name ("int", "str", "list", ...).
static Value typenamefn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "typename", argc, 1, 1))
        return V_EMPTY_VAL;
    Value x = argv[0];
    MyMoString *name = NULL;
    if (V_IS_OBJ_TYPE(x, OBJ_INSTANCE))
        name = AS_INSTANCE(V_AS_OBJ(x))->klass->name;
    else
    {
        MyMoBuiltInClass *klass = vm->builtInClasses[valueObjType(x)];
        if (klass)
            name = klass->name;
    }
    popArgs(vm, argc);
    if (name)
        return V_OBJ_VAL(AS_OBJECT(name));
    const char *type = valueTypeName(x); // "<object 'name'>"
    const char *start = strchr(type, '\'');
    int len = start ? (int)(strrchr(type, '\'') - start - 1) : (int)strlen(type);
    return V_OBJ_VAL(AS_OBJECT(newString(vm, start ? start + 1 : type, len)));
}

static Value callablefn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "callable", argc, 1, 1))
        return V_EMPTY_VAL;
    bool result = false;
    if (V_IS_OBJ(argv[0]))
    {
        switch (V_AS_OBJ(argv[0])->type)
        {
        case OBJ_FUNCTION:
        case OBJ_CLOUSER:
        case OBJ_BOUND_METHOD:
        case OBJ_BUILTIN_FUNCTION:
        case OBJ_BUILTIN_METHOD:
        case OBJ_CLASS:
        case OBJ_BUILTIN_CLASS:
            result = true;
            break;
        default:
            break;
        }
    }
    popArgs(vm, argc);
    return V_BOOL_VAL(result);
}

// format(value[, spec]) — see format.c for the spec language.
static Value formatfn(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "format", argc, 1, 2))
        return V_EMPTY_VAL;
    if (argc == 2 && !V_IS_OBJ_TYPE(argv[1], OBJ_STRING))
    {
        runtimeError(vm, "TypeError: format() spec must be a string, not %s", valueTypeName(argv[1]));
        return V_EMPTY_VAL;
    }
    MyMoString *spec = argc == 2 ? AS_STRING(V_AS_OBJ(argv[1])) : NULL;
    Value out = formatValueToString(vm, argv[0], spec ? spec->value : "", spec ? spec->length : 0);
    if (!V_IS_EMPTY(out))
        popArgs(vm, argc);
    return out;
}

// exit([code]) — flushes output and ends the process.
static Value exitfn(MVM *vm, uint argc, Value argv[])
{
    long code = 0;
    if (!arity(vm, "exit", argc, 0, 1) || (argc == 1 && !needInt(vm, "exit", argv[0], &code)))
        return V_EMPTY_VAL;
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
}

// ------------------------------------------------------------ conversions

// tuple() / tuple(iterable)
static Value newTupleMethod(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "tuple", argc, 0, 1))
        return V_EMPTY_VAL;
    if (argc == 1 && V_IS_OBJ_TYPE(argv[0], OBJ_TUPLE))
    {
        Value t = argv[0];
        popArgs(vm, argc);
        return t;
    }
    MyMoTuple *tuple = newTuple(vm);
    if (argc == 1 && !appendIterable(vm, "tuple", argv[0], &tuple->values))
        return V_EMPTY_VAL;
    popArgs(vm, argc);
    return V_OBJ_VAL(AS_OBJECT(tuple));
}

// dict() / dict(other_dict) / dict([(key, value), ...])
static Value newDictMethod(MVM *vm, uint argc, Value argv[])
{
    if (!arity(vm, "dict", argc, 0, 1))
        return V_EMPTY_VAL;
    MyMoDict *dict = newDict(vm);
    // dict(a=1, b=2): keyword arguments become entries (after any
    // positional source).
    MyMoDict *keywords = takeAllKeywords(vm);
    if (argc == 1)
    {
        Value src = argv[0];
        if (V_IS_OBJ_TYPE(src, OBJ_DICT))
            copyDict(vm, AS_DICT(V_AS_OBJ(src)), dict);
        else
        {
            pushV(vm, V_OBJ_VAL(AS_OBJECT(dict)));
            MyMoList *pairs = iterableToList(vm, "dict", src);
            popV(vm);
            if (!pairs)
                return V_EMPTY_VAL;
            for (int i = 0; i < pairs->values.count; i++)
            {
                Value p = pairs->values.values[i];
                ValueArray *kv = V_IS_OBJ_TYPE(p, OBJ_TUPLE) ? &AS_TUPLE(V_AS_OBJ(p))->values
                               : V_IS_OBJ_TYPE(p, OBJ_LIST)  ? &AS_LIST(V_AS_OBJ(p))->values
                                                             : NULL;
                if (!kv || kv->count != 2)
                {
                    runtimeError(vm, "ValueError: dict() element %d is not a (key, value) pair", i);
                    return V_EMPTY_VAL;
                }
                MyMoObject *key = dictKey(vm, kv->values[0]);
                if (!key)
                    return V_EMPTY_VAL;
                setEntryV(vm, dict, key, kv->values[1]);
            }
        }
    }
    if (keywords)
        copyDict(vm, keywords, dict);
    popArgs(vm, argc);
    return V_OBJ_VAL(AS_OBJECT(dict));
}

void defineBuiltInHelpers(MVM *vm)
{
    defineBuiltInFunction(vm, "min", minfn);
    defineBuiltInFunction(vm, "max", maxfn);
    defineBuiltInFunction(vm, "sum", sumfn);
    defineBuiltInFunction(vm, "any", anyfn);
    defineBuiltInFunction(vm, "all", allfn);
    defineBuiltInFunction(vm, "sorted", sortedfn);
    defineBuiltInFunction(vm, "reversed", reversedfn);
    defineBuiltInFunction(vm, "enumerate", enumeratefn);
    defineBuiltInFunction(vm, "zip", zipfn);
    defineBuiltInFunction(vm, "map", mapfn);
    defineBuiltInFunction(vm, "filter", filterfn);
    defineBuiltInFunction(vm, "abs", absfn);
    defineBuiltInFunction(vm, "round", roundfn);
    defineBuiltInFunction(vm, "divmod", divmodfn);
    defineBuiltInFunction(vm, "pow", powfn);
    defineBuiltInFunction(vm, "hex", hexfn);
    defineBuiltInFunction(vm, "oct", octfn);
    defineBuiltInFunction(vm, "bin", binfn);
    defineBuiltInFunction(vm, "ord", ordfn);
    defineBuiltInFunction(vm, "chr", chrfn);
    defineBuiltInFunction(vm, "isinstance", isinstancefn);
    defineBuiltInFunction(vm, "callable", callablefn);
    defineBuiltInFunction(vm, "typename", typenamefn);
    defineBuiltInFunction(vm, "exit", exitfn);
    defineBuiltInFunction(vm, "format", formatfn);

    // tuple and dict constructors; `float` is another name for double.
    MyMoObject *tupleName = AS_OBJECT(newString(vm, "tuple", 5));
    setEntry(vm, &vm->builtins, tupleName, AS_OBJECT(vm->builtInClasses[OBJ_TUPLE]));
    defineMethod(vm, OBJ_TUPLE, "__new__", newTupleMethod);
    defineMethod(vm, OBJ_DICT, "__new__", newDictMethod);
    MyMoObject *floatName = AS_OBJECT(newString(vm, "float", 5));
    setEntry(vm, &vm->builtins, floatName, AS_OBJECT(vm->builtInClasses[OBJ_DOUBLE]));
}
