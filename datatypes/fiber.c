#include "fiber.h"
#include "../vm.h"
#include "nil.h"
#include "bool.h"

MyMoFiber *newFiber(MVM *vm, MyMoFunction *function)
{
    MyMoFiber *fiber = AllocateObject(vm, MyMoFiber, OBJ_FIBER);
    fiber->callFrames = New(CallFrame *, 1);
    fiber->frameCount = 0;
    fiber->frameCapacity = 0;
    fiber->state = FIBER_READY;
    fiber->type = FIBER_CHILD;
    fiber->parent = NULL;
    fiber->freeFramesHead = NULL;
    initValueArray(vm, &fiber->stack);
    // Pre-allocate operand stack. The dispatch-loop register `sp` always
    // points into this buffer and never grows during execution — overflow
    // beyond `stackCapacity` is undefined. 64K slots = 512 KiB per fiber.
    // Real growth-on-overflow lands as part of the GC/runtime polish pass.
    fiber->stack.capacity = 65536;
    fiber->stack.values = ResizeArray(vm, Value, fiber->stack.values, 0, 65536);
    CallFrame *frame = New(CallFrame, 1);
    frame->function = function;
    initDict(&frame->locals);
    fiber->callFrames[0] = frame;
    if (function)
    {
        frame->ip = function->chunk->code;
    }
    return fiber;
}

void freeFiber(MVM *vm, MyMoFiber *fiber)
{
    for (size_t i = 0; i <= fiber->frameCount; i++)
    {
        freeDict(vm, &fiber->callFrames[i]->locals);
        free(fiber->callFrames[i]);
    }
    // Drain the free-frame pool. While on the free list, frame->function
    // holds the next-free pointer; the locals dict is empty (already
    // freed by the OP_FRET that pushed it).
    CallFrame *fp = fiber->freeFramesHead;
    while (fp)
    {
        CallFrame *next = (CallFrame *)fp->function;
        free(fp);
        fp = next;
    }
    FreeArray(vm, CallFrame *, fiber->callFrames, fiber->frameCapacity);
    freeValueArray(vm, &fiber->stack);
    free(fiber);
}

void printFiber(MyMoFiber *fiber)
{
    printf("<object fiber of %s at %p>", fiber->callFrames[0]->function->name->value, fiber);
}

MyMoObject *newFiberMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 1)
    {
        runtimeError(vm, "__new__() takes exactly 1 argument (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoObject *function = pop(vm);
    if (!IS_FUNCTION(args[0]))
    {
        runtimeError(vm, "fiber() takes a <object 'function'> as argument but %s is given", getType(function));
        return NEW_EMPTY;
    }
    return NEW_FIBER(vm, AS_FUNCTION(function));
}

MyMoObject *runFiberMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, argc));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: run() can only be applied on instance");
        return NEW_EMPTY;
    }
    MyMoFiber *fiber = AS_FIBER(function->self);
    if (fiber->state == FIBER_RUNNING)
    {
        runtimeError(vm, "RuntimeError: cannot run a running fiber");
        return NEW_EMPTY;
    }
    else if (fiber->state == FIBER_YIELD)
    {
        runtimeError(vm, "RuntimeError: cannot run a yielded fiber");
        return NEW_EMPTY;
    }
    CallFrame *frame = fiber->callFrames[0];
    if (frame->function->argc != argc)
    {
        runtimeError(vm, "TypeError: %s() takes exactly %d arguments (%d given)", frame->function->name->value, frame->function->argc, argc);
        return NEW_EMPTY;
    }
    MyMoFunction *fiberFunction = frame->function;
    if (fiber->state == FIBER_DEAD)
    {
        frame->ip = fiberFunction->chunk->code;
    }
    if (argc && !fiberFunction->isargs)
    {
        for (int i = argc - 1; i >= 0; i--)
        {
            setEntry(vm, &frame->locals, AS_OBJECT(fiberFunction->argv[i]), pop(vm));
        }
    }
    fiber->state = FIBER_RUNNING;
    fiber->parent = vm->fiber;
    vm->fiber = fiber;
    push(vm, AS_OBJECT(function));
    return AS_OBJECT(fiberFunction);
}

MyMoObject *resumFiberMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, argc));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: resume() can only be applied on instance");
        return NEW_EMPTY;
    }
    MyMoFiber *fiber = AS_FIBER(function->self);
    if (fiber->state == FIBER_DEAD)
    {
        runtimeError(vm, "RuntimeError: cannot resume a dead fiber");
        return NEW_EMPTY;
    }
    else if (fiber->state == FIBER_RUNNING)
    {
        runtimeError(vm, "RuntimeError: cannot resume a running fiber");
        return NEW_EMPTY;
    }
    else if (fiber->state == FIBER_READY)
    {
        runtimeError(vm, "RuntimeError: cannot resume a ready fiber");
        return NEW_EMPTY;
    }
    MyMoObject *value = NEW_NIL;
    if (argc)
    {
        if (argc > 1)
        {
            runtimeError(vm, "TypeError: resume() takes exactly 0 or 1 argument (%d given)", argc);
            return NEW_EMPTY;
        }
        value = pop(vm);
    }
    fiber->state = FIBER_RUNNING;
    fiber->parent = vm->fiber;
    vm->fiber = fiber;
    return value;
}

MyMoObject *aliveFiberMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "alive() takes exactly 0 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, argc));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: alive() can only be applied on instance");
        return NEW_EMPTY;
    }
    MyMoFiber *fiber = AS_FIBER(function->self);
    return NEW_BOOL(fiber->state != FIBER_DEAD);
}

MyMoObject *killFiberMethod(MVM *vm, uint argc, MyMoObject *args[])
{
    if (argc != 0)
    {
        runtimeError(vm, "kill() takes exactly 0 arguments (%d given)", argc);
        return NEW_EMPTY;
    }
    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(peek(vm, argc));
    if (function->self == NULL)
    {
        runtimeError(vm, "TypeError: kill() can only be applied on instance");
        return NEW_EMPTY;
    }
    MyMoFiber *fiber = AS_FIBER(function->self);
    if (fiber->state == FIBER_DEAD)
    {
        runtimeError(vm, "RuntimeError: cannot kill a dead fiber");
        return NEW_EMPTY;
    }
    fiber->state = FIBER_DEAD;
    return NEW_BOOL(1);
}

void defineFiberMethods(MVM *vm)
{
    defineMethod(vm, OBJ_FIBER, "__new__", newFiberMethod);
    defineMethod(vm, OBJ_FIBER, "run", runFiberMethod);
    defineMethod(vm, OBJ_FIBER, "alive", aliveFiberMethod);
    defineMethod(vm, OBJ_FIBER, "resume", resumFiberMethod);
    defineMethod(vm, OBJ_FIBER, "kill", killFiberMethod);
}

void defineFiberClass(MVM *vm)
{
    MyMoString *name = newString(vm, "fiber", 5);
    MyMoBuiltInClass *fiberClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_FIBER] = fiberClass;
    defineFiberMethods(vm);
    setEntry(vm, &vm->builtins, AS_OBJECT(name), AS_OBJECT(fiberClass));
}