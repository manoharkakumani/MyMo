#include "range.h"
#include "../memory.h"
#include "../vm.h"
#include "class.h"
#include "function.h"
#include "dict.h"

MyMoRange *newRange(MVM *vm, long start, long stop, long step)
{
    MyMoRange *range = AllocateObject(vm, MyMoRange, OBJ_RANGE);
    range->start = start;
    range->stop = stop;
    range->step = step;
    return range;
}

long rangeLength(MyMoRange *r)
{
    if (r->step > 0 && r->start < r->stop)
        return (r->stop - r->start - 1) / r->step + 1;
    if (r->step < 0 && r->start > r->stop)
        return (r->start - r->stop - 1) / -r->step + 1;
    return 0;
}

long rangeAt(MyMoRange *r, long i)
{
    return r->start + i * r->step;
}

bool rangeContains(MyMoRange *r, long n)
{
    if (r->step > 0 ? (n < r->start || n >= r->stop) : (n > r->start || n <= r->stop))
        return false;
    return (n - r->start) % r->step == 0;
}

static bool rangeBound(MVM *vm, Value v, long *out)
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
    runtimeError(vm, "TypeError: range() arguments must be integers, not %s", valueTypeName(v));
    return false;
}

// range(stop) | range(start, stop) | range(start, stop, step)
static Value newRangeMethod(MVM *vm, uint argc, Value args[])
{
    if (argc < 1 || argc > 3)
    {
        runtimeError(vm, "TypeError: range() takes 1 to 3 arguments (%u given)", argc);
        return V_EMPTY_VAL;
    }
    long bounds[3] = {0, 0, 1};
    for (uint i = 0; i < argc; i++)
        if (!rangeBound(vm, args[i], &bounds[argc == 1 ? 1 : i]))
            return V_EMPTY_VAL;
    if (bounds[2] == 0)
    {
        runtimeError(vm, "ValueError: range() step must not be zero");
        return V_EMPTY_VAL;
    }
    for (uint i = 0; i < argc; i++)
        popV(vm);
    return V_OBJ_VAL(AS_OBJECT(newRange(vm, bounds[0], bounds[1], bounds[2])));
}

static Value lenRangeMethod(MVM *vm, uint argc, Value args[])
{
    MyMoObject *self = methodEnter(vm, "__len__", argc, 0, 0);
    if (!self) return V_EMPTY_VAL;
    return valueFromLong(vm, rangeLength(AS_RANGE(self)));
}

void defineRangeClass(MVM *vm)
{
    MyMoString *name = newString(vm, "range", 5);
    MyMoBuiltInClass *rangeClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_RANGE] = rangeClass;
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(rangeClass));
    defineMethod(vm, OBJ_RANGE, "__new__", newRangeMethod);
    defineMethod(vm, OBJ_RANGE, "__len__", lenRangeMethod);
}
