#include "operations.h"
#include "memory.h"
#include "vm.h"
#include "datatypes/datatypes.h"

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

MyMoObject *addition(MVM *vm, MyMoObject *a, MyMoObject *b)
{
    if (IS_LIST(a) && IS_LIST(b))
    {
        MyMoList *out = newList(vm);
        concatArrays(vm, &out->values, &AS_LIST(a)->values, &AS_LIST(b)->values);
        return AS_OBJECT(out);
    }
    if (IS_TUPLE(a) && IS_TUPLE(b))
    {
        MyMoTuple *out = newTuple(vm);
        concatArrays(vm, &out->values, &AS_TUPLE(a)->values, &AS_TUPLE(b)->values);
        return AS_OBJECT(out);
    }
    if (IS_BOOL(a) && IS_BOOL(b))
    {
        bool a_val = BOOL_VAL(a);
        bool b_val = BOOL_VAL(b);
        return NEW_BOOL(a_val || b_val);
    }
    if (IS_BOOL(a))
    {
        a = NEW_INT(vm, BOOL_VAL(a));
    }
    if (IS_BOOL(b))
    {
        b = NEW_INT(vm, BOOL_VAL(b));
    }
    if (IS_INT(a) && IS_INT(b))
    {
        long a_val = INT_VAL(a);
        long b_val = INT_VAL(b);
        return NEW_INT(vm, a_val + b_val);
    }
    else if (IS_DOUBLE(a) && IS_DOUBLE(b))
    {
        double a_val = DOUBLE_VAL(a);
        double b_val = DOUBLE_VAL(b);
        return NEW_DOUBLE(vm, a_val + b_val);
    }
    else if ((IS_DOUBLE(a) && IS_INT(b)) || (IS_INT(a) && IS_DOUBLE(b)))
    {
        double a_val = IS_DOUBLE(a) ? DOUBLE_VAL(a) : (double)INT_VAL(a);
        double b_val = IS_DOUBLE(b) ? DOUBLE_VAL(b) : (double)INT_VAL(b);
        return NEW_DOUBLE(vm, a_val + b_val);
    }
    else if (IS_STRING(a) && IS_STRING(b))
    {
        MyMoString *a_val = AS_STRING(a);
        MyMoString *b_val = AS_STRING(b);
        int length = a_val->length + b_val->length;
        char *chars = New(char, length + 1);
        memcpy(chars, a_val->value, a_val->length);
        memcpy(chars + a_val->length, b_val->value, b_val->length);
        chars[length] = '\0';
        MyMoObject *result = NEW_STRING(vm, chars, length);
        Free(vm,char,chars);
        return result;
    }
    else
    {
        runtimeError(vm, "TypeError: can't perform + between  %s and %s", getType(a), getType(b));
        return NEW_EMPTY;
    }
}

MyMoObject *subtraction(MVM *vm, MyMoObject *a, MyMoObject *b)
{
    if (IS_BOOL(a) && IS_BOOL(b))
    {
        bool a_val = BOOL_VAL(a);
        bool b_val = BOOL_VAL(b);
        return NEW_BOOL(a_val && !b_val);
    }
    if (IS_BOOL(a))
    {
        a = NEW_INT(vm, BOOL_VAL(a));
    }
    if (IS_BOOL(b))
    {
        b = NEW_INT(vm, BOOL_VAL(b));
    }
    if (IS_INT(a) && IS_INT(b))
    {
        int a_val = INT_VAL(a);
        int b_val = INT_VAL(b);
        return NEW_INT(vm, a_val - b_val);
    }
    else if (IS_DOUBLE(a) && IS_DOUBLE(b))
    {
        double a_val = DOUBLE_VAL(a);
        double b_val = DOUBLE_VAL(b);
        return NEW_DOUBLE(vm, a_val - b_val);
    }
    else if ((IS_DOUBLE(a) && IS_INT(b)) || (IS_INT(a) && IS_DOUBLE(b)))
    {
        double a_val = IS_DOUBLE(a) ? DOUBLE_VAL(a) : (double)INT_VAL(a);
        double b_val = IS_DOUBLE(b) ? DOUBLE_VAL(b) : (double)INT_VAL(b);
        return NEW_DOUBLE(vm, a_val - b_val);
    }
    else
    {
        runtimeError(vm, "TypeError: can't perform - between  %s and %s", getType(a), getType(b));
        return NEW_EMPTY;
    }
}

MyMoObject *stringMultiple(MVM *vm, MyMoString *a, double b)
{
    int length = a->length * b;
    char *chars = New(char, length + 1);
    for (int i = 0; i < length; i++)
    {
        chars[i] = a->value[i % a->length];
    }
    chars[length] = '\0';
    MyMoObject *result = NEW_STRING(vm, chars, length);
    Free(vm,char,chars);
    return result;
}

MyMoObject *multiplication(MVM *vm, MyMoObject *a, MyMoObject *b)
{
    if (IS_BOOL(a) && IS_BOOL(b))
    {
        bool a_val = BOOL_VAL(a);
        bool b_val = BOOL_VAL(b);
        return NEW_BOOL(a_val && b_val);
    }
    if (IS_BOOL(a))
    {
        a = NEW_INT(vm, BOOL_VAL(a));
    }
    if (IS_BOOL(b))
    {
        b = NEW_INT(vm, BOOL_VAL(b));
    }
    if (IS_INT(a) && IS_INT(b))
    {
        int a_val = INT_VAL(a);
        int b_val = INT_VAL(b);
        return NEW_INT(vm, a_val * b_val);
    }
    else if (IS_DOUBLE(a) && IS_DOUBLE(b))
    {
        double a_val = DOUBLE_VAL(a);
        double b_val = DOUBLE_VAL(b);
        return NEW_DOUBLE(vm, a_val * b_val);
    }
    else if ((IS_DOUBLE(a) && IS_INT(b)) || (IS_INT(a) && IS_DOUBLE(b)))
    {
        double a_val = IS_DOUBLE(a) ? DOUBLE_VAL(a) : (double)INT_VAL(a);
        double b_val = IS_DOUBLE(b) ? DOUBLE_VAL(b) : (double)INT_VAL(b);
        return NEW_DOUBLE(vm, a_val * b_val);
    }
    else if (IS_STRING(a) && IS_INT(b))
    {
        return stringMultiple(vm, AS_STRING(a), INT_VAL(b));
    }
    else if (IS_INT(a) && IS_STRING(b))
    {
        return stringMultiple(vm, AS_STRING(b), INT_VAL(a));
    }
    else
    {
        runtimeError(vm, "TypeError: can't perform * between  %s and %s", getType(a), getType(b));
        return NEW_EMPTY;
    }
}

MyMoObject *division(MVM *vm, MyMoObject *a, MyMoObject *b)
{
    if (IS_BOOL(a) && IS_BOOL(b))
    {
        bool a_val = BOOL_VAL(a);
        bool b_val = BOOL_VAL(b);
        return NEW_BOOL(a_val && b_val);
    }
    if (IS_BOOL(a))
    {
        a = NEW_INT(vm, BOOL_VAL(a));
    }
    if (IS_BOOL(b))
    {
        b = NEW_INT(vm, BOOL_VAL(b));
    }
    if (IS_INT(a) && IS_INT(b))
    {
        long a_val = INT_VAL(a);
        double b_val = INT_VAL(b);
        double result = a_val / b_val;
        if (result == (int) result)
        {
            return NEW_INT(vm, (long)result);
        }
        else
        {
            return NEW_DOUBLE(vm, result);
        }
    }
    else if (IS_DOUBLE(a) && IS_DOUBLE(b))
    {
        double a_val = DOUBLE_VAL(a);
        double b_val = DOUBLE_VAL(b);
        return NEW_DOUBLE(vm, a_val / b_val);
    }
    else if ((IS_DOUBLE(a) && IS_INT(b)) || (IS_INT(a) && IS_DOUBLE(b)))
    {
        double a_val = IS_DOUBLE(a) ? DOUBLE_VAL(a) : (double)INT_VAL(a);
        double b_val = IS_DOUBLE(b) ? DOUBLE_VAL(b) : (double)INT_VAL(b);
        return NEW_DOUBLE(vm, a_val / b_val);
    }
    else
    {
        runtimeError(vm, "TypeError: can't perform / between  %s and %s", getType(a), getType(b));
        return NEW_EMPTY;
    }
}
