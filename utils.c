#include "repr.h"
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

Value clockfn(MVM *vm, uint argc, Value argv[])
{
    UNUSED(vm);
    UNUSED(argv);
    if (argc)
    {
        runtimeError(vm, "TypeError: clock() takes no arguments (%d given).", argc);
        return V_EMPTY_VAL;
    }
    return objectToValue(NEW_DOUBLE(vm, (double)clock() / CLOCKS_PER_SEC));
}


Value inputfn(MVM *vm, uint argc, Value argv[])
{
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: input() takes 1 argument (%d given).", argc);
        return V_EMPTY_VAL;
    }
    if (argc != 0)
    {
        Value prompt = argv[0];
        if (!V_IS_OBJ_TYPE(prompt, OBJ_STRING))
        {
            runtimeError(vm, "TypeError: input() only takes a <object 'str'> got %s.", valueTypeName(prompt));
            return V_EMPTY_VAL;
        }
        printf("%s", AS_STRING(V_AS_OBJ(prompt))->value);
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
    return objectToValue(l);
}

Value printfn(MVM *vm, uint argc, Value argv[])
{
    // Format everything first, then pop: a user __str__ runs on the
    // stack above the arguments, which keeps them alive meanwhile.
    StrBuf b;
    strbufInit(&b);
    for (uint i = 0; i < argc; i++)
    {
        if (i)
            strbufAppend(&b, " ", 1);
        if (!formatValue(vm, &b, argv[i], false))
        {
            strbufFree(&b);
            return V_EMPTY_VAL;
        }
    }
    strbufAppend(&b, "\n", 1);
    fwrite(b.data, 1, (size_t)b.length, stdout);
    strbufFree(&b);
    for (uint i = 0; i < argc; i++)
        popV(vm);
    return V_NIL_VAL;
}

Value reprfn(MVM *vm, uint argc, Value argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: repr() takes 1 argument (%u given)", argc);
        return V_EMPTY_VAL;
    }
    Value out = valueToRepr(vm, argv[0]);
    if (!V_IS_EMPTY(out))
        popV(vm);
    return out;
}

Value typefn(MVM *vm, uint argc, Value argv[])
{
    UNUSED(vm);
    if (argc != 1)
    {
        runtimeError(vm, "type() only takes 1 argument");
        return V_EMPTY_VAL;
    }
    char *type = getType(pop(vm));
    return objectToValue(NEW_STRING(vm, type, strlen(type)));
}

Value globalsfn(MVM *vm, uint argc, Value argv[])
{
    if (argc)
    {
        runtimeError(vm, "globals() takes no arguments (%d given).", argc);
        return V_EMPTY_VAL;
    }
    return objectToValue(AS_OBJECT(&vm->globals));
}

Value compilefn(MVM *vm, uint argc, Value argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: compile() takes 1 argument (%d given).", argc);
        return V_EMPTY_VAL;
    }
    if (!V_IS_OBJ_TYPE(argv[0], OBJ_STRING))
    {
        runtimeError(vm, "TypeError: compile() takes a <object 'str'> argument but got %s.", valueTypeName(argv[0]));
        return V_EMPTY_VAL;
    }
    char *source = AS_STRING(V_AS_OBJ(argv[0]))->value;
    popV(vm);
    MyMoCode *code = newCode(vm);
    MyMoFunction *function = compile(code->vm, source, "@compile", COMPILE_STRING);
    if (function == NULL)
        return V_EMPTY_VAL;
    code->function = function;
    return objectToValue(AS_OBJECT(code));
}

Value execfn(MVM *vm, uint argc, Value argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: exec() takes 1 argument (%d given).", argc);
        return V_EMPTY_VAL;
    }
    if (!V_IS_OBJ_TYPE(argv[0], OBJ_CODE))
    {
        runtimeError(vm, "TypeError: exec() takes <object 'code'> got %s.", valueTypeName(argv[0]));
        return V_EMPTY_VAL;
    }
    MyMoCode *code = AS_CODE(V_AS_OBJ(argv[0]));
    popV(vm);
    I_Result result = interpreter(code->vm, code->function);
    if (result == RUNTIME_ERROR)
        return V_EMPTY_VAL;
    return V_BOOL_VAL(1);
}

Value lenfn(MVM *vm, uint argc, Value argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: len() takes 1 argument (%d given).", argc);
        return V_EMPTY_VAL;
    }
    MyMoObject *obj = pop(vm);
    switch (obj->type)
    {
    case OBJ_STRING:
        return objectToValue(NEW_INT(vm, AS_STRING(obj)->length));
    case OBJ_LIST:
        return objectToValue(NEW_INT(vm, AS_LIST(obj)->values.count));
    case OBJ_TUPLE:
        return objectToValue(NEW_INT(vm, AS_TUPLE(obj)->values.count));
    case OBJ_DICT:
        return objectToValue(NEW_INT(vm, AS_DICT(obj)->count));
    default:
        runtimeError(vm, "TypeError: %s has no len()", getType(obj));
        return V_EMPTY_VAL;
    }
}

Value yieldfn(MVM *vm, uint argc, Value argv[])
{
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: yeild() takes 0 or 1 argument (%d given).", argc);
        return V_EMPTY_VAL;
    }
    if (vm->fiber->parent == NULL)
    {
        runtimeError(vm, "RuntimeError: yield() outside a fiber.");
        return V_EMPTY_VAL;
    }
    MyMoObject *obj = NEW_NIL;
    if (argc)
    {
       obj = pop(vm);
    }
    vm->fiber->state = FIBER_YIELD;
    vm->fiber = vm->fiber->parent;
    return objectToValue(obj);
}

// Value awaitfn(MVM *vm, uint argc, Value argv[])
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

Value superfn(MVM *vm, uint argc, Value argv[])
{
    MyMoFunction *function = vm->fiber->callFrames[vm->fiber->frameCount]->function;
    MyMoObject *klass = function ->klass; 
    switch(argc){
        case 0:{
            if(function->type > FN_METHOD){
                return objectToValue(klass);
            }
            else if (klass == NULL || !IS_CLASS(klass)){
                runtimeError(vm, "RuntimeError: super() used outside a class method.");
                return V_EMPTY_VAL;
            }
            else{
                // Inside a method: return a proxy that binds the parent's
                // methods to this method's `self` (argv[0]), so
                // `super().__init__(x)` / `super().name()` just work.
                // `klass` is the class that defined the running method, so
                // inherited methods resolve against *their* parent.
                MyMoClass *defining = AS_CLASS(klass);
                if (defining->superClasses.count == 0 || !IS_CLASS(defining->superClasses.objects[defining->superClasses.count - 1]))
                {
                    runtimeError(vm, "TypeError: super(): class '%s' has no user-defined parent class.", defining->name->value);
                    return V_EMPTY_VAL;
                }
                CallFrame *frame = vm->fiber->callFrames[vm->fiber->frameCount];
                MyMoObject *self = function->argc <= CALLFRAME_ARGS_INLINE
                    ? valueToBoxedObject(vm, frame->args[0])
                    : getEntry(vm, &frame->locals, AS_OBJECT(function->argv[0]));
                MyMoClass *parent = AS_CLASS(defining->superClasses.objects[defining->superClasses.count - 1]);
                return objectToValue(AS_OBJECT(newSuper(vm, self, parent)));
            }
        }
        case 1:{
            MyMoObject *object = pop(vm);
            if(function->type > FN_METHOD){
                if (!(IS_CLASS(object)))
                {
                    runtimeError(vm, "TypeError: super() expected <object 'class'>  got %s.",getType(object));
                    return V_EMPTY_VAL;
                }
                MyMoClass *superClass = AS_CLASS(object);
                return objectToValue(superClass->superClasses.count ? superClass->superClasses.objects[superClass->superClasses.count - 1] : klass);
            }
            else{
                if (!(IS_INT(object)))
                {
                    runtimeError(vm, "TypeError: super() expected <object 'int'>  got %s.",getType(object));
                    return V_EMPTY_VAL;
                }
                MyMoClass *superClass = AS_CLASS(klass);
                if(INT_VAL(object) > (superClass->superClasses.count - 1) || INT_VAL(object) < 0 ){
                    return objectToValue(AS_OBJECT(vm->builtInClasses[OBJ_OBJECT]));
                }
                return objectToValue(superClass->superClasses.objects[superClass->superClasses.count - INT_VAL(object) -1]);
            }
        }
        case 2:{
            MyMoObject *object1 = pop(vm);
            MyMoObject *object2 = pop(vm);
            if(!(IS_CLASS(object2))){
                runtimeError(vm, "TypeError: super() expected <object 'class'> as 1st argv got %s.",getType(object2));
                return V_EMPTY_VAL;
            }
            if(!(IS_INT(object1))){
                runtimeError(vm, "TypeError: super() expected < object 'int'> as 2st argv got %s.",getType(object1));
                return V_EMPTY_VAL;
            }            
            MyMoClass *superClass = AS_CLASS(object2);
            if(INT_VAL(object1) > (superClass->superClasses.count - 1) || INT_VAL(object1) < 0 ){
                return objectToValue(AS_OBJECT(vm->builtInClasses[OBJ_OBJECT]));
            }
            return objectToValue(superClass->superClasses.count ? superClass->superClasses.objects[superClass->superClasses.count - INT_VAL(object1)-1 ] : klass);
        }
        default:{
            runtimeError(vm, "TypeError: super() takes 0 or 1 or 2 argument (%d given).", argc);
            return V_EMPTY_VAL;
        }
    }

    return V_NIL_VAL;
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
    defineBuiltInFunction(vm, "repr", reprfn);
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
    int len = strlen(path);
    if (path[len - 1] == 'c')
    {
        // A bare .myc (shipped without its source): no hash to check.
        function = cacheRead(vm, path, 0);
        if (function == NULL)
        {
            runtimeError(vm, "Module Error: '%s' is not a valid bytecode file for this MyMo version.", path);
            exit(74);
        }
        return function;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        runtimeError(vm, "Module Error: could not open module '%s'.", path);
        exit(74);
    }
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
        // Reuse foo.myc when it was written for exactly this source;
        // otherwise compile and refresh it. MYMO_NOCACHE=1 disables both.
        bool useCache = getenv("MYMO_NOCACHE") == NULL;
        uint64_t hash = cacheHash(buffer, bytesRead);
        char *cachePath = cachePathFor(path);
        function = useCache ? cacheRead(vm, cachePath, hash) : NULL;
        if (function)
            attachSource(function, buffer);
        else
        {
            function = compile(vm, buffer, path, COMPILE_SCRIPT);
            if (function && useCache)
                cacheWrite(function, cachePath, hash);
        }
        free(cachePath);
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
            char helper[k + 1];
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
    // Prefer the source whenever it exists: runFile checks the sibling
    // .myc against the source's hash itself. A lone .myc still runs.
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