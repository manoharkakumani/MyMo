#include "memory.h"
#include "vm.h"
#include "datatypes/int.h"

// ---- Value-native primitives (preferred path) ------------------------------

void pushV(MVM *vm, Value v)
{
    writeValueArray(vm, &vm->fiber->stack, v);
}

Value popV(MVM *vm)
{
    return vm->fiber->stack.values[--vm->fiber->stack.count];
}

Value peekV(MVM *vm, int position)
{
    return vm->fiber->stack.values[vm->fiber->stack.count - position - 1];
}

// ---- Legacy object-pointer API (compatibility wrappers) --------------------
// These let pre-Phase-1 callers keep working in terms of MyMoObject*. When
// the popped/peeked Value is an inline tagged primitive (int today; double /
// nil / bool in steps 1.4-1.5), it is "boxed" into a heap object on demand.
// Boxing costs one allocation per call — a deliberate tax on legacy callers
// that pushes them to migrate to popV / peekV. Hot paths (arithmetic OPs in
// vm.c) use popV directly and pay no boxing cost.
//
// Once every caller has migrated (step 1.7), this whole section deletes.

static MyMoObject *boxValueAsObject(MVM *vm, Value v)
{
    if (V_IS_OBJ(v)) return V_AS_OBJ(v);
    if (V_IS_INT(v)) return AS_OBJECT(newInt(vm, V_AS_INT(v)));
    // Doubles, nil, true, false are still heap-allocated today, so they
    // cannot reach this branch as inline values. After steps 1.4 / 1.5 this
    // helper grows the matching cases.
    return V_AS_OBJ(v);
}

void push(MVM *vm, MyMoObject *object)
{
    pushV(vm, V_OBJ_VAL(object));
}

MyMoObject *pop(MVM *vm)
{
    return boxValueAsObject(vm, popV(vm));
}

MyMoObject *peek(MVM *vm, int position)
{
    return boxValueAsObject(vm, peekV(vm, position));
}

// ---- Debug / introspection -------------------------------------------------

void printStack(MVM *vm)
{
    int d = 0;
    printf("\n\t\t[ ");
    while (d < vm->fiber->stack.count)
    {
        printf(" ( ");
        printValue(vm->fiber->stack.values[d]);
        printf(" ) ");
        d++;
    }
    printf(" ]\n\n");
}
