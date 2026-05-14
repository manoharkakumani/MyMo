#ifndef __FIBER_H__
#define __FIBER_H__

#include "object.h"
#include "function.h"
#include "dict.h"
#include "../value.h"

#define AS_FIBER(object) ((MyMoFiber *)object)
#define IS_FIBER(object) (object->type == OBJ_FIBER)
#define IS_FIBER_RUNNING(fiber) (fiber->state == FIBER_RUNNING)
#define IS_FIBER_READY(fiber) (fiber->state == FIBER_READY)
#define IS_FIBER_SUSPENDED(fiber) (fiber->state == FIBER_SUSPENDED)
#define IS_FIBER_DEAD(fiber) (fiber->state == FIBER_DEAD)
#define IS_FIBER_CHILD(fiber) (fiber->type == FIBER_CHILD)
#define IS_FIBER_ROOT(fiber) (fiber->type == FIBER_ROOT)

#define NEW_FIBER(vm, value) AS_OBJECT(newFiber(vm, value))

typedef enum
{
    FIBER_READY,
    FIBER_RUNNING,
    FIBER_YIELD,
    FIBER_DEAD
} FiberState;

typedef enum
{
    FIBER_ROOT,
    FIBER_CHILD
} FiberType;

// One entry on the try/catch handler stack. Recorded by OP_TRY when
// a `try:` block starts executing, consulted by runtimeError when an
// exception fires so it can unwind to the matching `catch:` arm.
typedef struct {
    u8 *handlerIp;       // bytecode address of the catch arm
    uint frameCount;     // value to restore vm->fiber->frameCount to
    int  stackCount;     // value to restore vm->fiber->stack.count to
} TryHandler;

typedef struct fiber
{
    MyMoObject object;
    FiberState state;
    FiberType type;
    ValueArray stack;
    CallFrame **callFrames;
    uint frameCount;
    uint frameCapacity;
    struct fiber *parent;
    // Free-list of recyclable CallFrame allocations. callFunction picks
    // from here when non-empty, falling back to malloc; OP_FRET pushes
    // the frame back instead of free()ing. Saves a malloc + free per call
    // pair — ~30 ns each on common allocators.
    CallFrame *freeFramesHead;
    // Try/catch handler stack. `handlers[handlerCount-1]` is the
    // innermost active handler. OP_TRY pushes, OP_ENDTRY pops,
    // runtimeError consults to unwind. Bounded depth to keep the
    // fiber struct compact — most programs nest <8 deep.
    TryHandler handlers[32];
    int handlerCount;
    // The currently-in-flight exception value (NULL when no error
    // is being propagated). Set by raise / runtimeError, read by
    // the catch arm via OP_GETV on the bound name.
    MyMoObject *exception;
} MyMoFiber;

MyMoFiber *newFiber(MVM *vm, MyMoFunction *function);
void printFiber(MyMoFiber *fiber);
void freeFiber(MVM *vm, MyMoFiber *fiber);

void defineFiberClass(MVM *vm);

#endif
