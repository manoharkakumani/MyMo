#ifndef __MVM_H__
#define __MVM_H__

#include "datatypes/fiber.h"
#include "datatypes/class.h"
#include "datatypes/module.h"
#include "memory.h"
#include "stack.h"

struct vm
{
    MyMoFiber *fiber;
    MyMoClass *currentClass;
    MyMoClass *objectClass;
    MyMoBuiltInClass *builtInClasses[OBJ_TYPE_COUNT]; // NULL = no methods
    MyMoDict builtins;
    MyMoDict modules;
    MyMoModule *currentModule;
    MyMoDict globals;
    MyMoDict strings;
    MyMoDict numbers;
    MyMoDict integers;
    MyMoDict doubles;
    MyMoDict tupleKeys; // canonical tuples used as dict keys (dict.c)
    MyMoDict builtInModules;
    MyMoObject *objects;
    u32 classCall;
    MyMoObject *wildcard;   // singleton for `_` in case-statement patterns
    // Garbage collector state (gc.c).
    MyMoFiber *rootFiber;       // the initial fiber; always a root
    size_t objectCount;         // live objects on `objects`
    size_t nextGC;              // collect once objectCount reaches this
    size_t gcCount;             // collections so far
    u32 gcEpoch;                // bumped per collection; marks CallFrames
    int runDepth;               // nested runMVM depth; GC only runs at 1
    bool gcStress;              // MYMO_GC_STRESS: collect at every safe point
    CallFrame *retiredFrames;   // captured/module frames off the call stack
    MyMoObject **grayStack;
    int grayCount;
    int grayCapacity;
    // Embedding (include/mymo.h). mymo_call runs a nested dispatch loop
    // that returns when the call's frame returns: exitFrame/exitFiber
    // mark that boundary (-1/NULL when not in a nested call). try
    // handlers from outside the boundary are not used inside it.
    int exitFrame;
    struct fiber *exitFiber;
    ValueArray hostRoots;       // values retained by the host (GC roots)
    char lastError[512];        // message of the most recent runtime error
};

typedef enum
{
    OK,
    RUNTIME_ERROR,
    COMPILE_ERROR
} I_Result;

MVM *initVM();
void freeVM(MVM *vm);
int runMVM(MVM *vm);
I_Result interpreter(MVM *vm, MyMoFunction *main_);
void runtimeError(MVM *vm, const char *format, ...);

bool caller(MVM *vm, MyMoObject *callee, u32 argc);
void unwindFrames(MVM *vm, uint depth);

#endif