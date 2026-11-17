#ifndef __MODULE_H__
#define __MODULE_H__

#include "object.h"
#include "dict.h"
#include "string.h"
#include "function.h"

#define AS_MODULE(object) ((MyMoModule *)object)
#define IS_MODULE(object) (object->type == OBJ_MODULE)


#define MYMO_ENV "MYMO_HOME"

#ifdef _WIN32
  #define FILE_INFIX   "\\lib\\"
  #define FILE_POSTFIX "mod.dll"
#else 
  #define FILE_INFIX "/lib"
#endif

#ifdef __linux__
  #define FILE_POSTFIX "mod.so"
#endif

#ifdef __APPLE__
  #define FILE_POSTFIX "mod.dylib"
#endif

#ifdef _WIN32
  typedef MyMoObject* (__stdcall *loadModule)(MVM *vm);
#else
  typedef MyMoObject* (*loadModule)(MVM *vm);
#endif

#ifdef _WIN32
  #define MODULE(n)  MyMoObject* __declspec(dllexport) __stdcall n##Module(MVM *vm)
#else
  #define MODULE(n)  MyMoObject* n##Module(MVM *vm)
#endif

typedef enum{
    MODULE_BULTIN,
    MODULE_DEFINED
}ModuleType;

typedef struct MyMoModule
{
    MyMoObject object;
    ModuleType type;
    MyMoString *name;
    MyMoString *path;
    MyMoDict *variables;
    // The frame that ran the module's body, once it finished: its locals
    // are the module's live variables (its functions assign them through
    // `global`), so `mod.name` reads and writes go there.
    struct CallFrame *frame;
    struct MyMoModule *parent;
} MyMoModule;

typedef struct MyMoModuleFunction
{
    const char *name;
    BuiltInfunction function;
}MyMoModuleFunction;

typedef struct MyMoModuleVariable
{
    const char *name;
    MyMoObject *variable;
}MyMoModuleVariable;

typedef struct MyMoModuleDef
{
    const char *name;
    MyMoModuleFunction *functions;
    MyMoModuleVariable *variables;
    size_t totalfn;
    size_t totalvar;
}MyMoModuleDef;

MyMoModule *newModule(MVM *vm, MyMoString *name, MyMoString *path);
void printModule(MyMoModule *module);

MyMoObject *defineBuiltInModule(MVM *vm, MyMoModuleDef *moduleDef);

MyMoObject *loadBuiltInModule(MVM *vm, MyMoString *name);

#endif