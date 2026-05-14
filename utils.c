#include "memory.h"
#include "compiler.h"
#include "utils.h"
#include "vm.h"
#include "./datatypes/datatypes.h"
#include <time.h>
#include "cache.h"
#include "modules/stdlib_src.h"
#include <sys/stat.h>

//platform dependent
#ifdef _WIN32
#define IS_DELIMITER(s) ((s == '/') || (s == '\\'))
#include <direct.h>
typedef struct _stat  Stat;
#else
#define IS_DELIMITER(s) (s == '/')
typedef struct stat  Stat;
#endif

MyMoObject *clockfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    UNUSED(vm);
    UNUSED(argv);
    if (argc)
    {
        runtimeError(vm, "TypeError: clock() takes no arguments (%d given).", argc);
        return NEW_EMPTY;
    }
    return NEW_DOUBLE(vm, (double)clock() / CLOCKS_PER_SEC);
}


MyMoObject *inputfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: input() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (argc != 0)
    {
        MyMoObject *prompt = argv[0];
        if (!IS_STRING(prompt))
        {
            runtimeError(vm, "TypeError: input() only takes a <object 'str'> got %s.", getType(prompt));
            return NEW_EMPTY;
        }
        printf("%s", STRING_VAL(prompt));
    }
    u64 currentSize = 128;
    char *line = malloc(currentSize);
    // if (line == NULL) MemoryError("MVM out of memory on input()!");
    int c = EOF;
    u64 i = 0;
    while ((c = getchar()) != '\n' && c != EOF)
    {
        line[i++] = (char)c;
        if (i == currentSize)
        {
            currentSize = ResizeCapacity(currentSize);
            line = realloc(line, currentSize);
            // if (line == NULL) MemoryError("MVM out of memory on input()!");
        }
    }
    line[i] = '\0';
    MyMoObject *l = AS_OBJECT(NEW_STRING(vm, line, strlen(line)));
    free(line);
    return l;
}

MyMoObject *printfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    UNUSED(vm);
    if (argc == 0)
    {
        printf("\n");
        return NEW_BOOL(0);
    }
    int i = 0;
    for (i = 0; i < (argc - 1); i++)
    {
        printObject(argv[i]);
        pop(vm);
        printf(" ");
    }
    printObject(argv[i]);
    pop(vm);
    printf("\n");
    return NEW_BOOL(1);
}

MyMoObject *typefn(MVM *vm, uint argc, MyMoObject *argv[])
{
    UNUSED(vm);
    if (argc != 1)
    {
        runtimeError(vm, "type() only takes 1 argument");
        return NEW_EMPTY;
    }
    char *type = getType(pop(vm));
    return NEW_STRING(vm, type, strlen(type));
}

MyMoObject *globalsfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc)
    {
        runtimeError(vm, "globals() takes no arguments (%d given).", argc);
        return NEW_EMPTY;
    }
    return AS_OBJECT(&vm->globals);
}

MyMoObject *compilefn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: compile() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (!IS_STRING(argv[0]))
    {
        runtimeError(vm, "TypeError: compile() takes a <object 'str'> argument but got %s.", getType(argv[0]));
        return NEW_EMPTY;
    }
    char *source = STRING_VAL(pop(vm));
    MyMoCode *code = newCode(vm);
    MyMoFunction *function = compile(code->vm, source, "@compile", COMPILE_STRING);
    if (function == NULL)
        return NEW_EMPTY;
    code->function = function;
    return AS_OBJECT(code);
}

MyMoObject *execfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: exec() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (!(IS_CODE(argv[0])))
    {
        runtimeError(vm, "TypeError: exec() takes <object 'code'> got %s.",getType(argv[0]));
        return NEW_EMPTY;
    }
    MyMoCode *code = AS_CODE(pop(vm));
    I_Result result = interpreter(code->vm, code->function);
    if (result == RUNTIME_ERROR)
        return NEW_EMPTY;
    return NEW_BOOL(1);
}

MyMoObject *lenfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: len() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    MyMoObject *obj = pop(vm);
    switch (obj->type)
    {
    case OBJ_STRING:
        return NEW_INT(vm, AS_STRING(obj)->length);
    case OBJ_LIST:
        return NEW_INT(vm, AS_LIST(obj)->values.count);
    case OBJ_TUPLE:
        return NEW_INT(vm, AS_TUPLE(obj)->values.count);
    case OBJ_DICT:
        return NEW_INT(vm, AS_DICT(obj)->count);
    default:
        runtimeError(vm, "TypeError: %s has no len()", getType(obj));
        return NEW_EMPTY;
    }
}

MyMoObject *yieldfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: yeild() takes 0 or 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    MyMoObject *obj = NEW_NIL;
    if (argc)
    {
       obj = pop(vm);
    }
    vm->fiber->state = FIBER_YIELD;
    vm->fiber = vm->fiber->parent;
    return obj;
}

// MyMoObject *awaitfn(MVM *vm, uint argc, MyMoObject *argv[])
// {
//     if (argc > 1)
//     {
//         runtimeError(vm, "TypeError: await() takes 0 or 1 argument (%d given).", argc);
//         return NEW_EMPTY;
//     }
//     if(!vm->fiber->parent){
//         runtimeError(vm, "RuntimeError: await function needs to be called from fiber");
//         return NEW_EMPTY;
//     }
//     MyMoObject *obj = NEW_NIL;
//     if (argc)
//     {
//        obj = pop(vm);
//     }
//     vm->fiber->state = FIBER_YIELD;
//     vm->fiber = vm->fiber->parent;
//     return obj;
// }

MyMoObject *superfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    MyMoFunction *function = vm->fiber->callFrames[vm->fiber->frameCount]->function;
    MyMoObject *klass = function ->klass; 
    switch(argc){
        case 0:{
            if(function->type > FN_METHOD){
                return klass;
            }
            else{
                MyMoClass *superClass = AS_CLASS(klass);
                return superClass->superClasses.objects[superClass->superClasses.count-1];
            }
        }
        case 1:{
            MyMoObject *object = pop(vm);
            if(function->type > FN_METHOD){
                if (!(IS_CLASS(object)))
                {
                    runtimeError(vm, "TypeError: super() expected <object 'class'>  got %s.",getType(object));
                    return NEW_EMPTY;
                }
                MyMoClass *superClass = AS_CLASS(object);
                return superClass->superClasses.count ? superClass->superClasses.objects[superClass->superClasses.count - 1] : klass;
            }
            else{
                if (!(IS_INT(object)))
                {
                    runtimeError(vm, "TypeError: super() expected <object 'int'>  got %s.",getType(object));
                    return NEW_EMPTY;
                }
                MyMoClass *superClass = AS_CLASS(klass);
                if(INT_VAL(object) > (superClass->superClasses.count - 1) || INT_VAL(object) < 0 ){
                    return AS_OBJECT(vm->builtInClasses[OBJ_OBJECT]);
                }
                return superClass->superClasses.objects[superClass->superClasses.count - INT_VAL(object) -1];
            }
        }
        case 2:{
            MyMoObject *object1 = pop(vm);
            MyMoObject *object2 = pop(vm);
            if(!(IS_CLASS(object2))){
                runtimeError(vm, "TypeError: super() expected <object 'class'> as 1st argv got %s.",getType(object2));
                return NEW_EMPTY;
            }
            if(!(IS_INT(object1))){
                runtimeError(vm, "TypeError: super() expected < object 'int'> as 2st argv got %s.",getType(object1));
                return NEW_EMPTY;
            }            
            MyMoClass *superClass = AS_CLASS(object2);
            if(INT_VAL(object1) > (superClass->superClasses.count - 1) || INT_VAL(object1) < 0 ){
                return AS_OBJECT(vm->builtInClasses[OBJ_OBJECT]);
            }
            return superClass->superClasses.count ? superClass->superClasses.objects[superClass->superClasses.count - INT_VAL(object1)-1 ] : klass;
        }
        default:{
            runtimeError(vm, "TypeError: super() takes 0 or 1 or 2 argument (%d given).", argc);
            return NEW_EMPTY;
        }
    }

    return NEW_NIL;
}

void defineBuiltInFunction(MVM *vm, const char *name, BuiltInfunction function)
{
    MyMoObject *fnName = NEW_STRING(vm, name, strlen(name));
    MyMoObject *fn = AS_OBJECT(newBuiltInFunction(vm, AS_STRING(fnName), function, OBJ_BUILTIN_FUNCTION));
    setEntry(vm, &vm->builtins, fnName, fn);
}

void defineBuiltInFunctions(MVM *vm)
{
    defineBuiltInFunction(vm, "clock", clockfn);
    defineBuiltInFunction(vm, "input", inputfn);
    defineBuiltInFunction(vm, "print", printfn);
    defineBuiltInFunction(vm, "type", typefn);
    defineBuiltInFunction(vm, "globals", globalsfn);
    defineBuiltInFunction(vm, "compile", compilefn);
    defineBuiltInFunction(vm, "exec", execfn);
    defineBuiltInFunction(vm, "len", lenfn);
    defineBuiltInFunction(vm, "yield", yieldfn);
    // defineBuiltInFunction(vm,"await",awaitfn);
    defineBuiltInFunction(vm,"super",superfn);
}

MyMoFunction *runFile(MVM *vm, char *path)
{
    // Embedded standard-library modules live in the binary, not on
    // disk. They use a synthetic path of the form `@stdlib:NAME`
    // that pathResolver hands back when no real file is found.
    // Compile directly from the embedded source.
    if (strncmp(path, "@stdlib:", 8) == 0)
    {
        const char *name = path + 8;
        const char *src = stdlib_source_lookup(name);
        if (src == NULL)
        {
            runtimeError(vm, "Module Error: unknown stdlib module '%s'.", name);
            exit(74);
        }
        return compile(vm, src, path, COMPILE_SCRIPT);
    }
    MyMoFunction *function;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        runtimeError(vm, "Module Error: could not open module '%s'.", path);
        exit(74);
    }
    int len = strlen(path);
    if (path[len - 1] == 'c')
    {
        function = functionDeserialize(vm, file);
    }
    else
    {
        fseek(file, 0L, SEEK_END);
        size_t fileSize = ftell(file);
        rewind(file);
        char *buffer = (char *)malloc(fileSize + 1);
        if (buffer == NULL)
        {
            runtimeError(vm, "Not enough memory to read \"%s\".", path);
            exit(74);
        }
        size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
        if (bytesRead < fileSize)
        {
            runtimeError(vm, "Could not read module \"%s\".\n", path);
            exit(74);
        }
        buffer[bytesRead] = '\0';
        function = compile(vm, buffer, path, COMPILE_SCRIPT);
        free(buffer);
    }
    fclose(file);
    return function;
}

char *pathResolver(MVM *vm, char *_path)
{
    #define DELIMITER '/'
    size_t len = strlen(_path);
    char *path = New(char, len + 1);
    if (vm->currentModule != NULL)
    {
        if (IS_DELIMITER(_path[0]))
        {
            strcpy(path, _path);
        }
        else
        {
            uint c = 0;
            path = realloc(path, len + vm->currentModule->path->length + 4);
            memcpy(path, vm->currentModule->path->value, vm->currentModule->path->length);
            path[vm->currentModule->path->length] = '\0';
            char *lastSlash;
        parent:
            lastSlash = strrchr(path, DELIMITER);
            if (lastSlash != NULL)
            {
                lastSlash[1] = '\0';
            }
        current:
            if (_path[c] == '.')
            {
                if (IS_DELIMITER(_path[c + 1]))
                {
                    c += 2;
                    goto current;
                }
                else if (_path[c + 1] == '.' && IS_DELIMITER(_path[c + 2]))
                {
                    c += 3;
                    goto parent;
                }
            }
            int k = len + 1 - c;
            char helper[k];
            int i;
            for (i = 0; i < k; i++)
            {
                helper[i] = _path[c + i];
            }
            helper[i] = '\0';
            strcat(path, helper);
        }
        strcat(path, ".my");
    }
    else
    {
        char actualpath[MAX_PATH];
        char *__path;
        #ifdef _WIN32
            __path =  _fullpath(actualpath, _path, MAX_PATH); 
        #else
            __path = realpath(_path, actualpath);
        #endif
        if (__path)
        {
            size_t _len = strlen(__path);
            path = realloc(path, _len + 1);
            memcpy(path, __path, _len);
            path[_len] = '\0';
        }
        else
        {
            // No file on disk — fall through to the embedded
            // stdlib lookup at the bottom. Used to early-return
            // NULL which made `from "mono" use ...` fail in the
            // REPL (where vm->currentModule is NULL).
            free(path);
            if (stdlib_source_lookup(_path) != NULL)
            {
                size_t nameLen = strlen(_path);
                char *synth = New(char, nameLen + 9);
                memcpy(synth, "@stdlib:", 8);
                memcpy(synth + 8, _path, nameLen);
                synth[nameLen + 8] = '\0';
                return synth;
            }
            return NULL;
        }
    }
    size_t pathLen = strlen(path);
    path[pathLen] = '\0';
    char *cachePath = New(char, pathLen + 2);
    memcpy(cachePath, path, pathLen);
    cachePath[pathLen] = '\0';
    strcat(cachePath, "c");
    Stat file;
    Stat cachefile;
    int havePath = (stat(path, &file) == 0);
    int haveCache = (stat(cachePath, &cachefile) == 0);
    if (havePath && haveCache)
    {
        if (cachefile.st_mtime < file.st_mtime)
        {
            free(cachePath);
            return path;
        }
        free(path);
        return cachePath;
    }
    if (havePath)
    {
        free(cachePath);
        return path;
    }
    if (haveCache)
    {
        free(path);
        return cachePath;
    }
    // Neither source nor cache exists — check the embedded stdlib
    // table before giving up. If the name matches, return a
    // synthetic @stdlib: path that runFile recognizes; this lets
    // built-in MyMo-source modules (e.g. `mono`) be imported with
    // the same `from "name" use ...` syntax as user files.
    free(cachePath);
    free(path);
    if (stdlib_source_lookup(_path) != NULL)
    {
        size_t nameLen = strlen(_path);
        char *synth = New(char, nameLen + 9);
        memcpy(synth, "@stdlib:", 8);
        memcpy(synth + 8, _path, nameLen);
        synth[nameLen + 8] = '\0';
        return synth;
    }
    return NULL;
    #undef DELIMITER
}