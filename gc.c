// Mark-sweep garbage collector.
//
// When it runs: only at safe points in the dispatch loop (loop back-edges
// and call entry, see GC_SAFEPOINT in vm.c), and only at the outermost
// runMVM level. At those points every live value is on a fiber stack, in
// a call frame, or in a VM root, and no C code is holding an object in a
// local variable, so C builtins never need to protect their temporaries.
//
// Roots: the current and root fibers (with their stacks and frames),
// globals, builtins, the module tables, built-in classes, the current
// class/module, values retained by an embedding host, and the
// process-wide singletons (nil, true, false, the empty sentinel, the `_`
// wildcard).
//
// Weak tables: the intern tables (strings, ints, doubles, numbers) don't
// keep their entries alive. After marking, dead entries are dropped and
// each table is rebuilt (the find* probes stop at empty slots, so the
// tables must never contain tombstones).
//
// Frames: CallFrames aren't heap objects. A frame that a closure captured
// (or a module's frame) outlives its call; when it leaves the call stack
// it goes on vm->retiredFrames, and is freed once no function's `frame`
// field reaches it any more. Frames are marked with the collection epoch.

#include "gc.h"
#include "vm.h"
#include "datatypes/datatypes.h"
#include <stdlib.h>

#define GC_MIN_THRESHOLD 10000

static void markObject(MVM *vm, MyMoObject *object)
{
    if (object == NULL || object->refCount)
        return;
    object->refCount = 1; // mark bit (the header's refCount is otherwise unused)
    if (vm->grayCount == vm->grayCapacity)
    {
        vm->grayCapacity = vm->grayCapacity < 64 ? 64 : vm->grayCapacity * 2;
        vm->grayStack = realloc(vm->grayStack, sizeof(MyMoObject *) * (size_t)vm->grayCapacity);
        if (vm->grayStack == NULL)
        {
            fprintf(stderr, "fatal: out of memory during garbage collection\n");
            exit(71);
        }
    }
    vm->grayStack[vm->grayCount++] = object;
}

static void markValue(MVM *vm, Value value)
{
    if (V_IS_OBJ(value))
        markObject(vm, V_AS_OBJ(value));
}

static void markDictContents(MVM *vm, MyMoDict *dict)
{
    if (dict == NULL || dict->entries == NULL)
        return;
    for (int i = 0; i <= dict->capacity; i++)
    {
        Entry *entry = &dict->entries[i];
        if (entry->key == NULL)
            continue;
        markObject(vm, entry->key);
        markValue(vm, entry->value);
    }
}

static void markArray(MVM *vm, MyMoObjectArray *array)
{
    for (int i = 0; i < array->count; i++)
        markObject(vm, array->objects[i]);
}

static void markValues(MVM *vm, ValueArray *array)
{
    for (int i = 0; i < array->count; i++)
        markValue(vm, array->values[i]);
}

static void markFrame(MVM *vm, CallFrame *frame)
{
    if (frame == NULL || frame->gcEpoch == vm->gcEpoch)
        return;
    frame->gcEpoch = vm->gcEpoch;
    MyMoFunction *function = frame->function;
    markObject(vm, AS_OBJECT(function));
    markDictContents(vm, &frame->locals);
    // Parameters live in args[] for functions with few of them.
    if (function && function->argc <= CALLFRAME_ARGS_INLINE)
        for (int i = 0; i < function->argc; i++)
            markValue(vm, frame->args[i]);
}

static void blacken(MVM *vm, MyMoObject *object)
{
    switch (object->type)
    {
    case OBJ_LIST:
        markValues(vm, &AS_LIST(object)->values);
        break;
    case OBJ_TUPLE:
        markValues(vm, &AS_TUPLE(object)->values);
        break;
    case OBJ_DICT:
        markDictContents(vm, AS_DICT(object));
        break;
    case OBJ_SET:
        markDictContents(vm, &AS_SET(object)->items);
        break;
    case OBJ_FIBER:
    {
        MyMoFiber *fiber = AS_FIBER(object);
        for (int i = 0; i < fiber->stack.count; i++)
            markValue(vm, fiber->stack.values[i]);
        if (fiber->callFrames)
            for (uint i = 0; i <= fiber->frameCount; i++)
                markFrame(vm, fiber->callFrames[i]);
        markObject(vm, AS_OBJECT(fiber->parent));
        markObject(vm, fiber->exception);
        break;
    }
    case OBJ_FUNCTION:
    {
        MyMoFunction *function = AS_FUNCTION(object);
        markObject(vm, AS_OBJECT(function->name));
        for (int i = 0; i < function->argc; i++)
            markObject(vm, AS_OBJECT(function->argv[i]));
        if (function->chunk)
            for (int i = 0; i < function->chunk->constants.count; i++)
                markValue(vm, function->chunk->constants.values[i]);
        markObject(vm, AS_OBJECT(function->variables));
        markObject(vm, AS_OBJECT(function->assiginedParameters));
        markObject(vm, function->klass);
        // A closure copy shares its prototype's chunk and argv.
        markObject(vm, AS_OBJECT(function->proto));
        markFrame(vm, function->frame);
        markObject(vm, AS_OBJECT(function->bound));
        for (int i = 0; i < function->defaultCount; i++)
            markValue(vm, function->defaults[i]);
        break;
    }
    case OBJ_CLOUSER:
        markObject(vm, AS_OBJECT(AS_CLOUSER(object)->function));
        markObject(vm, AS_OBJECT(AS_CLOUSER(object)->variables));
        break;
    case OBJ_BOUND_METHOD:
        markObject(vm, AS_BOUND_METHOD(object)->self);
        markObject(vm, AS_OBJECT(AS_BOUND_METHOD(object)->method));
        break;
    case OBJ_BUILTIN_FUNCTION:
    case OBJ_BUILTIN_METHOD:
        markObject(vm, AS_OBJECT(AS_BUILTIN_FUNCTION(object)->name));
        markObject(vm, AS_BUILTIN_FUNCTION(object)->self);
        break;
    case OBJ_CLASS:
    {
        MyMoClass *klass = AS_CLASS(object);
        markObject(vm, AS_OBJECT(klass->name));
        markObject(vm, AS_OBJECT(klass->methods));
        markObject(vm, AS_OBJECT(klass->fields));
        markObject(vm, AS_OBJECT(klass->variables));
        markObject(vm, klass->init);
        markArray(vm, &klass->superClasses);
        markObject(vm, AS_OBJECT(klass->enclosing));
        break;
    }
    case OBJ_BUILTIN_CLASS:
        markObject(vm, AS_OBJECT(AS_BUILTIN_CLASS(object)->name));
        markObject(vm, AS_OBJECT(AS_BUILTIN_CLASS(object)->methods));
        break;
    case OBJ_INSTANCE:
        markObject(vm, AS_OBJECT(AS_INSTANCE(object)->klass));
        markObject(vm, AS_OBJECT(AS_INSTANCE(object)->fields));
        break;
    case OBJ_MODULE:
    {
        MyMoModule *module = AS_MODULE(object);
        markObject(vm, AS_OBJECT(module->name));
        markObject(vm, AS_OBJECT(module->path));
        markObject(vm, AS_OBJECT(module->variables));
        markObject(vm, AS_OBJECT(module->parent));
        markFrame(vm, module->frame);
        break;
    }
    case OBJ_CODE:
        markObject(vm, AS_OBJECT(AS_CODE(object)->function));
        break;
    case OBJ_ITER:
        markObject(vm, AS_ITER(object)->iterator);
        break;
    case OBJ_SUPER:
        markObject(vm, AS_SUPER(object)->self);
        markObject(vm, AS_OBJECT(AS_SUPER(object)->klass));
        break;
    default: // strings, numbers, nil/bool/empty, wildcard: no references
        break;
    }
}

static void markRoots(MVM *vm)
{
    markObject(vm, AS_OBJECT(vm->fiber));
    markObject(vm, AS_OBJECT(vm->rootFiber));
    markDictContents(vm, &vm->globals);
    markDictContents(vm, &vm->builtins);
    markDictContents(vm, &vm->modules);
    markDictContents(vm, &vm->builtInModules);
    for (int i = 0; i < OBJ_TYPE_COUNT; i++)
        markObject(vm, AS_OBJECT(vm->builtInClasses[i]));
    markObject(vm, AS_OBJECT(vm->currentClass));
    markObject(vm, AS_OBJECT(vm->objectClass));
    markObject(vm, AS_OBJECT(vm->currentModule));
    markObject(vm, vm->wildcard);
    markObject(vm, AS_OBJECT(NilObject));
    markObject(vm, AS_OBJECT(TrueBool));
    markObject(vm, AS_OBJECT(FalseBool));
    markObject(vm, AS_OBJECT(EmptyObject));
    // Values the embedding host retained (mymo_retain).
    for (int i = 0; i < vm->hostRoots.count; i++)
        markValue(vm, vm->hostRoots.values[i]);
    if (vm->kwargs)
        markObject(vm, AS_OBJECT(vm->kwargs));
}

// Drop unmarked keys from an intern table and rebuild it without
// tombstones (findString & co. stop probing at the first empty key).
static void sweepInternTable(MVM *vm, MyMoDict *table)
{
    if (table->entries == NULL)
        return;
    Entry *old = table->entries;
    int oldCapacity = table->capacity;
    initDict(table);
    for (int i = 0; i <= oldCapacity; i++)
        if (old[i].key != NULL && old[i].key->refCount)
            setEntryV(vm, table, old[i].key, old[i].value);
    free(old);
}

static void sweepObjects(MVM *vm)
{
    MyMoObject *previous = NULL;
    MyMoObject *object = vm->objects;
    while (object != NULL)
    {
        if (object->refCount)
        {
            object->refCount = 0;
            previous = object;
            object = object->next;
            continue;
        }
        MyMoObject *unreached = object;
        object = object->next;
        if (previous)
            previous->next = object;
        else
            vm->objects = object;
        freeObject(vm, unreached);
        vm->objectCount--;
    }
}

static void sweepRetiredFrames(MVM *vm)
{
    CallFrame **link = &vm->retiredFrames;
    while (*link)
    {
        CallFrame *frame = *link;
        if (frame->gcEpoch == vm->gcEpoch)
        {
            link = &frame->nextRetired;
            continue;
        }
        *link = frame->nextRetired;
        freeDict(vm, &frame->locals);
        free(frame);
    }
}

void retireFrame(MVM *vm, CallFrame *frame)
{
    frame->nextRetired = vm->retiredFrames;
    vm->retiredFrames = frame;
}

void freeRetiredFrames(MVM *vm)
{
    while (vm->retiredFrames)
    {
        CallFrame *frame = vm->retiredFrames;
        vm->retiredFrames = frame->nextRetired;
        freeDict(vm, &frame->locals);
        free(frame);
    }
}

void collectGarbage(MVM *vm)
{
    vm->gcEpoch++;
    markRoots(vm);
    while (vm->grayCount > 0)
        blacken(vm, vm->grayStack[--vm->grayCount]);
    sweepInternTable(vm, &vm->strings);
    sweepInternTable(vm, &vm->integers);
    sweepInternTable(vm, &vm->doubles);
    sweepInternTable(vm, &vm->tupleKeys);
    sweepInternTable(vm, &vm->numbers);
    sweepObjects(vm);
    sweepRetiredFrames(vm);
    size_t next = vm->objectCount * 2;
    vm->nextGC = vm->gcStress ? 0 : (next < GC_MIN_THRESHOLD ? GC_MIN_THRESHOLD : next);
    vm->gcCount++;
}

void initGC(MVM *vm)
{
    vm->objectCount = 0;
    vm->nextGC = GC_MIN_THRESHOLD;
    vm->gcCount = 0;
    vm->gcEpoch = 0;
    vm->runDepth = 0;
    vm->retiredFrames = NULL;
    vm->grayStack = NULL;
    vm->grayCount = 0;
    vm->grayCapacity = 0;
    const char *stress = getenv("MYMO_GC_STRESS");
    vm->gcStress = stress != NULL && stress[0] != '\0' && stress[0] != '0';
    if (vm->gcStress)
        vm->nextGC = 0;
}
