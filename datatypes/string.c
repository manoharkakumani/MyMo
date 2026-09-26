#include "../memory.h"
#include "../vm.h"
#include "string.h"
#include "dict.h"
#include "nil.h"
#include "function.h"
#include "list.h"
#include "tuple.h"
#include "../operations.h"
#include <ctype.h>
#include "../repr.h"
#include "../builtins.h"

MyMoString *newString(MVM *vm, const char *chars, int length)
{
    char *heapChars = Allocate(vm, char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    u32 hash = hasher(heapChars, length);
    MyMoString *interned = findString(&vm->strings, heapChars, length, hash);
    if (interned != NULL)
    {
        Free(vm, char, heapChars);
        return interned;
    }
    MyMoString *string = AllocateObject(vm, MyMoString, OBJ_STRING);
    string->object.hash = hash;
    string->length = length;
    string->value = heapChars;
    setPrimitive(vm, &vm->strings, (MyMoObject *)string);
    return string;
}
void printString(MyMoString *string)
{
    printf("%s", string->value);
}

Value newStringMethod(MVM *vm, uint argc, Value args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "str() takes  1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    if (argc == 0)
        return objectToValue(NEW_STRING(vm, "", 0));
    Value out = valueToStr(vm, args[0]);
    if (!V_IS_EMPTY(out))
        popV(vm);
    return out;
}

Value stringLengthMethod(MVM *vm, uint argc, Value args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "TypeError: __len__() takes 0 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, 0));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: __len__() can only be applied on instance");
        return V_EMPTY_VAL;
    }
        
    return objectToValue(NEW_INT(vm, AS_STRING(function->self)->length));
}
static bool needString(MVM *vm, const char *fn, Value arg)
{
    if (V_IS_OBJ_TYPE(arg, OBJ_STRING))
        return true;
    runtimeError(vm, "TypeError: %s() expects a string, got %s", fn, valueTypeName(arg));
    return false;
}

// Index of `needle` in `hay` at or after `from`, or -1.
static int findSub(MyMoString *hay, MyMoString *needle, int from)
{
    if (needle->length == 0)
        return from <= hay->length ? from : -1;
    for (int i = from; i + needle->length <= hay->length; i++)
        if (memcmp(hay->value + i, needle->value, (size_t)needle->length) == 0)
            return i;
    return -1;
}

// A start position argument, negative counting from the end, clamped.
static bool startIndex(MVM *vm, const char *fn, Value v, int length, int *out)
{
    if (!valueLooksLikeInt(v))
    {
        runtimeError(vm, "TypeError: %s() start must be an integer, not %s", fn, valueTypeName(v));
        return false;
    }
    long i = valueToLong(v);
    if (i < 0) i += length;
    if (i < 0) i = 0;
    if (i > length) i = length;
    *out = (int)i;
    return true;
}

// Index of the last `needle` in `hay`, or -1.
static int rfindSub(MyMoString *hay, MyMoString *needle)
{
    for (int i = hay->length - needle->length; i >= 0; i--)
        if (memcmp(hay->value + i, needle->value, (size_t)needle->length) == 0)
            return i;
    return -1;
}

static Value makeString(MVM *vm, const char *chars, int length)
{
    return V_OBJ_VAL(AS_OBJECT(newString(vm, chars, length)));
}

// Copy of s with each byte passed through `map` (ASCII case mapping).
static Value mapChars(MVM *vm, MyMoString *s, int (*map)(int))
{
    char *buf = New(char, s->length + 1);
    for (int i = 0; i < s->length; i++)
        buf[i] = (char)map((unsigned char)s->value[i]);
    Value out = makeString(vm, buf, s->length);
    free(buf);
    return out;
}

Value upperStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "upper", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    char *buf = New(char, s->length + 1);
    for (int i = 0; i < s->length; i++)
        buf[i] = (char)toupper((unsigned char)s->value[i]);
    MyMoObject *out = NEW_STRING(vm, buf, s->length);
    free(buf);
    return objectToValue(out);
}

Value lowerStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "lower", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    char *buf = New(char, s->length + 1);
    for (int i = 0; i < s->length; i++)
        buf[i] = (char)tolower((unsigned char)s->value[i]);
    MyMoObject *out = NEW_STRING(vm, buf, s->length);
    free(buf);
    return objectToValue(out);
}

// Strip whitespace, or any of the characters in `chars`, from the
// left and/or right end.
static Value stripEnds(MVM *vm, const char *fn, uint argc, Value args[], bool left, bool right)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 0, 1);
    if (!self) return V_EMPTY_VAL;
    MyMoString *chars = NULL;
    if (argc == 1 && !valueIsNil(args[0]))
    {
        if (!needString(vm, fn, args[0])) return V_EMPTY_VAL;
        chars = AS_STRING(V_AS_OBJ(args[0]));
    }
    MyMoString *s = AS_STRING(self);
#define STRIPPABLE(c) (chars ? memchr(chars->value, (c), (size_t)chars->length) != NULL : isspace((unsigned char)(c)))
    int start = 0, end = s->length;
    if (left)
        while (start < end && STRIPPABLE(s->value[start])) start++;
    if (right)
        while (end > start && STRIPPABLE(s->value[end - 1])) end--;
#undef STRIPPABLE
    return objectToValue(NEW_STRING(vm, s->value + start, end - start));
}

Value stripStringMethod(MVM *vm, uint argc, Value args[]) { return stripEnds(vm, "strip", argc, args, true, true); }
Value lstripStringMethod(MVM *vm, uint argc, Value args[]) { return stripEnds(vm, "lstrip", argc, args, true, false); }
Value rstripStringMethod(MVM *vm, uint argc, Value args[]) { return stripEnds(vm, "rstrip", argc, args, false, true); }

Value findStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "find", argc, 1, 2);
    if (!self || !needString(vm, "find", args[0])) return V_EMPTY_VAL;
    int from = 0;
    if (argc == 2 && !startIndex(vm, "find", args[1], AS_STRING(self)->length, &from)) return V_EMPTY_VAL;
    return V_INT_VAL(findSub(AS_STRING(self), AS_STRING(V_AS_OBJ(args[0])), from));
}

Value containsStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "contains", argc, 1, 1);
    if (!self || !needString(vm, "contains", args[0])) return V_EMPTY_VAL;
    return V_BOOL_VAL(findSub(AS_STRING(self), AS_STRING(V_AS_OBJ(args[0])), 0) >= 0);
}

Value startswithStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "startswith", argc, 1, 1);
    if (!self || !needString(vm, "startswith", args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *p = AS_STRING(V_AS_OBJ(args[0]));
    return V_BOOL_VAL(p->length <= s->length && memcmp(s->value, p->value, (size_t)p->length) == 0);
}

Value endswithStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "endswith", argc, 1, 1);
    if (!self || !needString(vm, "endswith", args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *p = AS_STRING(V_AS_OBJ(args[0]));
    return V_BOOL_VAL(p->length <= s->length
                      && memcmp(s->value + s->length - p->length, p->value, (size_t)p->length) == 0);
}

Value replaceStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "replace", argc, 2, 2);
    if (!self || !needString(vm, "replace", args[0]) || !needString(vm, "replace", args[1]))
        return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *from = AS_STRING(V_AS_OBJ(args[0])), *to = AS_STRING(V_AS_OBJ(args[1]));
    if (from->length == 0)
        return objectToValue(self);
    // Count first so the output buffer is sized exactly.
    int hits = 0;
    for (int i = findSub(s, from, 0); i >= 0; i = findSub(s, from, i + from->length))
        hits++;
    int len = s->length + hits * (to->length - from->length);
    char *buf = New(char, len + 1);
    int n = 0, pos = 0;
    for (int i = findSub(s, from, 0); i >= 0; i = findSub(s, from, pos))
    {
        memcpy(buf + n, s->value + pos, (size_t)(i - pos));
        n += i - pos;
        memcpy(buf + n, to->value, (size_t)to->length);
        n += to->length;
        pos = i + from->length;
    }
    memcpy(buf + n, s->value + pos, (size_t)(s->length - pos));
    MyMoObject *out = NEW_STRING(vm, buf, len);
    free(buf);
    return objectToValue(out);
}

// split(): on runs of whitespace (dropping empties); split(sep): on
// every occurrence of sep (keeping empties), like Python.
Value splitStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "split", argc, 0, 2);
    if (!self || (argc && !valueIsNil(args[0]) && !needString(vm, "split", args[0]))) return V_EMPTY_VAL;
    long maxsplit = -1;
    if (argc == 2)
    {
        if (!valueLooksLikeInt(args[1]))
        {
            runtimeError(vm, "TypeError: split() maxsplit must be an integer, not %s", valueTypeName(args[1]));
            return V_EMPTY_VAL;
        }
        maxsplit = valueToLong(args[1]);
    }
    MyMoString *s = AS_STRING(self);
    MyMoList *out = newList(vm);
    if (argc == 0 || valueIsNil(args[0]))
    {
        int i = 0;
        while (i < s->length)
        {
            while (i < s->length && isspace((unsigned char)s->value[i])) i++;
            if (i == s->length) break;
            int start = i;
            if (maxsplit >= 0 && out->values.count == maxsplit)
            {
                // The rest, minus trailing whitespace, is the last piece.
                int end = s->length;
                while (end > start && isspace((unsigned char)s->value[end - 1])) end--;
                writeValueArrayObject(vm, &out->values, NEW_STRING(vm, s->value + start, end - start));
                break;
            }
            while (i < s->length && !isspace((unsigned char)s->value[i])) i++;
            writeValueArrayObject(vm, &out->values, NEW_STRING(vm, s->value + start, i - start));
        }
        return objectToValue(AS_OBJECT(out));
    }
    MyMoString *sep = AS_STRING(V_AS_OBJ(args[0]));
    if (sep->length == 0)
    {
        runtimeError(vm, "ValueError: split() separator must not be empty");
        return V_EMPTY_VAL;
    }
    int pos = 0;
    for (int i = findSub(s, sep, 0); i >= 0 && (maxsplit < 0 || out->values.count < maxsplit); i = findSub(s, sep, pos))
    {
        writeValueArrayObject(vm, &out->values, NEW_STRING(vm, s->value + pos, i - pos));
        pos = i + sep->length;
    }
    writeValueArrayObject(vm, &out->values, NEW_STRING(vm, s->value + pos, s->length - pos));
    return objectToValue(AS_OBJECT(out));
}

// sep.join(list_of_strings)
Value joinStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "join", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    MyMoList *seq = newList(vm);
    if (!appendIterable(vm, "join", args[0], &seq->values))
        return V_EMPTY_VAL;
    ValueArray *items = &seq->values;
    MyMoString *sep = AS_STRING(self);
    int len = 0;
    for (int i = 0; i < items->count; i++)
    {
        Value item = items->values[i];
        if (!V_IS_OBJ(item) || !IS_STRING(V_AS_OBJ(item)))
        {
            runtimeError(vm, "TypeError: join() item %d is %s, not a string", i, getType(valueToBoxedObject(vm, item)));
            return V_EMPTY_VAL;
        }
        len += AS_STRING(V_AS_OBJ(item))->length + (i ? sep->length : 0);
    }
    char *buf = New(char, len + 1);
    int n = 0;
    for (int i = 0; i < items->count; i++)
    {
        if (i)
        {
            memcpy(buf + n, sep->value, (size_t)sep->length);
            n += sep->length;
        }
        MyMoString *item = AS_STRING(V_AS_OBJ(items->values[i]));
        memcpy(buf + n, item->value, (size_t)item->length);
        n += item->length;
    }
    MyMoObject *out = NEW_STRING(vm, buf, len);
    free(buf);
    return objectToValue(out);
}

Value rfindStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "rfind", argc, 1, 1);
    if (!self || !needString(vm, "rfind", args[0])) return V_EMPTY_VAL;
    return V_INT_VAL(rfindSub(AS_STRING(self), AS_STRING(V_AS_OBJ(args[0]))));
}

// Like find/rfind, but a missing substring raises ValueError.
Value indexStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "index", argc, 1, 2);
    if (!self || !needString(vm, "index", args[0])) return V_EMPTY_VAL;
    int from = 0;
    if (argc == 2 && !startIndex(vm, "index", args[1], AS_STRING(self)->length, &from)) return V_EMPTY_VAL;
    int i = findSub(AS_STRING(self), AS_STRING(V_AS_OBJ(args[0])), from);
    if (i < 0)
    {
        runtimeError(vm, "ValueError: substring not found");
        return V_EMPTY_VAL;
    }
    return V_INT_VAL(i);
}

Value rindexStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "rindex", argc, 1, 1);
    if (!self || !needString(vm, "rindex", args[0])) return V_EMPTY_VAL;
    int i = rfindSub(AS_STRING(self), AS_STRING(V_AS_OBJ(args[0])));
    if (i < 0)
    {
        runtimeError(vm, "ValueError: substring not found");
        return V_EMPTY_VAL;
    }
    return V_INT_VAL(i);
}

// Non-overlapping occurrences of sub ("" counts length + 1 positions).
Value countStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "count", argc, 1, 1);
    if (!self || !needString(vm, "count", args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *sub = AS_STRING(V_AS_OBJ(args[0]));
    if (sub->length == 0)
        return V_INT_VAL(s->length + 1);
    int n = 0;
    for (int i = findSub(s, sub, 0); i >= 0; i = findSub(s, sub, i + sub->length))
        n++;
    return V_INT_VAL(n);
}

// "a\nb\r\nc" -> ["a", "b", "c"] (a trailing newline adds no empty line)
Value splitlinesStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "splitlines", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    MyMoList *out = newList(vm);
    int start = 0;
    for (int i = 0; i < s->length; i++)
    {
        if (s->value[i] != '\n' && s->value[i] != '\r')
            continue;
        writeValueArray(vm, &out->values, makeString(vm, s->value + start, i - start));
        if (s->value[i] == '\r' && i + 1 < s->length && s->value[i + 1] == '\n')
            i++;
        start = i + 1;
    }
    if (start < s->length)
        writeValueArray(vm, &out->values, makeString(vm, s->value + start, s->length - start));
    return V_OBJ_VAL(AS_OBJECT(out));
}

// s.partition(sep) -> (before, sep, after); ("s", "", "") when missing.
// rpartition splits at the last occurrence (("", "", s) when missing).
static Value partitionAt(MVM *vm, const char *fn, uint argc, Value args[], bool last)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 1, 1);
    if (!self || !needString(vm, fn, args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *sep = AS_STRING(V_AS_OBJ(args[0]));
    if (sep->length == 0)
    {
        runtimeError(vm, "ValueError: %s() separator must not be empty", fn);
        return V_EMPTY_VAL;
    }
    int i = last ? rfindSub(s, sep) : findSub(s, sep, 0);
    MyMoTuple *out = newTuple(vm);
    Value empty = makeString(vm, "", 0);
    if (i < 0)
    {
        writeValueArray(vm, &out->values, last ? empty : V_OBJ_VAL(self));
        writeValueArray(vm, &out->values, empty);
        writeValueArray(vm, &out->values, last ? V_OBJ_VAL(self) : empty);
    }
    else
    {
        writeValueArray(vm, &out->values, makeString(vm, s->value, i));
        writeValueArray(vm, &out->values, args[0]);
        writeValueArray(vm, &out->values, makeString(vm, s->value + i + sep->length, s->length - i - sep->length));
    }
    return V_OBJ_VAL(AS_OBJECT(out));
}

Value partitionStringMethod(MVM *vm, uint argc, Value args[]) { return partitionAt(vm, "partition", argc, args, false); }
Value rpartitionStringMethod(MVM *vm, uint argc, Value args[]) { return partitionAt(vm, "rpartition", argc, args, true); }

Value capitalizeStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "capitalize", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    char *buf = New(char, s->length + 1);
    for (int i = 0; i < s->length; i++)
        buf[i] = (char)(i == 0 ? toupper((unsigned char)s->value[i]) : tolower((unsigned char)s->value[i]));
    Value out = makeString(vm, buf, s->length);
    free(buf);
    return out;
}

// Upper-case the first letter of every word, lower-case the rest.
Value titleStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "title", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    char *buf = New(char, s->length + 1);
    bool inWord = false;
    for (int i = 0; i < s->length; i++)
    {
        unsigned char c = (unsigned char)s->value[i];
        buf[i] = (char)(inWord ? tolower(c) : toupper(c));
        inWord = isalpha(c) != 0;
    }
    Value out = makeString(vm, buf, s->length);
    free(buf);
    return out;
}

static int swapCase(int c) { return isupper(c) ? tolower(c) : islower(c) ? toupper(c) : c; }

Value swapcaseStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "swapcase", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    return mapChars(vm, AS_STRING(self), swapCase);
}

// is*() predicates: true when the string is non-empty and every byte
// passes (isupper/islower: has a cased letter and none of the other case).
static Value classify(MVM *vm, const char *fn, uint argc, int (*test)(int))
{
    MyMoObject *self = methodEnter(vm, fn, argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    if (s->length == 0)
        return V_FALSE_VAL;
    for (int i = 0; i < s->length; i++)
        if (!test((unsigned char)s->value[i]))
            return V_FALSE_VAL;
    return V_TRUE_VAL;
}

static Value caseCheck(MVM *vm, const char *fn, uint argc, bool upper)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    bool cased = false;
    for (int i = 0; i < s->length; i++)
    {
        unsigned char c = (unsigned char)s->value[i];
        if (upper ? islower(c) : isupper(c))
            return V_FALSE_VAL;
        if (isalpha(c))
            cased = true;
    }
    return V_BOOL_VAL(cased);
}

Value isdigitStringMethod(MVM *vm, uint argc, Value args[]) { return classify(vm, "isdigit", argc, isdigit); }
Value isalphaStringMethod(MVM *vm, uint argc, Value args[]) { return classify(vm, "isalpha", argc, isalpha); }
Value isalnumStringMethod(MVM *vm, uint argc, Value args[]) { return classify(vm, "isalnum", argc, isalnum); }
Value isspaceStringMethod(MVM *vm, uint argc, Value args[]) { return classify(vm, "isspace", argc, isspace); }
Value isupperStringMethod(MVM *vm, uint argc, Value args[]) { return caseCheck(vm, "isupper", argc, true); }
Value islowerStringMethod(MVM *vm, uint argc, Value args[]) { return caseCheck(vm, "islower", argc, false); }

// Pad to `width` with `fill` (default space): align < 0 left-justifies,
// 0 centres, > 0 right-justifies.
static Value pad(MVM *vm, const char *fn, uint argc, Value args[], int align)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 1, 2);
    if (!self) return V_EMPTY_VAL;
    if (!valueLooksLikeInt(args[0]))
    {
        runtimeError(vm, "TypeError: %s() width must be an integer, not %s", fn, valueTypeName(args[0]));
        return V_EMPTY_VAL;
    }
    char fill = ' ';
    if (argc == 2)
    {
        if (!needString(vm, fn, args[1])) return V_EMPTY_VAL;
        if (AS_STRING(V_AS_OBJ(args[1]))->length != 1)
        {
            runtimeError(vm, "TypeError: %s() fill character must be exactly one character long", fn);
            return V_EMPTY_VAL;
        }
        fill = AS_STRING(V_AS_OBJ(args[1]))->value[0];
    }
    MyMoString *s = AS_STRING(self);
    long width = valueToLong(args[0]);
    if (width <= s->length)
        return V_OBJ_VAL(self);
    int total = (int)width - s->length;
    int left = align < 0 ? 0 : align > 0 ? total : total / 2 + (total & width & 1);
    char *buf = New(char, width + 1);
    memset(buf, fill, (size_t)width);
    memcpy(buf + left, s->value, (size_t)s->length);
    Value out = makeString(vm, buf, (int)width);
    free(buf);
    return out;
}

Value ljustStringMethod(MVM *vm, uint argc, Value args[]) { return pad(vm, "ljust", argc, args, -1); }
Value rjustStringMethod(MVM *vm, uint argc, Value args[]) { return pad(vm, "rjust", argc, args, 1); }
Value centerStringMethod(MVM *vm, uint argc, Value args[]) { return pad(vm, "center", argc, args, 0); }

// "42".zfill(5) -> "00042"; a leading sign stays in front.
Value zfillStringMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "zfill", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    if (!valueLooksLikeInt(args[0]))
    {
        runtimeError(vm, "TypeError: zfill() width must be an integer, not %s", valueTypeName(args[0]));
        return V_EMPTY_VAL;
    }
    MyMoString *s = AS_STRING(self);
    long width = valueToLong(args[0]);
    if (width <= s->length)
        return V_OBJ_VAL(self);
    char *buf = New(char, width + 1);
    int sign = s->length > 0 && (s->value[0] == '-' || s->value[0] == '+');
    int zeros = (int)width - s->length;
    if (sign)
        buf[0] = s->value[0];
    memset(buf + sign, '0', (size_t)zeros);
    memcpy(buf + sign + zeros, s->value + sign, (size_t)(s->length - sign));
    Value out = makeString(vm, buf, (int)width);
    free(buf);
    return out;
}

static Value removeAffix(MVM *vm, const char *fn, uint argc, Value args[], bool prefix)
{
    MyMoObject *self = methodEnter(vm, fn, argc, 1, 1);
    if (!self || !needString(vm, fn, args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *a = AS_STRING(V_AS_OBJ(args[0]));
    if (a->length == 0 || a->length > s->length)
        return V_OBJ_VAL(self);
    if (prefix && memcmp(s->value, a->value, (size_t)a->length) == 0)
        return makeString(vm, s->value + a->length, s->length - a->length);
    if (!prefix && memcmp(s->value + s->length - a->length, a->value, (size_t)a->length) == 0)
        return makeString(vm, s->value, s->length - a->length);
    return V_OBJ_VAL(self);
}

Value removeprefixStringMethod(MVM *vm, uint argc, Value args[]) { return removeAffix(vm, "removeprefix", argc, args, true); }
Value removesuffixStringMethod(MVM *vm, uint argc, Value args[]) { return removeAffix(vm, "removesuffix", argc, args, false); }

void defineStringMethods(MVM *vm)
{
    defineMethod(vm, OBJ_STRING, "__new__", newStringMethod);
    defineMethod(vm, OBJ_STRING, "__len__", stringLengthMethod);
    defineMethod(vm, OBJ_STRING, "upper", upperStringMethod);
    defineMethod(vm, OBJ_STRING, "lower", lowerStringMethod);
    defineMethod(vm, OBJ_STRING, "strip", stripStringMethod);
    defineMethod(vm, OBJ_STRING, "find", findStringMethod);
    defineMethod(vm, OBJ_STRING, "contains", containsStringMethod);
    defineMethod(vm, OBJ_STRING, "startswith", startswithStringMethod);
    defineMethod(vm, OBJ_STRING, "endswith", endswithStringMethod);
    defineMethod(vm, OBJ_STRING, "replace", replaceStringMethod);
    defineMethod(vm, OBJ_STRING, "split", splitStringMethod);
    defineMethod(vm, OBJ_STRING, "join", joinStringMethod);
    defineMethod(vm, OBJ_STRING, "lstrip", lstripStringMethod);
    defineMethod(vm, OBJ_STRING, "rstrip", rstripStringMethod);
    defineMethod(vm, OBJ_STRING, "rfind", rfindStringMethod);
    defineMethod(vm, OBJ_STRING, "index", indexStringMethod);
    defineMethod(vm, OBJ_STRING, "rindex", rindexStringMethod);
    defineMethod(vm, OBJ_STRING, "count", countStringMethod);
    defineMethod(vm, OBJ_STRING, "splitlines", splitlinesStringMethod);
    defineMethod(vm, OBJ_STRING, "partition", partitionStringMethod);
    defineMethod(vm, OBJ_STRING, "rpartition", rpartitionStringMethod);
    defineMethod(vm, OBJ_STRING, "capitalize", capitalizeStringMethod);
    defineMethod(vm, OBJ_STRING, "title", titleStringMethod);
    defineMethod(vm, OBJ_STRING, "swapcase", swapcaseStringMethod);
    defineMethod(vm, OBJ_STRING, "isdigit", isdigitStringMethod);
    defineMethod(vm, OBJ_STRING, "isalpha", isalphaStringMethod);
    defineMethod(vm, OBJ_STRING, "isalnum", isalnumStringMethod);
    defineMethod(vm, OBJ_STRING, "isspace", isspaceStringMethod);
    defineMethod(vm, OBJ_STRING, "isupper", isupperStringMethod);
    defineMethod(vm, OBJ_STRING, "islower", islowerStringMethod);
    defineMethod(vm, OBJ_STRING, "ljust", ljustStringMethod);
    defineMethod(vm, OBJ_STRING, "rjust", rjustStringMethod);
    defineMethod(vm, OBJ_STRING, "center", centerStringMethod);
    defineMethod(vm, OBJ_STRING, "zfill", zfillStringMethod);
    defineMethod(vm, OBJ_STRING, "removeprefix", removeprefixStringMethod);
    defineMethod(vm, OBJ_STRING, "removesuffix", removesuffixStringMethod);
}

void defineStringClass(MVM *vm)
{
    MyMoString *name = newString(vm, "str", 3);
    MyMoBuiltInClass *stringClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_STRING] = stringClass;
    defineStringMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(stringClass));
}