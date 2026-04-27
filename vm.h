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
    MyMoBuiltInClass *builtInClasses[OBJ_FUNCTION];
    MyMoDict builtins;
    MyMoDict modules;
    MyMoModule *currentModule;
    MyMoDict globals;
    MyMoDict strings;
    MyMoDict numbers;
    MyMoDict integers;
    MyMoDict doubles;
    MyMoDict builtInModules;
    MyMoObject *objects;
    u32 classCall;
    MyMoObject *wildcard;   // singleton for `_` in case-statement patterns
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

#endif