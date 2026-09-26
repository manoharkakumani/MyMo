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

Value newStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc > 1)
    {
        runtimeError(vm, "str() takes  1 argument (%d given)", argc);
        return V_EMPTY_VAL;
    }
    else if (argc == 0)
    {
        return objectToValue(NEW_STRING(vm, "", 0));
    }
    else
    {
        if (IS_STRING(args[0]))
        {
            return objectToValue(pop(vm));
        }
        else if (IS_INT(args[0]))
        {
            char x[1000];
            sprintf(x, "%ld", INT_VAL(pop(vm)));
            return objectToValue(NEW_STRING(vm, x, strlen(x)));
        }
        else if (IS_DOUBLE(args[0]))
        {
            char x[1000];
            sprintf(x, "%g", DOUBLE_VAL(pop(vm)));
            return objectToValue(NEW_STRING(vm, x, strlen(x)));
        }
        else
        {
            char *a = getType(pop(vm));
            return objectToValue(NEW_STRING(vm, a, strlen(a)));
        }
    }
}

Value stringLengthMethod(MVM *vm, uint argc, MyMoObject *args[])
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
static bool needString(MVM *vm, const char *fn, MyMoObject *arg)
{
    if (IS_STRING(arg))
        return true;
    runtimeError(vm, "TypeError: %s() expects a string, got %s", fn, getType(arg));
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

Value upperStringMethod(MVM *vm, uint argc, MyMoObject *args[])
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

Value lowerStringMethod(MVM *vm, uint argc, MyMoObject *args[])
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

Value stripStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "strip", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    int start = 0, end = s->length;
    while (start < end && isspace((unsigned char)s->value[start])) start++;
    while (end > start && isspace((unsigned char)s->value[end - 1])) end--;
    return objectToValue(NEW_STRING(vm, s->value + start, end - start));
}

Value findStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "find", argc, 1, 1);
    if (!self || !needString(vm, "find", args[0])) return V_EMPTY_VAL;
    return V_INT_VAL(findSub(AS_STRING(self), AS_STRING(args[0]), 0));
}

Value containsStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "contains", argc, 1, 1);
    if (!self || !needString(vm, "contains", args[0])) return V_EMPTY_VAL;
    return V_BOOL_VAL(findSub(AS_STRING(self), AS_STRING(args[0]), 0) >= 0);
}

Value startswithStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "startswith", argc, 1, 1);
    if (!self || !needString(vm, "startswith", args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *p = AS_STRING(args[0]);
    return V_BOOL_VAL(p->length <= s->length && memcmp(s->value, p->value, (size_t)p->length) == 0);
}

Value endswithStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "endswith", argc, 1, 1);
    if (!self || !needString(vm, "endswith", args[0])) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *p = AS_STRING(args[0]);
    return V_BOOL_VAL(p->length <= s->length
                      && memcmp(s->value + s->length - p->length, p->value, (size_t)p->length) == 0);
}

Value replaceStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "replace", argc, 2, 2);
    if (!self || !needString(vm, "replace", args[0]) || !needString(vm, "replace", args[1]))
        return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self), *from = AS_STRING(args[0]), *to = AS_STRING(args[1]);
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
Value splitStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "split", argc, 0, 1);
    if (!self || (argc && !needString(vm, "split", args[0]))) return V_EMPTY_VAL;
    MyMoString *s = AS_STRING(self);
    MyMoList *out = newList(vm);
    if (argc == 0)
    {
        int i = 0;
        while (i < s->length)
        {
            while (i < s->length && isspace((unsigned char)s->value[i])) i++;
            int start = i;
            while (i < s->length && !isspace((unsigned char)s->value[i])) i++;
            if (i > start)
                writeMyMoObjectArray(vm, &out->values, NEW_STRING(vm, s->value + start, i - start));
        }
        return objectToValue(AS_OBJECT(out));
    }
    MyMoString *sep = AS_STRING(args[0]);
    if (sep->length == 0)
    {
        runtimeError(vm, "ValueError: split() separator must not be empty");
        return V_EMPTY_VAL;
    }
    int pos = 0;
    for (int i = findSub(s, sep, 0); i >= 0; i = findSub(s, sep, pos))
    {
        writeMyMoObjectArray(vm, &out->values, NEW_STRING(vm, s->value + pos, i - pos));
        pos = i + sep->length;
    }
    writeMyMoObjectArray(vm, &out->values, NEW_STRING(vm, s->value + pos, s->length - pos));
    return objectToValue(AS_OBJECT(out));
}

// sep.join(list_of_strings)
Value joinStringMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoObject *self = methodEnter(vm, "join", argc, 1, 1);
    if (!self) return V_EMPTY_VAL;
    if (!IS_LIST(args[0]) && !IS_TUPLE(args[0]))
    {
        runtimeError(vm, "TypeError: join() expects a list or tuple, got %s", getType(args[0]));
        return V_EMPTY_VAL;
    }
    MyMoObjectArray *items = IS_LIST(args[0]) ? &AS_LIST(args[0])->values : &AS_TUPLE(args[0])->values;
    MyMoString *sep = AS_STRING(self);
    int len = 0;
    for (int i = 0; i < items->count; i++)
    {
        if (!IS_STRING(items->objects[i]))
        {
            runtimeError(vm, "TypeError: join() item %d is %s, not a string", i, getType(items->objects[i]));
            return V_EMPTY_VAL;
        }
        len += AS_STRING(items->objects[i])->length + (i ? sep->length : 0);
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
        MyMoString *item = AS_STRING(items->objects[i]);
        memcpy(buf + n, item->value, (size_t)item->length);
        n += item->length;
    }
    MyMoObject *out = NEW_STRING(vm, buf, len);
    free(buf);
    return objectToValue(out);
}

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
}

void defineStringClass(MVM *vm)
{
    MyMoString *name = newString(vm, "str", 3);
    MyMoBuiltInClass *stringClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_STRING] = stringClass;
    defineStringMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(stringClass));
}