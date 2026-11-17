#include "repr.h"
#include "vm.h"
#include "memory.h"
#include "include/mymo.h"
#include "datatypes/datatypes.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Containers nested deeper than this (or containing themselves) print
// as "..." instead of recursing forever.
#define MAX_FORMAT_DEPTH 64

void strbufInit(StrBuf *b)
{
    b->data = NULL;
    b->length = 0;
    b->capacity = 0;
}

void strbufFree(StrBuf *b)
{
    free(b->data);
    strbufInit(b);
}

void strbufAppend(StrBuf *b, const char *s, int length)
{
    if (b->length + length + 1 > b->capacity)
    {
        int capacity = b->capacity < 64 ? 64 : b->capacity;
        while (capacity < b->length + length + 1)
            capacity *= 2;
        b->data = realloc(b->data, (size_t)capacity);
        if (b->data == NULL)
        {
            fprintf(stderr, "MemoryError: out of memory while formatting a value\n");
            exit(1);
        }
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, s, (size_t)length);
    b->length += length;
    b->data[b->length] = '\0';
}

void strbufAppendC(StrBuf *b, const char *s)
{
    strbufAppend(b, s, (int)strlen(s));
}

static void appendf(StrBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void appendf(StrBuf *b, const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    strbufAppend(b, buf, n);
}

// Round-trippable spelling with a ".0" on whole numbers so a double
// never prints like an int: 2.0, 0.1, 1e+20, inf, nan.
static void appendDouble(StrBuf *b, double d)
{
    // Fewest of 15/16/17 significant digits that read back as d
    // (0.1 -> 0.1, 0.1 + 0.2 -> 0.30000000000000004).
    char buf[64];
    int n = 0;
    for (int precision = 15; precision <= 17; precision++)
    {
        n = snprintf(buf, sizeof(buf), "%.*g", precision, d);
        if (strtod(buf, NULL) == d || d != d)
            break;
    }
    strbufAppend(b, buf, n);
    if (strpbrk(buf, ".eEna") == NULL) // no fraction, exponent, inf or nan
        strbufAppend(b, ".0", 2);
}

static void appendQuoted(StrBuf *b, MyMoString *s)
{
    strbufAppend(b, "\"", 1);
    for (int i = 0; i < s->length; i++)
    {
        unsigned char c = (unsigned char)s->value[i];
        switch (c)
        {
        case '"':  strbufAppend(b, "\\\"", 2); break;
        case '\\': strbufAppend(b, "\\\\", 2); break;
        case '\n': strbufAppend(b, "\\n", 2); break;
        case '\t': strbufAppend(b, "\\t", 2); break;
        case '\r': strbufAppend(b, "\\r", 2); break;
        default:
            if (c < 0x20 || c == 0x7f)
                appendf(b, "\\x%02x", c);
            else
                strbufAppend(b, (const char *)&c, 1);
        }
    }
    strbufAppend(b, "\"", 1);
}

static bool formatDepth(MVM *vm, StrBuf *b, Value v, bool repr, int depth);

static bool formatElements(MVM *vm, StrBuf *b, ValueArray *values, int depth)
{
    for (int i = 0; i < values->count; i++)
    {
        if (i)
            strbufAppend(b, ", ", 2);
        if (!formatDepth(vm, b, values->values[i], true, depth + 1))
            return false;
    }
    return true;
}

// Calls instance.<name>() when its class defines it; *found says whether
// it did. The result must be a string.
static bool callStrMethod(MVM *vm, StrBuf *b, MyMoInstance *instance, const char *name, bool *found)
{
    *found = false;
    Value method;
    MyMoObject *key = AS_OBJECT(newString(vm, name, (int)strlen(name)));
    if (!getEntryV(instance->klass->methods, key, &method) || !V_IS_OBJ_TYPE(method, OBJ_FUNCTION))
        return true;
    *found = true;
    MyMoBoundMethod *bound = newBoundMethod(vm, AS_OBJECT(instance), AS_FUNCTION(V_AS_OBJ(method)));
    Value out;
    if (mymo_call(vm, V_OBJ_VAL(AS_OBJECT(bound)), 0, NULL, &out) != MYMO_OK)
        return false;
    if (!V_IS_OBJ_TYPE(out, OBJ_STRING))
    {
        runtimeError(vm, "TypeError: %s() returned %s, not a string", name, valueTypeName(out));
        return false;
    }
    MyMoString *s = AS_STRING(V_AS_OBJ(out));
    strbufAppend(b, s->value, s->length);
    return true;
}

static void formatFunction(StrBuf *b, MyMoFunction *function)
{
    const char *name = function->name ? function->name->value : "?";
    switch (function->type)
    {
    case FN_ARROWFN:
        strbufAppendC(b, "<anonymous function>");
        break;
    case FN_GENERATOR:
    case FN_GEN_METHOD:
        appendf(b, "<generator %s>", name);
        break;
    case FN_METHOD:
    case FN_INIT:
    case FN_OPERATOR:
        appendf(b, "<method %s>", name);
        break;
    case FN_SCRIPT:
    case FN_COMPILED:
    case FN_MODULE:
        appendf(b, "<script %s>", name);
        break;
    default:
        appendf(b, "<function %s>", name);
        break;
    }
}

static bool formatObject(MVM *vm, StrBuf *b, MyMoObject *o, bool repr, int depth)
{
    switch (o->type)
    {
    case OBJ_STRING:
        if (repr)
            appendQuoted(b, AS_STRING(o));
        else
            strbufAppend(b, AS_STRING(o)->value, AS_STRING(o)->length);
        return true;
    case OBJ_INT:
        appendf(b, "%ld", AS_INT(o)->value);
        return true;
    case OBJ_DOUBLE:
        appendDouble(b, AS_DOUBLE(o)->value);
        return true;
    case OBJ_NIL:
        strbufAppend(b, "Nil", 3);
        return true;
    case OBJ_BOOL:
        strbufAppendC(b, AS_BOOL(o)->value ? "True" : "False");
        return true;
    case OBJ_LIST:
        strbufAppend(b, "[", 1);
        if (!formatElements(vm, b, &AS_LIST(o)->values, depth))
            return false;
        strbufAppend(b, "]", 1);
        return true;
    case OBJ_TUPLE:
    {
        ValueArray *values = &AS_TUPLE(o)->values;
        strbufAppend(b, "(", 1);
        if (!formatElements(vm, b, values, depth))
            return false;
        strbufAppendC(b, values->count == 1 ? ",)" : ")");
        return true;
    }
    case OBJ_DICT:
    {
        MyMoDict *dict = AS_DICT(o);
        strbufAppend(b, "{", 1);
        bool first = true;
        Entry *entry;
        DICT_FOREACH(dict, entry)
        {
            if (!first)
                strbufAppend(b, ", ", 2);
            first = false;
            if (!formatDepth(vm, b, objectToValue(entry->key), true, depth + 1))
                return false;
            strbufAppend(b, ": ", 2);
            if (!formatDepth(vm, b, entry->value, true, depth + 1))
                return false;
        }
        strbufAppend(b, "}", 1);
        return true;
    }
    case OBJ_INSTANCE:
    {
        MyMoInstance *instance = AS_INSTANCE(o);
        bool found;
        if (repr)
        {
            if (!callStrMethod(vm, b, instance, "__repr__", &found))
                return false;
            if (found)
                return true;
        }
        if (!callStrMethod(vm, b, instance, "__str__", &found))
            return false;
        if (!found)
            appendf(b, "<%s instance>", instance->klass->name->value);
        return true;
    }
    case OBJ_CLASS:
        appendf(b, "<class %s>", AS_CLASS(o)->name->value);
        return true;
    case OBJ_BUILTIN_CLASS:
        appendf(b, "<class %s>", AS_BUILTIN_CLASS(o)->name->value);
        return true;
    case OBJ_FUNCTION:
        formatFunction(b, AS_FUNCTION(o));
        return true;
    case OBJ_BOUND_METHOD:
    {
        MyMoBoundMethod *bound = AS_BOUND_METHOD(o);
        appendf(b, "<bound method %s.%s>", AS_INSTANCE(bound->self)->klass->name->value,
                bound->method->name->value);
        return true;
    }
    case OBJ_BUILTIN_FUNCTION:
        appendf(b, "<built-in function %s>", AS_BUILTIN_FUNCTION(o)->name->value);
        return true;
    case OBJ_BUILTIN_METHOD:
        appendf(b, "<built-in method %s>", AS_BUILTIN_FUNCTION(o)->name->value);
        return true;
    case OBJ_MODULE:
        appendf(b, "<module %s>", AS_MODULE(o)->name->value);
        return true;
    case OBJ_WILDCARD:
        strbufAppend(b, "_", 1);
        return true;
    case OBJ_SET:
    {
        MyMoDict *items = &AS_SET(o)->items;
        if (items->count == 0)
        {
            strbufAppendC(b, "set()");
            return true;
        }
        strbufAppend(b, "{", 1);
        bool first = true;
        Entry *entry;
        DICT_FOREACH(items, entry)
        {
            if (!first)
                strbufAppend(b, ", ", 2);
            first = false;
            if (!formatDepth(vm, b, objectToValue(entry->key), true, depth + 1))
                return false;
        }
        strbufAppend(b, "}", 1);
        return true;
    }
    case OBJ_RANGE:
    {
        MyMoRange *r = AS_RANGE(o);
        if (r->step == 1)
            appendf(b, "range(%ld, %ld)", r->start, r->stop);
        else
            appendf(b, "range(%ld, %ld, %ld)", r->start, r->stop, r->step);
        return true;
    }
    default:
        strbufAppendC(b, getType(o));
        return true;
    }
}

static bool formatDepth(MVM *vm, StrBuf *b, Value v, bool repr, int depth)
{
    if (V_IS_INT(v))
    {
        appendf(b, "%d", V_AS_INT(v));
        return true;
    }
    if (V_IS_DOUBLE(v))
    {
        appendDouble(b, V_AS_DOUBLE(v));
        return true;
    }
    if (V_IS_NIL(v))
    {
        strbufAppend(b, "Nil", 3);
        return true;
    }
    if (V_IS_TRUE(v))
    {
        strbufAppend(b, "True", 4);
        return true;
    }
    if (V_IS_FALSE(v))
    {
        strbufAppend(b, "False", 5);
        return true;
    }
    if (!V_IS_OBJ(v))
    {
        strbufAppendC(b, "<?>");
        return true;
    }
    if (depth > MAX_FORMAT_DEPTH)
    {
        strbufAppend(b, "...", 3);
        return true;
    }
    return formatObject(vm, b, V_AS_OBJ(v), repr, depth);
}

bool formatValue(MVM *vm, StrBuf *b, Value v, bool repr)
{
    return formatDepth(vm, b, v, repr, 0);
}

static Value toString(MVM *vm, Value v, bool repr)
{
    if (!repr && V_IS_OBJ_TYPE(v, OBJ_STRING))
        return v;
    StrBuf b;
    strbufInit(&b);
    if (!formatValue(vm, &b, v, repr))
    {
        strbufFree(&b);
        return V_EMPTY_VAL;
    }
    Value out = V_OBJ_VAL(AS_OBJECT(newString(vm, b.data ? b.data : "", b.length)));
    strbufFree(&b);
    return out;
}

Value valueToStr(MVM *vm, Value v) { return toString(vm, v, false); }
Value valueToRepr(MVM *vm, Value v) { return toString(vm, v, true); }
