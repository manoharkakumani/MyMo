#include "int.h"
#include "../memory.h"
#include "../vm.h"
#include "nil.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>

MyMoInt *newInt(MVM *vm, long number)
{
    char x[1000];
    sprintf(x, "%ld", number);
    size_t length = strlen(x);
    u32 hash = hasher(x, length);
    MyMoInt *interned = findInt(&vm->integers, number, length, hash);
    if (interned != NULL)
    {
        return interned;
    }
    MyMoInt *longNumber = AllocateObject(vm, MyMoInt, OBJ_INT);
    longNumber->value = number;
    longNumber->object.hash = hash;
    longNumber->length = length;
    setPrimitive(vm, &vm->integers, (MyMoObject *)longNumber);
    return longNumber;
}

void printInt(MyMoInt *number)
{
    printf("%ld", number->value);
}

// int(s, base): digits in `base` (2..36; 0 = from the prefix 0x/0o/0b,
// else 10), surrounding whitespace, a sign and _ separators allowed.
static bool parseInt(const char *s, int length, int base, long *out)
{
    int i = 0, end = length;
    while (i < end && isspace((unsigned char)s[i])) i++;
    while (end > i && isspace((unsigned char)s[end - 1])) end--;
    bool negative = false;
    if (i < end && (s[i] == '+' || s[i] == '-'))
        negative = s[i++] == '-';
    if (end - i >= 2 && s[i] == '0')
    {
        char p = (char)tolower((unsigned char)s[i + 1]);
        int prefixBase = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
        if (prefixBase && (base == 0 || base == prefixBase))
        {
            base = prefixBase;
            i += 2;
        }
    }
    if (base == 0)
        base = 10;
    unsigned long value = 0;
    bool digits = false;
    for (; i < end; i++)
    {
        char c = s[i];
        if (c == '_' && digits && i + 1 < end && s[i + 1] != '_')
            continue;
        int d = isdigit((unsigned char)c) ? c - '0' : isalpha((unsigned char)c) ? tolower((unsigned char)c) - 'a' + 10 : 99;
        if (d >= base)
            return false;
        if (value > (ULONG_MAX - (unsigned long)d) / (unsigned long)base)
            return false; // overflow
        value = value * (unsigned long)base + (unsigned long)d;
        digits = true;
    }
    if (!digits || value > (unsigned long)LONG_MAX + negative)
        return false;
    *out = negative ? (long)(0UL - value) : (long)value;
    return true;
}

Value newIntMethod(MVM *vm, uint argc, Value args[])
{
    if (argc > 2)
    {
        runtimeError(vm, "TypeError: int() takes at most 2 arguments (%u given)", argc);
        return V_EMPTY_VAL;
    }
    if (argc == 0)
        return V_INT_VAL(0);
    Value v = args[0];
    long base = 10;
    if (argc == 2)
    {
        if (!V_IS_OBJ_TYPE(v, OBJ_STRING))
        {
            runtimeError(vm, "TypeError: int() can't convert %s with an explicit base", valueTypeName(v));
            return V_EMPTY_VAL;
        }
        if (!valueLooksLikeInt(args[1]) || (base = valueToLong(args[1])) == 1 || base < 0 || base > 36)
        {
            runtimeError(vm, "ValueError: int() base must be 0 or 2..36");
            return V_EMPTY_VAL;
        }
    }
    Value result;
    if (valueLooksLikeInt(v))
        result = v;
    else if (valueLooksLikeDouble(v))
    {
        double d = valueToDouble(v);
        if (!isfinite(d) || fabs(d) >= 9.2e18)
        {
            runtimeError(vm, "OverflowError: cannot convert %g to int", d);
            return V_EMPTY_VAL;
        }
        result = valueFromLong(vm, (long)d); // truncates, like Python
    }
    else if (valueIsBool(v))
        result = V_INT_VAL(valueAsBool(v) ? 1 : 0);
    else if (V_IS_OBJ_TYPE(v, OBJ_STRING))
    {
        MyMoString *str = AS_STRING(V_AS_OBJ(v));
        long n;
        if (!parseInt(str->value, str->length, (int)base, &n))
        {
            runtimeError(vm, "ValueError: invalid literal for int() with base %ld: '%s'", base, str->value);
            return V_EMPTY_VAL;
        }
        result = valueFromLong(vm, n);
    }
    else
    {
        runtimeError(vm, "TypeError: int() can't convert %s", valueTypeName(v));
        return V_EMPTY_VAL;
    }
    for (uint i = 0; i < argc; i++)
        popV(vm);
    return result;
}

void defineIntMethods(MVM *vm)
{
    defineMethod(vm, OBJ_INT, "__new__", newIntMethod);
}

void defineIntClass(MVM *vm)
{
    MyMoString *name = newString(vm, "int", 3);
    MyMoBuiltInClass *intClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_INT] = intClass;
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(intClass));
    defineIntMethods(vm);
}