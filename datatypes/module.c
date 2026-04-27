#include "module.h"
#include "../modules/modules.h"
#include "../vm.h"
#include "nil.h"

#ifdef _WIN32
  #include <Windows.h>
#else
    #include <dlfcn.h>
#endif

MyMoModule *newModule(MVM *vm, MyMoString *name, MyMoString *path)
{
    MyMoModule *module = AllocateObject(vm, MyMoModule, OBJ_MODULE);
    module->name = name;
    module->path = path;
    module->variables = newDict(vm);
    module->parent = NULL;
    setEntry(vm, &vm->modules, AS_OBJECT(path), AS_OBJECT(module));
    return module;
}

void printModule(MyMoModule *module)
{
    switch (module->type)
    {
    case MODULE_BULTIN:
        printf("<module '%s' (built-in)>", module->name->value);
        break;
    default:
        printf("<module %s at %p>", module->name->value, module);

    }
}

void defineModuleFunctions(MVM *vm, MyMoModule *module, MyMoModuleFunction functions[], size_t size){
    for (int i = 0; i < size; i++)
    {
        MyMoObject *name = NEW_STRING(vm, functions[i].name, strlen(functions[i].name));
        MyMoObject *function = AS_OBJECT(newBuiltInFunction(vm, AS_STRING(name), functions[i].function, OBJ_BUILTIN_FUNCTION));
        setEntry(vm,module->variables, name, function);        
    }
}

void defineModuleVariables(MVM *vm, MyMoModule *module, MyMoModuleVariable variables[], size_t size){
    for (int i = 0; i < size; i++)
    {
        setEntry(vm,module->variables, NEW_STRING(vm, variables[i].name, strlen(variables[i].name)), variables[i].variable);        
    }
}

MyMoObject *defineBuiltInModule(MVM *vm, MyMoModuleDef *moduleDef)
{
    char path[1024];
    snprintf(path, sizeof(path), "module '%s' (built-in)", moduleDef->name);
    MyMoModule *module = newModule(vm,newString(vm, moduleDef->name, strlen(moduleDef->name)), newString(vm, path, strlen(path)));
    defineModuleFunctions(vm, module, moduleDef->functions, moduleDef->totalfn);
    defineModuleVariables(vm, module, moduleDef->variables, moduleDef->totalvar);
    setEntry(vm,&vm->builtInModules,AS_OBJECT(module->name),AS_OBJECT(module));
    return AS_OBJECT(module);
}

// Search-path order for `from "name" use ...` when `name.my` doesn't exist
// and the module isn't already in vm->builtInModules:
//   1. $MYMO_HOME/lib/<name>mod.<ext>          (per-user / install root)
//   2. ./<name>mod.<ext>                       (current working directory)
//   3. ./modules/<name>mod.<ext>               (alongside built-in C modules)
//   4. /opt/mymo/lib/<name>mod.<ext>           (system default)
// Where <ext> is .dylib on macOS, .so on Linux, .dll on Windows.

static const char *moduleExt(void)
{
#if defined(__APPLE__)
    return ".dylib";
#elif defined(_WIN32)
    return ".dll";
#else
    return ".so";
#endif
}

static void *tryDlopen(const char *path)
{
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
}

static void *tryDlsym(void *handle, const char *symbol)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)handle, symbol);
#else
    return dlsym(handle, symbol);
#endif
}

static int fileExists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

MyMoObject *loadBuiltInModule(MVM *vm, MyMoString *name)
{
    char path[MAX_PATH + 1];
    void *handle = NULL;
    const char *ext = moduleExt();
    const char *home = getenv(MYMO_ENV);

    // Resolution chain — first match wins.
    const char *bases[] = { home, ".", "./modules", "/opt/mymo" };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
    {
        if (!bases[i]) continue;
        if (i == 0 || i == 3)
            snprintf(path, sizeof(path), "%s/lib/%smod%s", bases[i], name->value, ext);
        else
            snprintf(path, sizeof(path), "%s/%smod%s", bases[i], name->value, ext);
        if (!fileExists(path)) continue;
        handle = tryDlopen(path);
        if (handle) break;
    }
    if (!handle)
    {
#ifndef _WIN32
        // Last fallback: bare-name dlopen so the OS dynamic-loader's
        // LD_LIBRARY_PATH / DYLD_LIBRARY_PATH can find it.
        snprintf(path, sizeof(path), "%smod%s", name->value, ext);
        handle = tryDlopen(path);
#endif
    }
    if (!handle) return NEW_EMPTY;

    // Look up the entry point: <name>Module(MVM *vm) -> MyMoObject*.
    char symbolName[MAX_PATH + 1];
    snprintf(symbolName, sizeof(symbolName), "%sModule", name->value);
    loadModule entry = (loadModule)tryDlsym(handle, symbolName);
    if (!entry)
    {
#ifndef _WIN32
        fputs(dlerror(), stderr);
#endif
        return NEW_EMPTY;
    }
    MyMoObject *module = entry(vm);
    if (!module || IS_EMPTY(module)) return NEW_EMPTY;
    // Cache so the same `from "x" use ...` later re-uses without
    // re-dlopening. defineBuiltInModule (called by entry()) already
    // inserted into vm->builtInModules, so this is idempotent.
    return module;
}

const char *getHomeDir(void)
{
    const char *home = getenv(MYMO_ENV);
    return home ? home : "/opt/mymo";
}