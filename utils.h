#ifndef __BUILTIN_H__
#define __BUILTIN_H__
#include "common.h"
#include "datatypes/function.h"

void defineBuiltInFunctions(MVM *vm);

char *pathResolver(MVM *vm, char *path);
MyMoFunction *runFile(MVM *vm, char *path);

void setProjectRoot(const char *scriptPath);

#endif