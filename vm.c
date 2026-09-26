#include "common.h"
#include "vm.h"
#include "repr.h"
#include "prelude.h"
#include "builtins.h"
#include <ctype.h>
#include "format.h"
#include <limits.h>
#include "include/mymo.h"
#include "stack.h"
#include "operations.h"
#include "datatypes/datatypes.h"
#include "utils.h"
#include "gc.h"
#include "debug.h"
#include "bytecode.h"   // IC_BYTES, IC_TAG_*
#include "modules/modules.h"

#include <math.h>

// #define DEBUG_STACK_TRACE
// #define DEBUG_PRINT_CODE

MVM *initVM()
{
    MVM *vm = New(MVM, 1);
    vm->objects = NULL;
    initGC(vm);
    vm->currentModule = NULL;
    vm->currentClass = NULL;
    vm->objectClass = NULL;
    vm->classCall = 0;
    memset(vm->builtInClasses, 0, sizeof(vm->builtInClasses));
    vm->fiber = newFiber(vm, NULL);
    vm->rootFiber = vm->fiber;
    vm->exitFrame = -1;
    vm->exitFiber = NULL;
    vm->lastError[0] = '\0';
    initValueArray(vm, &vm->hostRoots);
    initDict(&vm->globals);
    initDict(&vm->builtins);
    initDict(&vm->strings);
    initDict(&vm->numbers);
    initDict(&vm->integers);
    initDict(&vm->doubles);
    initDict(&vm->tupleKeys);
    vm->kwargs = NULL;
    vm->quietErrors = 0;
    vm->argsPacked = false;
    initDict(&vm->modules);
    initDict(&vm->builtInModules);
    defineBuiltInClasses(vm);
    defineBuiltInFunctions(vm);
    // Wildcard singleton — used only by case-pattern `_`. isEqual() treats
    // any comparison involving this as a match.
    vm->wildcard = allocateObject(vm, sizeof(MyMoObject), OBJ_WILDCARD);
    // Register all statically-linked built-in modules. Dynamic .so/.dylib
    // modules load lazily via OP_USE → loadBuiltInModule.
    defineBuiltInModules(vm);
    loadPrelude(vm);
    return vm;
}

void freeVM(MVM *vm)
{
    // printObjectarray(vm->objects);
    // resetStack(vm);
    freeDict(vm, &vm->globals);
    freeDict(vm, &vm->builtins);
    freeDict(vm, &vm->strings);
    freeDict(vm, &vm->numbers);
    freeDict(vm, &vm->integers);
    freeDict(vm, &vm->doubles);
    freeDict(vm, &vm->tupleKeys);
    freeDict(vm, &vm->modules);
    freeDict(vm, &vm->builtInModules);
    freeObjects(vm);
    freeRetiredFrames(vm); // after freeObjects: freeFiber retires captured frames
    freeValueArray(vm, &vm->hostRoots);
    free(vm->grayStack);
    free(vm);
}

// Extract the Nth line (1-indexed) of `source` into `out` (max
// out_cap bytes including the terminator). Returns the number of
// bytes written, or 0 if the line doesn't exist. Used to echo the
// offending source line under each traceback frame.
static int extractSourceLine(const char *source, int lineNo, char *out, size_t out_cap)
{
    if (!source || lineNo < 1 || out_cap == 0) return 0;
    const char *p = source;
    int cur = 1;
    while (*p && cur < lineNo)
    {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != lineNo) return 0;
    size_t n = 0;
    while (*p && *p != '\n' && n + 1 < out_cap)
    {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return (int)n;
}

// A try handler that may catch the current error: the fiber's innermost
// one, unless it was installed outside the nested mymo_call we're in.
static bool fiberHasHandler(MVM *vm, MyMoFiber *fiber)
{
    if (fiber->handlerCount == 0)
        return false;
    if (vm->exitFiber == fiber && (int)fiber->handlers[fiber->handlerCount - 1].frameCount <= vm->exitFrame)
        return false;
    return true;
}

static bool hasActiveHandler(MVM *vm)
{
    return fiberHasHandler(vm, vm->fiber);
}

// Will some try handler catch an error raised now? An error that nothing
// in a fiber catches ends that fiber and continues in the fiber that
// resumed it, so look up the chain of resumers (but not past the fiber a
// nested mymo_call runs in).
static bool handlerInChain(MVM *vm)
{
    for (MyMoFiber *fiber = vm->fiber; fiber; fiber = fiber->parent)
    {
        if (fiberHasHandler(vm, fiber))
            return true;
        if (fiber == vm->exitFiber)
            break;
    }
    return false;
}

// The exception object for a runtime error message "Name: text": an
// instance of the built-in exception class Name (RuntimeError when the
// message has no known class prefix) whose `message` is text. Before
// the prelude has loaded, just the message string.
static MyMoObject *errorObject(MVM *vm, const char *msg)
{
    const char *text = msg;
    Value klass = V_NIL_VAL;
    const char *colon = strchr(msg, ':');
    if (colon && colon > msg && colon - msg < 64)
    {
        int len = (int)(colon - msg);
        while (len > 0 && msg[len - 1] == ' ')
            len--;
        bool identifier = len > 0;
        for (int i = 0; i < len; i++)
            if (!isalnum((unsigned char)msg[i]) && msg[i] != '_')
                identifier = false;
        if (identifier && getEntryV(&vm->builtins, AS_OBJECT(newString(vm, msg, len)), &klass) &&
            V_IS_OBJ_TYPE(klass, OBJ_CLASS))
        {
            text = colon + 1;
            while (*text == ' ')
                text++;
        }
        else
            klass = V_NIL_VAL;
    }
    if (valueIsNil(klass))
    {
        // Unprefixed: errors from the I/O modules ("socket.connect(): ...")
        // are OSErrors, the rest RuntimeErrors.
        static const char *ioModules[] = {"socket", "server", "runloop", "nodes", "os", "io", "http", "sqlite", NULL};
        const char *fallback = "RuntimeError";
        for (const char **m = ioModules; *m; m++)
        {
            size_t n = strlen(*m);
            if (strncmp(msg, *m, n) == 0 && (msg[n] == '.' || msg[n] == ':'))
                fallback = "OSError";
        }
        if (!getEntryV(&vm->builtins, AS_OBJECT(newString(vm, fallback, (int)strlen(fallback))), &klass) ||
            !V_IS_OBJ_TYPE(klass, OBJ_CLASS))
            return AS_OBJECT(newString(vm, msg, (int)strlen(msg)));
    }
    MyMoInstance *instance = newInstance(vm, AS_CLASS(V_AS_OBJ(klass)));
    setEntryV(vm, instance->fields, AS_OBJECT(newString(vm, "message", 7)),
              V_OBJ_VAL(AS_OBJECT(newString(vm, text, (int)strlen(text)))));
    return AS_OBJECT(instance);
}

void runtimeError(MVM *vm, const char *format, ...)
{
    // Build the message into a single buffer so we can both stash
    // it on the fiber (for `catch e:` to bind) and print it at the
    // bottom of the traceback. snprintf-into-fixed-buffer cap
    // matches the rest of the error formatting in the VM.
    char msg[512];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    memcpy(vm->lastError, msg, sizeof(msg));

    // If a `try` handler is in flight and OP_RAISE didn't already
    // set a concrete exception value, expose the error message as
    // the catch-bound value. Skip the noisy traceback print in
    // that case — the user is explicitly handling this.
    // Inside a mymo_call whose caller has a `try` further out, the error
    // will propagate to it through the builtin; stash it without printing.
    bool outerHandler = vm->exitFiber == vm->fiber && vm->fiber->handlerCount > 0;
    if (handlerInChain(vm) || outerHandler || vm->quietErrors > 0)
    {
        if (vm->fiber->exception == NULL)
            vm->fiber->exception = errorObject(vm, msg);
        return;
    }

    fprintf(stderr, "Traceback (most recent call last):\n");
    MyMoFiber *fiber = vm->fiber;
    while (fiber != NULL)
    {
        if (fiber->parent != NULL)
        {
            fprintf(stderr, "  while running fiber of %s\n",
                    fiber->callFrames[0]->function->name->value);
        }
        for (u32 i = 0; i <= fiber->frameCount; i++)
        {
            CallFrame *frame = fiber->callFrames[i];
            MyMoFunction *function = frame->function;
            size_t instruction = frame->ip - function->chunk->code - 1;
            int line = (int)function->chunk->lines[instruction];
            int col  = (int)function->chunk->cols[instruction];
            fprintf(stderr, "  [%d : %d] in ", line, col);
            if (function->type == FN_MODULE)
                fprintf(stderr, "<module %s>\n", function->name->value);
            else
                fprintf(stderr, "%s\n", function->name->value);
            // Echo the source line plus a caret under the column.
            // Skipped silently when source isn't available (cache
            // load, REPL synthetic wrapping, etc.).
            char lineBuf[512];
            int n = extractSourceLine(function->chunk->source, line, lineBuf, sizeof(lineBuf));
            if (n > 0)
            {
                int lead = 0;
                while (lead < n && (lineBuf[lead] == ' ' || lineBuf[lead] == '\t'))
                    lead++;
                fprintf(stderr, "      %s\n", lineBuf + lead);
                int caretCol = col - lead;
                if (caretCol < 1) caretCol = 1;
                fprintf(stderr, "      ");
                for (int k = 1; k < caretCol; k++) fputc(' ', stderr);
                fprintf(stderr, "^\n");
            }
        }
        fiber = fiber->parent;
    }
    fputs(msg, stderr);
    fputs("\n", stderr);
}

// Resolve a free variable through a closure's chain of defining frames
// (function->frame, then that frame's function's frame, ...). Each frame
// is checked in its locals dict and then its inline parameter slots —
// params live in frame->args[] (OP_GETARG), not in the locals dict.
static bool lookupEnclosing(MVM *vm, CallFrame *parent, MyMoObject *name, Value *out)
{
    UNUSED(vm);
    MyMoString *key = AS_STRING(name);
    for (; parent; parent = parent->function->frame)
    {
        if (getEntryV(&parent->locals, name, out))
            return true;
        MyMoFunction *fn = parent->function;
        if (fn->argc > CALLFRAME_ARGS_INLINE)
            continue;
        for (int i = 0; i < fn->argc; i++)
        {
            MyMoString *arg = fn->argv[i];
            if (arg->length == key->length && memcmp(arg->value, key->value, key->length) == 0)
            {
                *out = parent->args[i];
                return true;
            }
        }
    }
    return false;
}

// An integer result: inline when it fits in 32 bits, else a heap int.
// Returns the Value so the dispatch loop pushes it on its cached `sp`.
static Value intResult(MVM *vm, long n)
{
    if (n >= INT32_MIN && n <= INT32_MAX)
        return V_INT_VAL((int32_t)n);
    return V_OBJ_VAL(NEW_INT(vm, n));
}

// Arrange the arguments of a call to a function with *rest / **kw
// parameters: the stack ends with its argc positional arguments (self
// included for methods); `extra` holds keyword arguments no named
// parameter took (or NULL). Fills defaults, packs surplus positionals
// into a tuple for *rest and extra keywords into a dict for **kw, leaving
// exactly fn->argc arguments.
static bool packVarargs(MVM *vm, MyMoFunction *fn, int argc, MyMoDict *extra)
{
    int fixed = FIXED_PARAMS(fn);
    const char *name = fn->name ? fn->name->value : "function";
    MyMoTuple *rest = NULL;
    if (argc > fixed)
    {
        if (!(fn->isargs & VARARGS_REST))
        {
            runtimeError(vm, "TypeError: %s() takes %d positional argument%s but %d were given", name, fixed,
                         fixed == 1 ? "" : "s", argc);
            return false;
        }
        Value *base = vm->fiber->stack.values + vm->fiber->stack.count - (argc - fixed);
        rest = newTuple(vm);
        for (int i = 0; i < argc - fixed; i++)
            writeValueArray(vm, &rest->values, base[i]);
        vm->fiber->stack.count -= argc - fixed;
    }
    else if (argc < fixed)
    {
        int firstDefault = fixed - fn->defaultCount;
        if (argc < firstDefault)
        {
            runtimeError(vm, "TypeError: %s() missing required argument '%s'", name, fn->argv[argc]->value);
            return false;
        }
        for (int i = argc; i < fixed; i++)
            pushV(vm, fn->defaults[i - firstDefault]);
    }
    if (fn->isargs & VARARGS_REST)
        pushV(vm, V_OBJ_VAL(AS_OBJECT(rest ? rest : newTuple(vm))));
    if (fn->isargs & VARARGS_KW)
    {
        MyMoDict *kw = newDict(vm);
        if (extra)
            copyDict(vm, extra, kw);
        pushV(vm, V_OBJ_VAL(AS_OBJECT(kw)));
    }
    else if (extra && extra->count > 0)
    {
        runtimeError(vm, "TypeError: %s() got an unexpected keyword argument '%s'", name,
                     AS_STRING(extra->entries[extra->head].key)->value);
        return false;
    }
    return true;
}

bool callFunction(MVM *vm, MyMoFunction *function, int argc, bool calleeSlot)
{
    if (function->isargs)
    {
        if (!vm->argsPacked && !packVarargs(vm, function, argc, NULL))
            return false;
        vm->argsPacked = false;
        argc = function->argc;
    }
    int fill = missingDefaults(function, argc);
    if (fill < 0)
    {
        arityError(vm, function, argc);
        return false;
    }
    for (int i = 0; i < fill; i++)
        pushV(vm, function->defaults[function->defaultCount - fill + i]);
    argc += fill;
    {
        // Off-by-one fix: callFrames is indexed up to and including
        // frameCount (slot 0 is the script/initial frame, slot frameCount
        // is the most recently pushed). We're about to write slot
        // frameCount+1, so we need capacity >= frameCount+2.
        if (STACK_EXHAUSTED(vm->fiber->stack.count))
        {
            runtimeError(vm, "RecursionError: maximum recursion depth exceeded.");
            return false;
        }
        if (vm->fiber->frameCapacity < (uint)(vm->fiber->frameCount + 2))
        {
            u32 capacity = vm->fiber->frameCapacity;
            vm->fiber->frameCapacity = ResizeCapacity(vm->fiber->frameCapacity);
            // Make sure we actually grew enough (ResizeCapacity 0 -> 8,
            // 8 -> 16, etc.); always re-check.
            while (vm->fiber->frameCapacity < (uint)(vm->fiber->frameCount + 2))
                vm->fiber->frameCapacity = ResizeCapacity(vm->fiber->frameCapacity);
            vm->fiber->callFrames = ResizeArray(vm, CallFrame *, vm->fiber->callFrames, capacity, vm->fiber->frameCapacity);
        }
        CallFrame *frame;
        // Pool: reuse a recycled CallFrame if one is available, else malloc.
        // Frame pool grows monotonically — frames are returned to the pool
        // on OP_FRET, never freed individually. Pool drains in freeFiber.
        // Recycled frames already have their locals dict in initial state
        // (freeDict zeroes it on the OP_FRET path), so we skip initDict.
        if (vm->fiber->freeFramesHead != NULL)
        {
            frame = vm->fiber->freeFramesHead;
            // function field doubles as the free-list "next" link while
            // the frame is parked on the pool.
            vm->fiber->freeFramesHead = (CallFrame *)frame->function;
        }
        else
        {
            frame = New(CallFrame, 1);
            initDict(&frame->locals);
            frame->gcEpoch = 0;
            frame->nextRetired = NULL;
        }
        frame->function = function;
        frame->captured = false;
        frame->calleeSlot = calleeSlot;
        frame->ip = function->chunk->code;
        vm->fiber->callFrames[++vm->fiber->frameCount] = frame;
        if (argc)
        {
            // Fast path: small-arg-count functions (≤ CALLFRAME_ARGS_INLINE)
            // store args directly into frame->args[] for slot-indexed access
            // via OP_GETARG / OP_SETARG. Saves the dict insert/lookup per
            // arg reference inside the body — the dominant cost in a tight
            // recursion like fib.
            //
            // Args are popped in reverse from the operand stack (last
            // argument is on top), so we fill args[argc-1..0].
            if (argc <= CALLFRAME_ARGS_INLINE)
            {
                for (int i = argc - 1; i >= 0; i--)
                {
                    frame->args[i] = popV(vm);
                }
            }
            else
            {
                // Spill to dict for >8 args (rare — unsupported by current
                // function decls anyway; argv is sized 256 but the compiler
                // caps at 255, so effectively always ≤ inline cap today).
                for (int i = argc - 1; i >= 0; i--)
                {
                    setEntry(vm, &frame->locals, AS_OBJECT(function->argv[i]), pop(vm));
                }
            }
        }
        return true;
    }
}

static bool callerEx(MVM *vm, MyMoObject *callee, u32 argc, bool calleeSlot);

// Call `callee` with the stack laid out as [callee, args...].
bool caller(MVM *vm, MyMoObject *callee, u32 argc)
{
    return callerEx(vm, callee, argc, true);
}

// calleeSlot=false: the slot below the args was reused as the first
// argument (self), so there is no separate callee value to pop on return.
static bool callerEx(MVM *vm, MyMoObject *callee, u32 argc, bool calleeSlot)
{
    switch (callee->type)
    {
    case OBJ_BUILTIN_METHOD:
    case OBJ_BUILTIN_FUNCTION:
    {
        BuiltInfunction function = AS_BUILTIN_FUNCTION(callee)->function;
        // Built-ins still take MyMoObject** (their signature migrates in a
        // later step). All values on the stack are heap objects today, so
        // unwrap each Value via V_AS_OBJ into a temporary argv.
        // A copy, not a pointer into the stack: a builtin that calls back
        // into MyMo (mymo_call) pushes over these slots.
        Value argCopy[256];
        Value *vargs = vm->fiber->stack.values + vm->fiber->stack.count - argc;
        memcpy(argCopy, vargs, sizeof(Value) * argc);
        Value result = function(vm, argc, argCopy);
        if (V_IS_EMPTY(result))
            return false;
        pop(vm);
        pushV(vm, result);
        return true;
    }
    case OBJ_FUNCTION:
    {

        MyMoFunction *function = AS_FUNCTION(callee);
        if (function->type == FN_SCRIPT)
        {
            runtimeError(vm, "TypeError: <Script '%s'> is not callable.", function->name->value);
            return false;
        }
        // Functions containing `yield` may be called directly: yielding
        // then suspends the enclosing fiber (and errors outside one).
        else if (function->type == FN_MODULE)
        {
            runtimeError(vm, "TypeError: <module '%s'> is not callable.", function->name->value);
            return false;
        }
        return callFunction(vm, function, argc, calleeSlot);
    }
    case OBJ_CLASS:
    {
        MyMoObject *newMethed = NEW_STRING(vm, "__new__", 7);
        MyMoClass *klass = AS_CLASS(callee);
        MyMoObject *__new__ = getEntry(vm, vm->builtInClasses[OBJ_OBJECT]->methods, newMethed);
        BuiltInfunction function = AS_BUILTIN_FUNCTION(__new__)->function;
        push(vm, callee);
        // __new__ always returns a fresh instance object.
        Value calleeArg = V_OBJ_VAL(callee);
        Value resultV = function(vm, 1, &calleeArg);
        if (V_IS_EMPTY(resultV))
            return false;
        MyMoObject *result = V_AS_OBJ(resultV);
        if (klass->init)
        {
            vm->classCall++;
            vm->fiber->stack.values[vm->fiber->stack.count - argc - 1] = V_OBJ_VAL(result);
            return callerEx(vm, klass->init, argc + 1, false); // slot holds the new instance
        }
        else
        {
            if (argc)
            {
                runtimeError(vm, "TypeError: %s() Takes 0 arguments but got %d.", AS_CLASS(callee)->name->value, argc);
                return false;
            }
            pop(vm);
            push(vm, result);
            return true;
        }
    }
    case OBJ_BUILTIN_CLASS:
    {
        MyMoBuiltInClass *klass = AS_BUILTIN_CLASS(callee);
        MyMoObject *newMethed = NEW_STRING(vm, "__new__", 7);
        MyMoObject *__new__ = getEntry(vm, klass->methods, newMethed);
        BuiltInfunction function = AS_BUILTIN_FUNCTION(__new__)->function;
        // A copy, not a pointer into the stack: a builtin that calls back
        // into MyMo (mymo_call) pushes over these slots.
        Value argCopy[256];
        Value *vargs = vm->fiber->stack.values + vm->fiber->stack.count - argc;
        memcpy(argCopy, vargs, sizeof(Value) * argc);
        Value result = function(vm, argc, argCopy);
        if (V_IS_EMPTY(result))
            return false;
        pop(vm);
        pushV(vm, result);
        return true;
    }
    case OBJ_BOUND_METHOD:
    {
        MyMoBoundMethod *bound = AS_BOUND_METHOD(callee);
        if (bound->method->type > FN_METHOD)
        {
            // A plain function bound as a method (e.g. a decorator's
            // wrapper): its OP_FRET pops a callee slot, so keep that slot
            // and insert `self` as the first argument after it.
            Value *first = vm->fiber->stack.values + vm->fiber->stack.count - argc;
            memmove(first + 1, first, sizeof(Value) * argc);
            *first = V_OBJ_VAL(bound->self);
            vm->fiber->stack.count++;
            return callerEx(vm, AS_OBJECT(bound->method), argc + 1, true);
        }
        // Real methods consume the callee slot as `self`.
        vm->fiber->stack.values[vm->fiber->stack.count - argc - 1] = V_OBJ_VAL(bound->self);
        return callerEx(vm, AS_OBJECT(bound->method), argc + 1, false);
    }
    case OBJ_INSTANCE:
    {
        // obj(args) calls obj.__call__(args); the callee slot already
        // holds obj, which becomes `self`.
        MyMoObject *method = getMethod(vm, callee, "__call__");
        if (!IS_EMPTY(method) && method->type == OBJ_FUNCTION)
            return callerEx(vm, method, argc + 1, false);
        runtimeError(vm, "TypeError: %s object is not callable (no __call__)", AS_INSTANCE(callee)->klass->name->value);
        return false;
    }
    default:
        runtimeError(vm, "TypeError: %s is not callable", getType(callee));
        return false;
    }
}

// Keyword arguments. The stack holds [callee, positional..., keyword
// values...]. For a MyMo function (or a method, or a class's __init__)
// the keyword values move into their parameter positions, missing ones
// take their defaults, and unknown/duplicate/missing names are errors;
// *argc becomes the full parameter count. Builtins get the keywords as
// a dict in *kwargs (see takeKeyword) and only the positional arguments
// stay on the stack.
static bool bindKeywords(MVM *vm, Value calleeV, u32 *argc, int kwc, MyMoString **names, MyMoDict **kwargs)
{
    *kwargs = NULL;
    MyMoObject *callee = V_IS_OBJ(calleeV) ? V_AS_OBJ(calleeV) : NULL;
    MyMoFunction *fn = NULL;
    int offset = 0; // 1 when the first parameter is `self`, passed implicitly
    if (callee)
    {
        switch (callee->type)
        {
        case OBJ_FUNCTION:
            fn = AS_FUNCTION(callee);
            break;
        case OBJ_BOUND_METHOD:
            fn = AS_BOUND_METHOD(callee)->method;
            offset = 1;
            break;
        case OBJ_CLASS:
            if (AS_CLASS(callee)->init)
            {
                fn = AS_FUNCTION(AS_CLASS(callee)->init);
                offset = 1;
            }
            break;
        default:
            break;
        }
    }
    Value *base = vm->fiber->stack.values + vm->fiber->stack.count - *argc;
    int positional = (int)*argc - kwc;
    if (fn == NULL)
    {
        if (callee && (callee->type == OBJ_BUILTIN_FUNCTION || callee->type == OBJ_BUILTIN_METHOD ||
                       callee->type == OBJ_BUILTIN_CLASS))
        {
            MyMoDict *dict = newDict(vm);
            for (int i = 0; i < kwc; i++)
                setEntryV(vm, dict, AS_OBJECT(names[i]), base[positional + i]);
            vm->fiber->stack.count -= kwc;
            *argc = (u32)positional;
            *kwargs = dict;
            return true;
        }
        runtimeError(vm, "TypeError: %s doesn't take keyword arguments", valueTypeName(calleeV));
        return false;
    }
    const char *fname = fn->name ? fn->name->value : "function";
    int nparams = FIXED_PARAMS(fn) - offset; // named parameters we fill here
    MyMoTuple *rest = NULL;
    MyMoDict *extra = NULL;
    if (positional > nparams && (fn->isargs & VARARGS_REST))
    {
        rest = newTuple(vm);
        for (int i = nparams; i < positional; i++)
            writeValueArray(vm, &rest->values, base[i]);
        pushV(vm, V_OBJ_VAL(AS_OBJECT(rest))); // rooted while we allocate
        positional = nparams;
    }
    if (positional > nparams)
    {
        runtimeError(vm, "TypeError: %s() takes %d positional argument%s but %d were given", fname, nparams,
                     nparams == 1 ? "" : "s", positional);
        return false;
    }
    Value slots[256];
    for (int i = 0; i < nparams; i++)
        slots[i] = i < positional ? base[i] : V_EMPTY_VAL;
    for (int k = 0; k < kwc; k++)
    {
        int p = -1;
        for (int i = offset; i < nparams + offset && p < 0; i++)
            if (fn->argv[i] && fn->argv[i]->length == names[k]->length &&
                memcmp(fn->argv[i]->value, names[k]->value, (size_t)names[k]->length) == 0)
                p = i - offset;
        if (p < 0 && (fn->isargs & VARARGS_KW))
        {
            if (!extra)
            {
                extra = newDict(vm);
                pushV(vm, V_OBJ_VAL(AS_OBJECT(extra))); // rooted while we allocate
            }
            setEntryV(vm, extra, AS_OBJECT(names[k]), base[positional + (int)(rest ? rest->values.count : 0) + k]);
            continue;
        }
        if (p < 0)
        {
            runtimeError(vm, "TypeError: %s() got an unexpected keyword argument '%s'", fname, names[k]->value);
            return false;
        }
        if (!V_IS_EMPTY(slots[p]))
        {
            runtimeError(vm, "TypeError: %s() got multiple values for argument '%s'", fname, names[k]->value);
            return false;
        }
        slots[p] = base[positional + (int)(rest ? rest->values.count : 0) + k];
    }
    int firstDefault = FIXED_PARAMS(fn) - fn->defaultCount;
    for (int i = 0; i < nparams; i++)
    {
        if (!V_IS_EMPTY(slots[i]))
            continue;
        int param = i + offset;
        if (param < firstDefault)
        {
            runtimeError(vm, "TypeError: %s() missing required argument '%s'", fname, fn->argv[param]->value);
            return false;
        }
        slots[i] = fn->defaults[param - firstDefault];
    }
    memcpy(base, slots, sizeof(Value) * (size_t)nparams);
    int count = nparams;
    if (fn->isargs & VARARGS_REST)
        base[count++] = V_OBJ_VAL(AS_OBJECT(rest ? rest : newTuple(vm)));
    if (fn->isargs & VARARGS_KW)
        base[count++] = V_OBJ_VAL(AS_OBJECT(extra ? extra : newDict(vm)));
    vm->fiber->stack.count = (int)(base - vm->fiber->stack.values) + count;
    *argc = (u32)count;
    vm->argsPacked = fn->isargs != 0;
    return true;
}

// `catch types` against a raised value: types is an exception class or a
// tuple of them. Instances match by class (with inheritance); a raised
// string matches Exception, or the class its "Name:" prefix names.
static bool exceptionMatches(MVM *vm, Value exc, Value types, bool *out)
{
    if (V_IS_OBJ_TYPE(types, OBJ_TUPLE))
    {
        ValueArray *options = &AS_TUPLE(V_AS_OBJ(types))->values;
        for (int i = 0; i < options->count; i++)
        {
            if (!exceptionMatches(vm, exc, options->values[i], out))
                return false;
            if (*out)
                return true;
        }
        *out = false;
        return true;
    }
    if (!V_IS_OBJ_TYPE(types, OBJ_CLASS))
    {
        runtimeError(vm, "TypeError: catch expects an exception class or a tuple of them, not %s", valueTypeName(types));
        return false;
    }
    MyMoClass *klass = AS_CLASS(V_AS_OBJ(types));
    if (V_IS_OBJ_TYPE(exc, OBJ_STRING))
    {
        MyMoString *s = AS_STRING(V_AS_OBJ(exc));
        int n = klass->name->length;
        *out = strcmp(klass->name->value, "Exception") == 0 ||
               (s->length >= n && memcmp(s->value, klass->name->value, (size_t)n) == 0 &&
                (s->length == n || s->value[n] == ':'));
        return true;
    }
    *out = isInstanceOf(vm, exc, types);
    return true;
}

static bool bindKeywords(MVM *vm, Value calleeV, u32 *argc, int kwc, MyMoString **names, MyMoDict **kwargs);

// Call with keyword arguments; the stack is [callee, args...] with the
// last kwc arguments named by `names`. The dispatch loop SAVEs before
// and LOADs after, like caller().
static bool callWithKeywords(MVM *vm, Value calleeV, u32 argc, int kwc, MyMoString **names)
{
    MyMoDict *kwargs;
    if (!bindKeywords(vm, calleeV, &argc, kwc, names, &kwargs))
        return false;
    MyMoDict *outer = vm->kwargs;
    vm->kwargs = kwargs;
    bool ok = caller(vm, V_AS_OBJ(calleeV), argc);
    if (ok && kwargs && kwargs->count > 0)
    {
        MyMoObject *c = V_AS_OBJ(calleeV);
        const char *fname = c->type == OBJ_BUILTIN_CLASS ? AS_BUILTIN_CLASS(c)->name->value
                                                         : AS_BUILTIN_FUNCTION(c)->name->value;
        runtimeError(vm, "TypeError: %s() got an unexpected keyword argument '%s'", fname,
                     AS_STRING(kwargs->entries[kwargs->head].key)->value);
        ok = false;
    }
    vm->kwargs = outer;
    return ok;
}

// Python-style operator method names and the operator each one defines.
static const char *DUNDER_OPERATORS[][2] = {
    {"__add__", "+"}, {"__sub__", "-"}, {"__mul__", "*"}, {"__truediv__", "/"}, {"__div__", "/"},
    {"__floordiv__", "//"}, {"__mod__", "%"}, {"__pow__", "**"}, {"__eq__", "=="}, {"__ne__", "!="},
    {"__lt__", "<"}, {"__gt__", ">"}, {"__le__", "<="}, {"__ge__", ">="}, {"__and__", "&"},
    {"__or__", "|"}, {"__xor__", "^"}, {"__lshift__", "<<"}, {"__rshift__", ">>"}, {"__neg__", "-@"},
    {"__pos__", "+@"}, {"__iadd__", "+="}, {"__isub__", "-="}, {"__imul__", "*="}, {"__itruediv__", "/="},
    {"__ifloordiv__", "//="}, {"__imod__", "%="}, {"__ipow__", "**="}, {"__iand__", "&="},
    {"__ior__", "|="}, {"__ixor__", "^="}, {"__ilshift__", "<<="}, {"__irshift__", ">>="}, {NULL, NULL}};

// Store a method on a class. A Python-style operator name (__add__) also
// defines the operator it stands for (+), which is what the VM looks up.
static void installMethod(MVM *vm, MyMoClass *klass, MyMoObject *name, Value method)
{
    setEntryV(vm, klass->methods, name, method);
    MyMoString *n = AS_STRING(name);
    if (n->length < 5 || n->value[0] != '_' || n->value[1] != '_')
        return;
    for (int i = 0; DUNDER_OPERATORS[i][0]; i++)
        if (strcmp(n->value, DUNDER_OPERATORS[i][0]) == 0)
        {
            const char *op = DUNDER_OPERATORS[i][1];
            setEntryV(vm, klass->methods, AS_OBJECT(newString(vm, op, (int)strlen(op))), method);
            return;
        }
}

// The method for operator `op` on an instance. An in-place operator
// (+=) without its own method uses the plain one (+).
static MyMoObject *operatorMethod(MVM *vm, MyMoObject *instance, const char *op)
{
    MyMoObject *method = getMethod(vm, instance, op);
    size_t len = strlen(op);
    if (IS_EMPTY(method) && len >= 2 && op[len - 1] == '=' && strcmp(op, "==") != 0 && strcmp(op, "!=") != 0 &&
        strcmp(op, "<=") != 0 && strcmp(op, ">=") != 0)
    {
        char plain[8];
        snprintf(plain, sizeof(plain), "%.*s", (int)len - 1, op);
        method = getMethod(vm, instance, plain);
    }
    return method;
}

// Integer-only binary operators (& | ^ << >>) on Values.
#define BitwiseOp(a, b, op)                                                 \
    do                                                                      \
    {                                                                       \
        if (!valueLooksLikeInt(a) || !valueLooksLikeInt(b))                 \
        {                                                                   \
            SAVE();                                                         \
            runtimeError(vm, "TypeError: Operands must be integers.");     \
            goto _runtime_error;                                            \
        }                                                                   \
        pushV(vm, intResult(vm, valueToLong(a) op valueToLong(b)));         \
    } while (0);

// Unary + / - on a numeric Value.
#define UnaryOp(op, a)                                                      \
    do                                                                      \
    {                                                                       \
        if (valueLooksLikeInt(a))                                           \
            pushV(vm, intResult(vm, op valueToLong(a)));                    \
        else if (valueLooksLikeDouble(a))                                   \
            pushV(vm, V_DOUBLE_VAL(op valueToDouble(a)));                   \
        else                                                                \
        {                                                                   \
            SAVE();                                                         \
            runtimeError(vm, "TypeError: operand must be a number");                  \
            goto _runtime_error;                                            \
        }                                                                   \
    } while (0);

// Call an instance's operator method: `a` is the instance Value, `b` the
// right operand (V_EMPTY_VAL for unary operators). Stack: [method, a, b].
#define OperatorOverLoad(a, b, op)                                                          \
    do                                                                                      \
    {                                                                                       \
        MyMoObject *method = operatorMethod(vm, V_AS_OBJ(a), op);                           \
        if (IS_EMPTY(method))                                                               \
        {                                                                                   \
            SAVE();                                                                         \
            runtimeError(vm, "AttributeError: %s has no method %s", valueTypeName(a), op); \
            goto _runtime_error;                                                            \
        }                                                                                   \
        pushV(vm, V_OBJ_VAL(method));                                                       \
        pushV(vm, a);                                                                       \
        if (!V_IS_EMPTY(b))                                                                 \
            pushV(vm, b);                                                                   \
        SAVE();                                                                             \
        if (!caller(vm, method, V_IS_EMPTY(b) ? 1 : 2))                                     \
        {                                                                                   \
            goto _runtime_error;                                                            \
        }                                                                                   \
        LOAD();                                                                             \
        DISPATCH();                                                                         \
    } while (0)

bool isFalsey(MyMoObject *obj)
{
    return valueIsFalsey(objectToValue(obj));
}

// Value-native truthiness. Handles inline ints, doubles, nil/true/
// false directly; boxed objects fall through to the legacy heap-
// object check above.
static inline bool isFalseyV(Value v)
{
    if (V_IS_NIL(v))    return true;
    if (V_IS_TRUE(v))   return false;
    if (V_IS_FALSE(v))  return true;
    if (V_IS_INT(v))    return V_AS_INT(v) == 0;
    if (V_IS_DOUBLE(v)) return V_AS_DOUBLE(v) == 0.0;
    if (V_IS_OBJ(v))    return isFalsey(V_AS_OBJ(v));
    return true;
}

int runMVM(MVM *vm)
{
    int agp = 0;
    register CallFrame *frame = vm->fiber->callFrames[vm->fiber->frameCount];
    // Phase 4: hoist `ip` and stack-top pointer into local registers. The
    // global `vm->fiber->stack.count` and `frame->ip` lag behind these
    // locals; SAVE() syncs them out before any external call that may
    // inspect them, LOAD() pulls them back. Idiomatic Lua-style.
    register u8 *ip = frame->ip;
    register Value *sp = vm->fiber->stack.values + vm->fiber->stack.count;
    // High bytes set by OP_WIDE for the next instruction's constant
    // operands: bits 8-15 for the first read, 24-31 for the second.
    // Zero except right after an OP_WIDE, so normal reads are unchanged.
    u32 wide = 0, constIndex;
#include "dispatch.h"
#define ReadByte()     (*ip++)
#define ReadShort()    (ip += 2, (u16)(ip[-2] << 8) | ip[-1])
#define ReadConstIndex() (constIndex = (u32)ReadByte() | (wide & 0xff00u), wide >>= 16, constIndex)
#define ReadConstant() (frame->function->chunk->constants.values[ReadConstIndex()])
#define ReadObject()   (V_AS_OBJ(ReadConstant()))

// Stack ops on the cached register `sp`. Hot paths use these.
// `(n)` is cast to int because callers commonly pass u32/size_t/u8 — the
// expression `-1 - (u32)n` would otherwise unsigned-wrap to a huge index.
#define lpush(v)       (*sp++ = (v))
#define lpushObj(o)    (*sp++ = V_OBJ_VAL(o))
#define lpop()         (*--sp)
#define lpeek(n)       (sp[-1 - (int)(n)])

// Sync register state out to fiber/frame before calling helpers that
// inspect them (caller, runtimeError, operations.c, getEntry/setEntry,
// printStack, etc). LOAD pulls back afterwards. The frame pointer can
// also change across CALL/RET, so LOAD re-reads it.
#define SAVE() do { vm->fiber->stack.count = (int)(sp - vm->fiber->stack.values); frame->ip = ip; } while (0)
#define LOAD() do { frame = vm->fiber->callFrames[vm->fiber->frameCount]; ip = frame->ip; sp = vm->fiber->stack.values + vm->fiber->stack.count; } while (0)

// GC safe point (see gc.c). Placed only where every live value is on a
// fiber stack, in a frame or in a VM root — loop back-edges and call
// entry — and skipped in nested runMVM calls, whose C callers may hold
// objects in locals.
#define GC_SAFEPOINT() do { if (vm->objectCount >= vm->nextGC && vm->runDepth == 1) { SAVE(); collectGarbage(vm); LOAD(); } } while (0)

// runtimeError reads `frame->ip` to compute the source line, but
// inside the dispatch loop `ip` is held in a register and only
// flushed back on SAVE(). Without this every error would report
// the location of the *last* SAVE — usually a stale offset
// somewhere upstream of the actual error.
//
// Shadow the public `runtimeError` symbol with a macro for the
// duration of runMVM so every existing call site auto-SAVEs first.
// The `(runtimeError)` parens disable macro expansion on the call
// inside, so we recurse into the real function.
#define runtimeError(vm_, ...) do { SAVE(); (runtimeError)(vm_, __VA_ARGS__); } while (0)

// Legacy push/pop/peek calls inside dispatch handlers must use the local
// `sp`; redefine them as macros that override the global function names.
// The originals in stack.c remain used outside runMVM.
// Legacy object push: normalizes heap nil/bool singletons to inline Values
// (objectToValue), so they never persist on the stack.
#define push(vm_, obj)  (lpush(objectToValue(obj)))
#define pop(vm_)        valueToBoxedObject((vm_), lpop())
#define peek(vm_, n)    valueToBoxedObject((vm_), lpeek(n))
#define pushV(vm_, v)   (lpush(v))
#define popV(vm_)       (lpop())
#define peekV(vm_, n)   (lpeek(n))

#ifdef DEBUG_STACK_TRACE

#define DISPATCH()                                                                                       \
    do                                                                                                   \
    {                                                                                                    \
        SAVE();                                                                                          \
        printStack(vm);                                                                                  \
        disassembleInstruction(frame->function->chunk, (int)(ip - frame->function->chunk->code));        \
        goto *dispatchTable[ReadByte()];                                                                 \
    } while (0);

#else

#define DISPATCH() goto *dispatchTable[ReadByte()]

#endif

    for (;;)
    {
    OP_NOP:
    {
        DISPATCH();
    }
    OP_TRY:
    {
        // Push a handler. The next 2 bytes are a u16 forward offset
        // from the byte after the offset to the catch arm — same
        // shape as OP_JMP / OP_JIF.
        u16 offset = ReadShort();
        if (vm->fiber->handlerCount >= 32)
        {
            runtimeError(vm, "RuntimeError: try-block nesting too deep (max 32)");
            goto _runtime_error;
        }
        TryHandler *h = &vm->fiber->handlers[vm->fiber->handlerCount++];
        h->handlerIp  = ip + offset;
        h->frameCount = vm->fiber->frameCount;
        // SAVE the current sp so unwinding can throw away any
        // partial stack the try-body had pushed when it errored.
        h->stackCount = (int)(sp - vm->fiber->stack.values);
        DISPATCH();
    }
    OP_ENDTRY:
    {
        // Pop the topmost handler — normal exit from the try-body
        // with no error fired.
        if (vm->fiber->handlerCount > 0) vm->fiber->handlerCount--;
        DISPATCH();
    }
    OP_IN:
    {
        // [item, container] -> [bool]; instances use __contains__.
        Value container = popV(vm);
        Value item = popV(vm);
        if (V_IS_OBJ_TYPE(container, OBJ_INSTANCE))
            OperatorOverLoad(container, item, "__contains__");
        SAVE();
        Value result = containsValue(vm, container, item);
        if (V_IS_EMPTY(result))
            goto _runtime_error;
        pushV(vm, result);
        DISPATCH();
    }
    OP_METV:
    {
        // Install a decorated method: [class, value, original] -> [class].
        // Like OP_MET, but the method is whatever the decorators returned.
        // The undecorated function still belongs to the class (super()
        // inside it resolves through its klass).
        MyMoObject *name = ReadObject();
        MyMoFunction *original = AS_FUNCTION(V_AS_OBJ(lpop()));
        Value value = lpop();
        MyMoClass *klass = AS_CLASS(V_AS_OBJ(lpeek(0)));
        original->klass = AS_OBJECT(klass);
        if (V_IS_OBJ(value) && IS_FUNCTION(V_AS_OBJ(value)))
        {
            MyMoFunction *method = AS_FUNCTION(V_AS_OBJ(value));
            if (AS_STRING(name)->length == 8 && memcmp(AS_STRING(name)->value, "__init__", 8) == 0)
                klass->init = AS_OBJECT(method);
            method->klass = AS_OBJECT(klass);
        }
        installMethod(vm, klass, name, value);
        DISPATCH();
    }
    OP_DEFAULTS:
    {
        // [d1..dn, fn] -> [fn]: replace fn's defaults with d1..dn.
        u8 n = ReadByte();
        MyMoFunction *function = AS_FUNCTION(V_AS_OBJ(lpeek(0)));
        free(function->defaults);
        function->defaults = malloc(sizeof(Value) * n);
        memcpy(function->defaults, sp - 1 - n, sizeof(Value) * n);
        function->defaultCount = n;
        sp[-1 - n] = sp[-1];
        sp -= n;
        DISPATCH();
    }
    OP_WIDE:
    {
        wide = ((u32)ip[0] << 8) | ((u32)ip[1] << 24);
        ip += 2;
        DISPATCH();
    }
    OP_RAISE:
    raise_value:
    {
        // Pop the raised value, wrap it as a string error message
        // for now (typed exceptions land later), and trigger the
        // unwind path. We sync ip first so runtimeError reports the
        // correct line for any traceback printed below.
        Value raised = lpeek(0);
        SAVE();
        if (V_IS_OBJ_TYPE(raised, OBJ_CLASS))
        {
            // `raise ValueError` raises ValueError().
            if (mymo_call(vm, raised, 0, NULL, &raised) != MYMO_OK)
                goto _runtime_error;
            sp[-1] = raised;
        }
        // The message printed if nothing catches it: the value's str().
        Value text = valueToStr(vm, raised);
        const char *message = V_IS_EMPTY(text) ? "RaiseError" : AS_STRING(V_AS_OBJ(text))->value;
        lpop();
        SAVE();
        // Stash the raw object on the fiber so a `catch e:` binds
        // exactly what was raised.
        vm->fiber->exception = valueToBoxedObject(vm, raised);
        runtimeError(vm, "%s", message);
        goto _runtime_error;
    }
    OP_CONST:
    {
        // First adopter of the Value-native push — ReadConstant returns a
        // Value (still always a wrapped object pointer at this stage).
        pushV(vm, ReadConstant());
        DISPATCH();
    }
    OP_NIL:
    {
        // Inline NaN-boxed singleton. Legacy consumers that pop a
        // MyMoObject* go through valueToBoxedObject, which maps the
        // inline tag back to the existing NilObject heap singleton.
        pushV(vm, V_NIL_VAL);
        DISPATCH();
    }
    OP_TRUE:
    {
        pushV(vm, V_TRUE_VAL);
        DISPATCH();
    }
    OP_FALSE:
    {
        pushV(vm, V_FALSE_VAL);
        DISPATCH();
    }
    OP_LIST:
    {
        u32 count = ReadByte();
        MyMoList *list = newList(vm);
        for (u32 i = 0; i < count; i++)
        {
            writeValueArray(vm, &list->values, peekV(vm, count - i - 1));
        }
        for (u32 i = 0; i < count; i++)
        {
            pop(vm);
        }
        push(vm, AS_OBJECT(list));
        DISPATCH();
    }
    OP_TUPLE:
    {
        u32 count = ReadByte();
        MyMoTuple *tuple = newTuple(vm);
        for (u32 i = 0; i < count; i++)
        {
            writeValueArray(vm, &tuple->values, peekV(vm, count - i - 1));
        }
        for (u32 i = 0; i < count; i++)
        {
            pop(vm);
        }
        push(vm, AS_OBJECT(tuple));
        DISPATCH();
    }
    OP_DICT:
    {
        // [k0, v0, k1, v1, ...] -> [dict], inserted left to right so the
        // dict keeps the literal's order. The pairs stay on the stack
        // (rooted) until the dict holds them.
        u32 count = ReadByte();
        Value *pairs = sp - 2 * count;
        MyMoDict *dict = newDict(vm);
        for (u32 i = 0; i < count; i++)
        {
            SAVE();
            MyMoObject *key = dictKey(vm, pairs[2 * i]);
            if (!key)
                goto _runtime_error;
            setEntryV(vm, dict, key, pairs[2 * i + 1]);
        }
        sp = pairs;
        lpush(V_OBJ_VAL(AS_OBJECT(dict)));
        DISPATCH();
    }
    OP_SUBSCRK:
    {
        // obj[key] op= v reads obj[key] but keeps obj and key for the
        // OP_SETSUBSCR that follows: duplicate them, then subscript.
        Value key = lpeek(0);
        Value obj = lpeek(1);
        lpush(obj);
        lpush(key);
        goto subscr;
    }
    OP_DELSUBSCR:
    {
        // del obj[key]: [obj, key] -> []
        Value key = lpeek(0);
        Value target = lpeek(1);
        SAVE();
        if (V_IS_OBJ_TYPE(target, OBJ_DICT))
        {
            if (!deleteEntry(vm, AS_DICT(V_AS_OBJ(target)), dictLookupKey(vm, key)))
            {
                Value text = valueToRepr(vm, key);
                runtimeError(vm, "KeyError: %s", V_IS_EMPTY(text) ? "?" : AS_STRING(V_AS_OBJ(text))->value);
                goto _runtime_error;
            }
        }
        else if (V_IS_OBJ_TYPE(target, OBJ_LIST))
        {
            ValueArray *values = &AS_LIST(V_AS_OBJ(target))->values;
            long i = valueLooksLikeInt(key) ? valueToLong(key) : LONG_MIN;
            if (i != LONG_MIN && i < 0)
                i += values->count;
            if (i < 0 || i >= values->count)
            {
                runtimeError(vm, valueLooksLikeInt(key) ? "IndexError: list assignment index out of range"
                                                        : "TypeError: list indices must be integers");
                goto _runtime_error;
            }
            memmove(&values->values[i], &values->values[i + 1], sizeof(Value) * (size_t)(values->count - i - 1));
            values->count--;
        }
        else if (V_IS_OBJ_TYPE(target, OBJ_INSTANCE))
        {
            // del obj[key] calls obj.__delitem__(key); discard its result.
            MyMoObject *method = getMethod(vm, V_AS_OBJ(target), "__delitem__");
            if (IS_EMPTY(method))
            {
                runtimeError(vm, "TypeError: %s does not support item deletion (no __delitem__)", valueTypeName(target));
                goto _runtime_error;
            }
            Value args[2] = {target, key};
            if (mymo_call(vm, V_OBJ_VAL(method), 2, args, NULL) != MYMO_OK)
                goto _runtime_error;
        }
        else
        {
            runtimeError(vm, "TypeError: %s does not support item deletion", valueTypeName(target));
            goto _runtime_error;
        }
        sp -= 2;
        DISPATCH();
    }
    OP_SUBSCR:
    subscr:
    {
        // [object, index] -> [object[index]]
        Value index = lpeek(0);
        Value target = lpeek(1);
        if (!V_IS_OBJ(target))
        {
            SAVE();
            runtimeError(vm, "TypeError: %s is not subscriptable", valueTypeName(target));
            goto _runtime_error;
        }
        MyMoObject *object = V_AS_OBJ(target);
        if (object->type == OBJ_DICT)
        {
            Value value;
            if (!getEntryV(AS_DICT(object), dictLookupKey(vm, index), &value))
            {
                SAVE();
                Value text = valueToRepr(vm, index);
                runtimeError(vm, "KeyError: %s", V_IS_EMPTY(text) ? "?" : AS_STRING(V_AS_OBJ(text))->value);
                goto _runtime_error;
            }
            sp -= 2;
            lpush(value);
            DISPATCH();
        }
        if (object->type == OBJ_INSTANCE)
        {
            sp -= 2;
            OperatorOverLoad(target, index, "__getitem__");
        }
        if (!valueLooksLikeInt(index) && !valueIsBool(index))
        {
            SAVE();
            runtimeError(vm, "TypeError: indices must be integers, not %s", valueTypeName(index));
            goto _runtime_error;
        }
        long i = valueIsBool(index) ? valueAsBool(index) : valueToLong(index);
        long count;
        switch (object->type)
        {
        case OBJ_STRING: count = AS_STRING(object)->chars; break;
        case OBJ_LIST:   count = AS_LIST(object)->values.count; break;
        case OBJ_TUPLE:  count = AS_TUPLE(object)->values.count; break;
        case OBJ_RANGE:  count = rangeLength(AS_RANGE(object)); break;
        default:
            SAVE();
            runtimeError(vm, "TypeError: %s is not subscriptable", getType(object));
            goto _runtime_error;
        }
        if (i < 0)
            i += count;
        if (i < 0 || i >= count)
        {
            SAVE();
            runtimeError(vm, "IndexError: %s index out of range", object->type == OBJ_STRING ? "string"
                         : object->type == OBJ_LIST ? "list" : object->type == OBJ_TUPLE ? "tuple" : "range");
            goto _runtime_error;
        }
        Value result;
        switch (object->type)
        {
        case OBJ_STRING:
        {
            MyMoString *str = AS_STRING(object);
            int at = stringByteOffset(str, (int)i);
            result = V_OBJ_VAL(AS_OBJECT(newString(vm, str->value + at, stringCharBytes(str, at))));
            break;
        }
        case OBJ_LIST:   result = AS_LIST(object)->values.values[i]; break;
        case OBJ_TUPLE:  result = AS_TUPLE(object)->values.values[i]; break;
        default:         result = valueFromLong(vm, rangeAt(AS_RANGE(object), i)); break;
        }
        sp -= 2;
        lpush(result);
        DISPATCH();
    }
    OP_SETSUBSCR:
    {
        // [object, index, value] -> [value]
        Value value = lpeek(0);
        Value index = lpeek(1);
        Value target = lpeek(2);
        if (V_IS_OBJ_TYPE(target, OBJ_LIST))
        {
            ValueArray *values = &AS_LIST(V_AS_OBJ(target))->values;
            if (!valueLooksLikeInt(index))
            {
                SAVE();
                runtimeError(vm, "TypeError: list indices must be integers, not %s", valueTypeName(index));
                goto _runtime_error;
            }
            long i = valueToLong(index);
            if (i < 0)
                i += values->count;
            if (i < 0 || i >= values->count)
            {
                SAVE();
                runtimeError(vm, "IndexError: list assignment index out of range");
                goto _runtime_error;
            }
            values->values[i] = value;
        }
        else if (V_IS_OBJ_TYPE(target, OBJ_DICT))
        {
            SAVE();
            MyMoObject *key = dictKey(vm, index);
            if (!key)
                goto _runtime_error;
            setEntryV(vm, AS_DICT(V_AS_OBJ(target)), key, value);
        }
        else if (V_IS_OBJ_TYPE(target, OBJ_INSTANCE))
        {
            // obj[index] = value calls obj.__setitem__(index, value).
            MyMoObject *method = getMethod(vm, V_AS_OBJ(target), "__setitem__");
            if (IS_EMPTY(method))
            {
                SAVE();
                runtimeError(vm, "TypeError: %s does not support item assignment (no __setitem__)", valueTypeName(target));
                goto _runtime_error;
            }
            sp[-3] = V_OBJ_VAL(method);
            sp[-2] = target;
            sp[-1] = index;
            lpush(value);
            SAVE();
            if (!caller(vm, method, 3))
                goto _runtime_error;
            LOAD();
            DISPATCH();
        }
        else
        {
            SAVE();
            runtimeError(vm, "TypeError: %s does not support item assignment", valueTypeName(target));
            goto _runtime_error;
        }
        sp -= 3;
        lpush(value);
        DISPATCH();
    }
    OP_UNPACK:
    {
        // [seq] -> [seq[0], ..., seq[n-1]] for a list, tuple or string of
        // exactly n elements (`a, b = pair`, `for k, v in pairs`).
        u32 count = ReadByte();
        Value seq = lpop();
        ValueArray *values = NULL;
        MyMoString *chars = NULL;
        int length = -1;
        if (V_IS_OBJ_TYPE(seq, OBJ_LIST))
            values = &AS_LIST(V_AS_OBJ(seq))->values;
        else if (V_IS_OBJ_TYPE(seq, OBJ_TUPLE))
            values = &AS_TUPLE(V_AS_OBJ(seq))->values;
        else if (V_IS_OBJ_TYPE(seq, OBJ_STRING))
            chars = AS_STRING(V_AS_OBJ(seq));
        else
        {
            SAVE();
            runtimeError(vm, "TypeError: cannot unpack %s", valueTypeName(seq));
            goto _runtime_error;
        }
        length = values ? values->count : chars->chars;
        if (length != (int)count)
        {
            SAVE();
            runtimeError(vm, "ValueError: expected %u values to unpack, got %d", count, length);
            goto _runtime_error;
        }
        if (values)
        {
            for (int i = 0; i < length; i++)
                lpush(values->values[i]);
        }
        else
        {
            // Keep the string reachable while the characters are allocated.
            lpush(seq);
            SAVE();
            for (int i = 0, at = 0; i < length; i++)
            {
                int n = stringCharBytes(chars, at);
                lpush(V_OBJ_VAL(NEW_STRING(vm, chars->value + at, n)));
                at += n;
            }
            for (int i = 0; i < length; i++)
                sp[-length - 1 + i] = sp[-length + i];
            sp--;
        }
        DISPATCH();
    }
    OP_SLICE:
    {
        // [seq, start, stop, step] -> [seq[start:stop:step]] with Python's
        // rules: Nil bounds default by direction, negative bounds count
        // from the end, out-of-range bounds clamp. Always a new sequence.
        Value stepV = lpeek(0), stopV = lpeek(1), startV = lpeek(2), seqV = lpeek(3);
        long bounds[3];
        Value boundVals[3] = {startV, stopV, stepV};
        for (int i = 0; i < 3; i++)
        {
            if (valueIsNil(boundVals[i]))
                continue;
            if (!valueLooksLikeInt(boundVals[i]))
            {
                SAVE();
                runtimeError(vm, "TypeError: slice indices must be integers or Nil, not %s", valueTypeName(boundVals[i]));
                goto _runtime_error;
            }
            bounds[i] = valueToLong(boundVals[i]);
        }
        long step = valueIsNil(stepV) ? 1 : bounds[2];
        if (step == 0)
        {
            SAVE();
            runtimeError(vm, "ValueError: slice step cannot be zero");
            goto _runtime_error;
        }
        long length;
        if (V_IS_OBJ_TYPE(seqV, OBJ_STRING))
            length = AS_STRING(V_AS_OBJ(seqV))->chars;
        else if (V_IS_OBJ_TYPE(seqV, OBJ_LIST))
            length = AS_LIST(V_AS_OBJ(seqV))->values.count;
        else if (V_IS_OBJ_TYPE(seqV, OBJ_TUPLE))
            length = AS_TUPLE(V_AS_OBJ(seqV))->values.count;
        else
        {
            SAVE();
            runtimeError(vm, "TypeError: %s can't be sliced", valueTypeName(seqV));
            goto _runtime_error;
        }
        long lower = step > 0 ? 0 : -1, upper = step > 0 ? length : length - 1;
        long ends[2];
        for (int i = 0; i < 2; i++)
        {
            if (valueIsNil(boundVals[i]))
                ends[i] = (i == 0) == (step > 0) ? lower : upper;
            else
            {
                long v = bounds[i];
                if (v < 0)
                {
                    v += length;
                    if (v < lower)
                        v = lower;
                }
                else if (v > upper)
                    v = upper;
                ends[i] = v;
            }
        }
        long start = ends[0], stop = ends[1];
        long count = step > 0 ? (stop > start ? (stop - start - 1) / step + 1 : 0)
                              : (start > stop ? (start - stop - 1) / -step + 1 : 0);
        Value result;
        SAVE();
        if (V_IS_OBJ_TYPE(seqV, OBJ_STRING))
        {
            MyMoString *str = AS_STRING(V_AS_OBJ(seqV));
            if (str->chars == str->length || count == 0)
            {
                char *buf = New(char, count + 1);
                for (long i = 0; i < count; i++)
                    buf[i] = str->value[start + i * step];
                result = V_OBJ_VAL(AS_OBJECT(newString(vm, buf, (int)count)));
                free(buf);
            }
            else if (step == 1)
            {
                int from = stringByteOffset(str, (int)start), to = stringByteOffset(str, (int)(start + count));
                result = V_OBJ_VAL(AS_OBJECT(newString(vm, str->value + from, to - from)));
            }
            else
            {
                // Non-ASCII with a step: copy code point by code point.
                int *offsets = malloc(sizeof(int) * (size_t)(str->chars + 1));
                for (int i = 0, c = 0; i <= str->length; i++)
                    if (i == str->length || ((unsigned char)str->value[i] & 0xC0) != 0x80)
                        offsets[c++] = i;
                char *buf = New(char, str->length + 1);
                int n = 0;
                for (long i = 0; i < count; i++)
                {
                    long c = start + i * step;
                    int len = offsets[c + 1] - offsets[c];
                    memcpy(buf + n, str->value + offsets[c], (size_t)len);
                    n += len;
                }
                result = V_OBJ_VAL(AS_OBJECT(newString(vm, buf, n)));
                free(buf);
                free(offsets);
            }
        }
        else
        {
            bool isList = V_IS_OBJ_TYPE(seqV, OBJ_LIST);
            ValueArray *src = isList ? &AS_LIST(V_AS_OBJ(seqV))->values : &AS_TUPLE(V_AS_OBJ(seqV))->values;
            MyMoObject *out = isList ? AS_OBJECT(newList(vm)) : AS_OBJECT(newTuple(vm));
            ValueArray *dst = isList ? &AS_LIST(out)->values : &AS_TUPLE(out)->values;
            for (long i = 0; i < count; i++)
                writeValueArray(vm, dst, src->values[start + i * step]);
            result = V_OBJ_VAL(out);
        }
        sp -= 4;
        lpush(result);
        DISPATCH();
    }
    OP_NOT:
    {
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, V_EMPTY_VAL, "!");
        }
        pushV(vm, V_BOOL_VAL(valueIsFalsey(a)));
        DISPATCH();
    }
    OP_DUP:
    {
        Value top = lpeek(0);
        lpush(top);
        DISPATCH();
    }
    OP_POP:
    {
        lpop(); // discard without boxing (pop() would allocate for ints)
        DISPATCH();
    }
    OP_EQUAL:
    {
        // Value-native fast path: identical NaN-boxed bits compare equal in
        // O(1), which covers same-tagged-int, same-pointer, same nil/bool.
        // Mismatched bits with both sides numeric coerce to double compare.
        // The `inplace` byte is consumed but not interpreted as a negation:
        // the compiler emits an explicit OP_NOT after OP_EQUAL for `!=`,
        // so we always push the equality result here and let OP_NOT
        // handle the inversion. (Earlier code inverted on inplace==1 and
        // then OP_NOT inverted again, which made `!=` return true on
        // equal operands — pre-existing bug.)
        u32 inplace = ReadByte();
        UNUSED(inplace);
        Value vb = peekV(vm, 0);
        Value va = peekV(vm, 1);
        if (!(V_IS_OBJ(va) && IS_INSTANCE(V_AS_OBJ(va))))
        {
            popV(vm); popV(vm);
            bool eq = valuesEqual(va, vb);
            pushV(vm, V_BOOL_VAL(eq));
            DISPATCH();
        }
        // Slow path: instance with __eq__/__ne__ overload. Fall through to
        // the legacy heap-object dispatch.
        Value b = popV(vm);
        Value a = popV(vm);
        MyMoObject *method = getMethod(vm, V_AS_OBJ(a), inplace ? "!=" : "==");
        if (IS_EMPTY(method) && inplace)
        {
            // No != method: use == (the OP_NOT that follows negates it).
            method = getMethod(vm, V_AS_OBJ(a), "==");
            inplace = 0;
        }
        if (IS_EMPTY(method))
        {
            pushV(vm, V_BOOL_VAL(valuesEqual(a, b)));
            DISPATCH();
        }
        if (inplace)
        {
            UNUSED(ReadByte()); // != itself: skip the OP_NOT
        }
        pushV(vm, V_OBJ_VAL(method));
        pushV(vm, a);
        pushV(vm, b);
        SAVE();
        if (!caller(vm, method, 2))
        {
            goto _runtime_error;
        }
        LOAD();
        DISPATCH();
    }
    OP_GREATER:
    {
        u32 inplace = ReadByte();
        Value vb = lpeek(0);
        Value va = lpeek(1);
        if (V_IS_INT(va) && V_IS_INT(vb))
        {
            int32_t a = V_AS_INT(va);
            int32_t b = V_AS_INT(vb);
            sp -= 2;
            lpush(V_BOOL_VAL(a > b));
            DISPATCH();
        }
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long a = valueToLong(va);
            long b = valueToLong(vb);
            sp -= 2;
            lpush(V_BOOL_VAL(a > b));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double a = valueAsNumber(va);
            double b = valueAsNumber(vb);
            sp -= 2;
            lpush(V_BOOL_VAL(a > b));
            DISPATCH();
        }
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            if (inplace) UNUSED(ReadByte());
            OperatorOverLoad(a, b, inplace ? "<=" : ">");
        }
        if (V_IS_OBJ_TYPE(a, OBJ_STRING) && V_IS_OBJ_TYPE(b, OBJ_STRING))
        {
            pushV(vm, V_BOOL_VAL(compareStrings(AS_STRING(V_AS_OBJ(a)), AS_STRING(V_AS_OBJ(b))) > 0));
            DISPATCH();
        }
        if ((V_IS_OBJ_TYPE(a, OBJ_LIST) && V_IS_OBJ_TYPE(b, OBJ_LIST)) ||
            (V_IS_OBJ_TYPE(a, OBJ_TUPLE) && V_IS_OBJ_TYPE(b, OBJ_TUPLE)))
        {
            // Lexicographic. a and b stay on the stack while an
            // element's "<" method may run.
            lpush(a);
            lpush(b);
            SAVE();
            bool result;
            if (!lessThan(vm, b, a, &result))
                goto _runtime_error;
            sp -= 2;
            pushV(vm, V_BOOL_VAL(result));
            DISPATCH();
        }
        if (!valueLooksLikeNumber(a) || !valueLooksLikeNumber(b))
        {
            runtimeError(vm, "TypeError: operands must be numbers");
            goto _runtime_error;
        }
        pushV(vm, V_BOOL_VAL(valueAsNumber(a) > valueAsNumber(b)));
        DISPATCH();
    }
    OP_LESS:
    {
        u32 inplace = ReadByte();
        Value vb = lpeek(0);
        Value va = lpeek(1);
        // Hottest path: inline-int comparison.
        if (V_IS_INT(va) && V_IS_INT(vb))
        {
            int32_t a = V_AS_INT(va);
            int32_t b = V_AS_INT(vb);
            sp -= 2;
            lpush(V_BOOL_VAL(a < b));
            DISPATCH();
        }
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long a = valueToLong(va);
            long b = valueToLong(vb);
            sp -= 2;
            lpush(V_BOOL_VAL(a < b));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double a = valueAsNumber(va);
            double b = valueAsNumber(vb);
            sp -= 2;
            lpush(V_BOOL_VAL(a < b));
            DISPATCH();
        }
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            if (inplace) UNUSED(ReadByte());
            OperatorOverLoad(a, b, inplace ? ">=" : "<");
        }
        if (V_IS_OBJ_TYPE(a, OBJ_STRING) && V_IS_OBJ_TYPE(b, OBJ_STRING))
        {
            pushV(vm, V_BOOL_VAL(compareStrings(AS_STRING(V_AS_OBJ(a)), AS_STRING(V_AS_OBJ(b))) < 0));
            DISPATCH();
        }
        if ((V_IS_OBJ_TYPE(a, OBJ_LIST) && V_IS_OBJ_TYPE(b, OBJ_LIST)) ||
            (V_IS_OBJ_TYPE(a, OBJ_TUPLE) && V_IS_OBJ_TYPE(b, OBJ_TUPLE)))
        {
            // Lexicographic. a and b stay on the stack while an
            // element's "<" method may run.
            lpush(a);
            lpush(b);
            SAVE();
            bool result;
            if (!lessThan(vm, a, b, &result))
                goto _runtime_error;
            sp -= 2;
            pushV(vm, V_BOOL_VAL(result));
            DISPATCH();
        }
        if (!valueLooksLikeNumber(a) || !valueLooksLikeNumber(b))
        {
            runtimeError(vm, "TypeError: operands must be numbers");
            goto _runtime_error;
        }
        pushV(vm, V_BOOL_VAL(valueAsNumber(a) < valueAsNumber(b)));
        DISPATCH();
    }
    OP_BAND:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_SET) && V_IS_OBJ_TYPE(b, OBJ_SET))
        {
            pushV(vm, V_OBJ_VAL(AS_OBJECT(setCombine(vm, AS_SET(V_AS_OBJ(a)), AS_SET(V_AS_OBJ(b)), '&'))));
            DISPATCH();
        }
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "&=" : "&");
        }
        BitwiseOp(a, b, &);
        DISPATCH();
    }
    OP_BOR:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_SET) && V_IS_OBJ_TYPE(b, OBJ_SET))
        {
            pushV(vm, V_OBJ_VAL(AS_OBJECT(setCombine(vm, AS_SET(V_AS_OBJ(a)), AS_SET(V_AS_OBJ(b)), '|'))));
            DISPATCH();
        }
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "|=" : "|");
        }
        BitwiseOp(a, b, |);
        DISPATCH();
    }
    OP_BXOR:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_SET) && V_IS_OBJ_TYPE(b, OBJ_SET))
        {
            pushV(vm, V_OBJ_VAL(AS_OBJECT(setCombine(vm, AS_SET(V_AS_OBJ(a)), AS_SET(V_AS_OBJ(b)), '^'))));
            DISPATCH();
        }
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "^=" : "^");
        }
        BitwiseOp(a, b, ^);
        DISPATCH();
    }
    OP_ADD:
    {
        u32 inplace = ReadByte();
        // Hottest path: both inline ints. Compute in 64-bit so we can detect
        // overflow and box to a heap MyMoInt for results outside int32 range
        // (matches Python/Ruby semantics: arithmetic doesn't silently wrap).
        Value vb = lpeek(0);
        Value va = lpeek(1);
        if (V_IS_INT(va) && V_IS_INT(vb))
        {
            long r = (long)V_AS_INT(va) + (long)V_AS_INT(vb);
            sp -= 2;
            if (r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpushObj(NEW_INT(vm, r));
            DISPATCH();
        }
        long r;
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb) && !__builtin_add_overflow(valueToLong(va), valueToLong(vb), &r))
        {
            // (On 64-bit overflow, fall through to the double path.)
            sp -= 2;
            if (r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpushObj(NEW_INT(vm, r));
            DISPATCH();
        }
        // Inline-double fast path. Takes any mix of int/double on either
        // side (inline or heap) and produces an inline V_DOUBLE_VAL.
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double r = valueAsNumber(va) + valueAsNumber(vb);
            sp -= 2;
            lpush(V_DOUBLE_VAL(r));
            DISPATCH();
        }
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "+=" : "+");
        }
        SAVE(); // sync ip so a TypeError inside addition() has the right line
        Value result = addValues(vm, a, b);
        if (V_IS_EMPTY(result))
            goto _runtime_error;
        pushV(vm, result);
        DISPATCH();
    }
    OP_SUB:
    {
        u32 inplace = ReadByte();
        Value vb = lpeek(0);
        Value va = lpeek(1);
        if (V_IS_INT(va) && V_IS_INT(vb))
        {
            long r = (long)V_AS_INT(va) - (long)V_AS_INT(vb);
            sp -= 2;
            if (r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpushObj(NEW_INT(vm, r));
            DISPATCH();
        }
        long r;
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb) && !__builtin_sub_overflow(valueToLong(va), valueToLong(vb), &r))
        {
            // (On 64-bit overflow, fall through to the double path.)
            sp -= 2;
            if (r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpushObj(NEW_INT(vm, r));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double r = valueAsNumber(va) - valueAsNumber(vb);
            sp -= 2;
            lpush(V_DOUBLE_VAL(r));
            DISPATCH();
        }
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "-=" : "-");
        }
        SAVE();
        Value result = subValues(vm, a, b);
        if (V_IS_EMPTY(result))
            goto _runtime_error;
        pushV(vm, result);
        DISPATCH();
    }
    OP_MUL:
    {
        u32 inplace = ReadByte();
        Value vb = lpeek(0);
        Value va = lpeek(1);
        if (V_IS_INT(va) && V_IS_INT(vb))
        {
            long r = (long)V_AS_INT(va) * (long)V_AS_INT(vb);
            sp -= 2;
            if (r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpushObj(NEW_INT(vm, r));
            DISPATCH();
        }
        long r;
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb) && !__builtin_mul_overflow(valueToLong(va), valueToLong(vb), &r))
        {
            // (On 64-bit overflow, fall through to the double path.)
            sp -= 2;
            if (r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpushObj(NEW_INT(vm, r));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double r = valueAsNumber(va) * valueAsNumber(vb);
            sp -= 2;
            lpush(V_DOUBLE_VAL(r));
            DISPATCH();
        }
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "*=" : "*");
        }
        SAVE();
        Value result = mulValues(vm, a, b);
        if (V_IS_EMPTY(result))
            goto _runtime_error;
        pushV(vm, result);
        DISPATCH();
    }
    OP_DIV:
    {
        u32 inplace = ReadByte();
        Value vb = lpeek(0);
        Value va = lpeek(1);
        // MyMo's `/` returns int when both operands are int and the
        // result is integer-valued; otherwise it returns double
        // (mirrors operations.c::division). Two fast paths preserve
        // that, both with an explicit zero-denominator guard.
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long b_val = valueToLong(vb);
            if (b_val == 0)
            {
                runtimeError(vm, "ZeroDivisionError: division by zero.");
                goto _runtime_error;
            }
            long a_val = valueToLong(va);
            double r = (double)a_val / (double)b_val;
            sp -= 2;
            if (r == (long)r && r >= INT32_MIN && r <= INT32_MAX)
                lpush(V_INT_VAL((int32_t)r));
            else
                lpush(V_DOUBLE_VAL(r));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double bn = valueAsNumber(vb);
            if (bn == 0.0)
            {
                runtimeError(vm, "ZeroDivisionError: division by zero.");
                goto _runtime_error;
            }
            double r = valueAsNumber(va) / bn;
            sp -= 2;
            lpush(V_DOUBLE_VAL(r));
            DISPATCH();
        }
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "/=" : "/");
        }
        SAVE();
        Value result = divValues(vm, a, b);
        if (V_IS_EMPTY(result))
            goto _runtime_error;
        pushV(vm, result);
        DISPATCH();
    }
    OP_POW:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "**=" : "**");
        }
        if (!valueLooksLikeNumber(a) || !valueLooksLikeNumber(b))
        {
            runtimeError(vm, "TypeError: operands must be numbers");
            goto _runtime_error;
        }
        if (valueLooksLikeInt(a) && valueLooksLikeInt(b) && valueToLong(b) >= 0)
        {
            // Exact integer power by squaring; fall back to double on
            // overflow of a signed 64-bit long.
            long base = valueToLong(a), exp = valueToLong(b), acc = 1;
            bool overflow = false;
            while (exp > 0 && !overflow)
            {
                if ((exp & 1) && __builtin_mul_overflow(acc, base, &acc))
                    overflow = true;
                exp >>= 1;
                if (exp > 0 && __builtin_mul_overflow(base, base, &base))
                    overflow = true;
            }
            if (!overflow)
            {
                pushV(vm, intResult(vm, acc));
                DISPATCH();
            }
        }
        pushV(vm, V_DOUBLE_VAL(pow(valueAsNumber(a), valueAsNumber(b))));
        DISPATCH();
    }
    OP_MOD:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "%=" : "%");
        }
        if (V_IS_OBJ_TYPE(a, OBJ_STRING))
        {
            // "%d items" % n, "%s=%r" % (k, v), "%(name)s" % {"name": x}
            lpush(a); // keep the operands rooted while formatting
            lpush(b);
            SAVE();
            Value text = percentFormat(vm, AS_STRING(V_AS_OBJ(a)), b);
            if (V_IS_EMPTY(text))
                goto _runtime_error;
            sp -= 2;
            lpush(text);
            DISPATCH();
        }
        if (!valueLooksLikeNumber(a) || !valueLooksLikeNumber(b))
        {
            SAVE();
            runtimeError(vm, "TypeError: operands must be numbers");
            goto _runtime_error;
        }
        // Python semantics: the result takes the divisor's sign.
        if (valueAsNumber(b) == 0)
        {
            runtimeError(vm, "ZeroDivisionError: modulo by zero");
            goto _runtime_error;
        }
        if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
        {
            long x = valueToLong(a), y = valueToLong(b), r = x % y;
            if (r != 0 && ((r < 0) != (y < 0)))
                r += y;
            pushV(vm, intResult(vm, r));
            DISPATCH();
        }
        double y = valueAsNumber(b);
        double r = fmod(valueAsNumber(a), y);
        if (r != 0 && ((r < 0) != (y < 0)))
            r += y;
        pushV(vm, V_DOUBLE_VAL(r));
        DISPATCH();
    }
    OP_LSFT:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "<<=" : "<<");
        }
        BitwiseOp(a, b, <<);
        DISPATCH();
    }
    OP_RSFT:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? ">>=" : ">>");
        }
        BitwiseOp(a, b, >>);
        DISPATCH();
    }
    OP_IDIV:
    {
        u32 inplace = ReadByte();
        Value b = popV(vm);
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, b, inplace ? "//=" : "//");
        }
        if (!valueLooksLikeNumber(a) || !valueLooksLikeNumber(b))
        {
            runtimeError(vm, "TypeError: operands must be numbers");
            goto _runtime_error;
        }
        // Floor division (Python semantics): -7 // 2 == -4.
        if (valueAsNumber(b) == 0)
        {
            runtimeError(vm, "ZeroDivisionError: integer division by zero");
            goto _runtime_error;
        }
        if (valueLooksLikeInt(a) && valueLooksLikeInt(b))
        {
            long x = valueToLong(a), y = valueToLong(b), q = x / y;
            if ((x % y != 0) && ((x < 0) != (y < 0)))
                q--;
            pushV(vm, intResult(vm, q));
            DISPATCH();
        }
        pushV(vm, V_DOUBLE_VAL(floor(valueAsNumber(a) / valueAsNumber(b))));
        DISPATCH();
    }
    OP_NEG:
    {
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, V_EMPTY_VAL, "-@");
        }
        UnaryOp(-, a);
        DISPATCH();
    }
    OP_POS:
    {
        Value a = popV(vm);
        if (V_IS_OBJ_TYPE(a, OBJ_INSTANCE))
        {
            OperatorOverLoad(a, V_EMPTY_VAL, "+@");
        }
        UnaryOp(+, a);
        DISPATCH();
    }
    OP_JIF:
    {
        u16 offset = ReadShort();
        if (isFalseyV(lpeek(0)))
        {
            ip += offset;
        }
        DISPATCH();
    }
    OP_JMP:
    {
        u16 offset = ReadShort();
        ip += offset;
        DISPATCH();
    }
    OP_CJMP:
    {
        u16 offset = ReadShort();
        MyMoObject *lhs = pop(vm);
        if (!isEqual(lhs, peek(vm, 0)))
        {
            ip += offset;
        }
        else
        {
            pop(vm);
        }
        DISPATCH();
    }
    OP_MCASE:
    {
        int count = ReadByte();
        MyMoObject *compare = peek(vm, count + 1);
        MyMoObject *match = pop(vm);
        for (int i = 0; i < count; ++i)
        {
            if (isEqual(compare, match))
            {
                i++;
                while (i <= count)
                {
                    pop(vm);
                    i++;
                }
                break;
            }
            match = pop(vm);
        }
        push(vm, match);
        DISPATCH();
    }
    OP_LOOP:
    {
        u16 offset = ReadShort();
        ip -= offset;
        GC_SAFEPOINT();
        DISPATCH();
    }
    OP_ITER:
    {
        // [iter] -> [iter, next] or jump past the loop when exhausted.
        if (sp - vm->fiber->stack.values >= 2 && V_IS_OBJ_TYPE(lpeek(1), OBJ_ITER) &&
            AS_ITER(V_AS_OBJ(lpeek(1)))->waiting)
        {
            // Back from running the iterated fiber: [iter, value]. A value
            // from a yield is the next element; from the fiber finishing,
            // it is the return value and the loop ends.
            u16 offset = ReadShort();
            Value value = lpop();
            MyMoIter *iterator = AS_ITER(V_AS_OBJ(lpeek(0)));
            iterator->waiting = false;
            if (AS_FIBER(iterator->iterator)->state == FIBER_DEAD)
                ip += offset;
            else
                lpush(value);
            DISPATCH();
        }
        u16 offset = ReadShort();
        MyMoIter *iterator = AS_ITER(V_AS_OBJ(lpeek(0)));
        MyMoObject *source = iterator->iterator;
        if (source->type == OBJ_FIBER)
        {
            // Run the fiber to its next yield (or its end), then come back
            // to this instruction: the fiber replaces our placeholder
            // with the value it yields or returns.
            MyMoFiber *fiber = AS_FIBER(source);
            if (fiber->state == FIBER_DEAD)
            {
                ip += offset;
                DISPATCH();
            }
            if (fiber->state == FIBER_RUNNING)
            {
                SAVE();
                runtimeError(vm, "RuntimeError: cannot iterate a running fiber");
                goto _runtime_error;
            }
            MyMoFunction *body = fiber->callFrames[0]->function;
            if (fiber->state == FIBER_READY)
            {
                int fill = missingDefaults(body, 0);
                if (fill < 0)
                {
                    SAVE();
                    runtimeError(vm, "TypeError: a fiber iterated by for must not need arguments "
                                     "(wrap the call: fiber(() => %s(...)))", body->name ? body->name->value : "fn");
                    goto _runtime_error;
                }
                for (int i = 0; i < fill && i < CALLFRAME_ARGS_INLINE; i++)
                    fiber->callFrames[0]->args[i] = body->defaults[body->defaultCount - fill + i];
                fiber->callFrames[0]->ip = body->chunk->code;
            }
            lpush(V_NIL_VAL); // placeholder the fiber's yield/return replaces
            iterator->waiting = true;
            ip -= 3;          // resume at this OP_ITER
            SAVE();
            bool starting = fiber->state == FIBER_READY;
            fiber->parent = vm->fiber;
            fiber->state = FIBER_RUNNING;
            vm->fiber = fiber;
            if (starting) // frame 0's callee slot (not pushV: that's the register sp)
                fiber->stack.values[fiber->stack.count++] = V_OBJ_VAL(AS_OBJECT(body));
            else
                fiber->stack.values[fiber->stack.count - 1] = V_NIL_VAL; // the pending yield() returns Nil
            LOAD();
            DISPATCH();
        }
        if (source->type == OBJ_INSTANCE)
        {
            // obj.__next__() until it raises StopIteration.
            MyMoObject *method = getMethod(vm, source, "__next__");
            SAVE();
            Value self = V_OBJ_VAL(source), next;
            vm->quietErrors++;
            MyMoResult called = mymo_call(vm, V_OBJ_VAL(method), 1, &self, &next);
            vm->quietErrors--;
            if (called != MYMO_OK)
            {
                MyMoObject *exc = vm->fiber->exception;
                Value stop;
                if (exc && exc->type == OBJ_INSTANCE &&
                    getEntryV(&vm->builtins, AS_OBJECT(newString(vm, "StopIteration", 13)), &stop) &&
                    isInstanceOf(vm, V_OBJ_VAL(exc), stop))
                {
                    vm->fiber->exception = NULL;
                    ip += offset;
                    DISPATCH();
                }
                // A real error: report it here (the exception object stays
                // the one __next__ raised).
                Value text = exc ? valueToStr(vm, objectToValue(exc)) : V_EMPTY_VAL;
                vm->fiber->exception = exc;
                runtimeError(vm, "%s", V_IS_EMPTY(text) ? vm->lastError : AS_STRING(V_AS_OBJ(text))->value);
                goto _runtime_error;
            }
            lpush(next);
            DISPATCH();
        }
        Value next = nextIter(vm, iterator);
        if (V_IS_EMPTY(next))
            ip += offset;
        else
            lpush(next);
        DISPATCH();
    }
    OP_GETI:
    {
        Value iterableV = lpeek(0);
        MyMoObject *iterator = V_IS_OBJ(iterableV) ? V_AS_OBJ(iterableV) : NULL;
        if (iterator && iterator->type == OBJ_INSTANCE)
        {
            // obj.__iter__() gives the iterator (an object with __next__,
            // a fiber, or any built-in iterable); an object with only
            // __next__ is its own iterator.
            MyMoObject *iterMethod = getMethod(vm, iterator, "__iter__");
            if (!IS_EMPTY(iterMethod))
            {
                SAVE();
                Value self = iterableV, result;
                if (mymo_call(vm, V_OBJ_VAL(iterMethod), 1, &self, &result) != MYMO_OK)
                    goto _runtime_error;
                sp[-1] = result;
                iterableV = result;
                iterator = V_IS_OBJ(result) ? V_AS_OBJ(result) : NULL;
            }
            if (iterator && iterator->type == OBJ_INSTANCE && IS_EMPTY(getMethod(vm, iterator, "__next__")))
            {
                SAVE();
                runtimeError(vm, "TypeError: %s is not iterable (no __iter__ or __next__)", valueTypeName(iterableV));
                goto _runtime_error;
            }
        }
        switch (iterator ? iterator->type : OBJ_NIL)
        {
        case OBJ_STRING:
        case OBJ_LIST:
        case OBJ_TUPLE:
        case OBJ_DICT:
        case OBJ_RANGE:
        case OBJ_SET:
        case OBJ_FIBER:
        case OBJ_INSTANCE:
        {
            MyMoIter *it = newIter(vm, iterator);
            sp[-1] = V_OBJ_VAL(AS_OBJECT(it));
            DISPATCH();
        }
        default:
            SAVE();
            runtimeError(vm, "TypeError: %s is not iterable", valueTypeName(iterableV));
            goto _runtime_error;
        }
    }
    OP_GETV:
    {
        MyMoObject *variable = ReadObject();
        // Inline-cache fast path.
        u8 *icp = ip;
        u8 ic_tag = icp[0];
        u16 ic_idx = (u16)icp[2] | ((u16)icp[3] << 8);
        u32 ic_ver = (u32)icp[4] | ((u32)icp[5] << 8) | ((u32)icp[6] << 16) | ((u32)icp[7] << 24);
        ip += IC_BYTES;
        if (ic_tag == IC_TAG_GLOBALS)
        {
            MyMoDict *d = &vm->globals;
            if (d->modifyCount == ic_ver && (int)ic_idx <= d->capacity
                && d->entries[ic_idx].key == variable)
            {
                lpush(d->entries[ic_idx].value);
                DISPATCH();
            }
        }
        else if (ic_tag == IC_TAG_LOCALS)
        {
            MyMoDict *d = &frame->locals;
            if (d->modifyCount == ic_ver && (int)ic_idx <= d->capacity
                && d->entries[ic_idx].key == variable)
            {
                lpush(d->entries[ic_idx].value);
                DISPATCH();
            }
        }
        // ---- Slow path: full lookup chain (existing semantics) --------
        Value val;
        if (vm->currentClass)
        {
            if (getEntryV(vm->currentClass->variables, variable, &val))
            {
                pushV(vm, val);
                DISPATCH();
            }
            if (getEntryV(vm->currentClass->methods, variable, &val))
            {
                pushV(vm, val);
                DISPATCH();
            }
        }
        // Helper: fill the IC for this OP_GETV with (tag, dict, entry_idx).
        // Macro parameter is `_k` (not `key`) to avoid shadowing Entry.key
        // during text substitution — `entries[i].key` would otherwise
        // expand into `entries[i].<arg>`.
        #define FILL_IC(tag, d, _k)                                                       \
            do {                                                                          \
                Entry *_es = (d)->entries;                                                \
                int _cap = (d)->capacity;                                                 \
                if (_es && _cap >= 0) {                                                   \
                    u32 _h = ((MyMoObject*)(_k))->hash & (u32)_cap;                       \
                    while (_es[_h].key != (MyMoObject*)(_k)) _h = (_h + 1) & (u32)_cap;   \
                    icp[0] = (tag);                                                       \
                    icp[1] = 0;                                                           \
                    icp[2] = (u8)(_h & 0xff);                                             \
                    icp[3] = (u8)((_h >> 8) & 0xff);                                      \
                    icp[4] = (u8)((d)->modifyCount & 0xff);                               \
                    icp[5] = (u8)(((d)->modifyCount >> 8) & 0xff);                        \
                    icp[6] = (u8)(((d)->modifyCount >> 16) & 0xff);                       \
                    icp[7] = (u8)(((d)->modifyCount >> 24) & 0xff);                       \
                }                                                                          \
            } while (0)
        if ((IS_FIBER_ROOT(vm->fiber)) && vm->fiber->frameCount == 0)
        {
            if (getEntryV(&vm->globals, variable, &val))
            {
                FILL_IC(IC_TAG_GLOBALS, &vm->globals, variable);
                pushV(vm, val);
                DISPATCH();
            }
            goto builtinvars;
        }
        else
        {
            if (getEntryV(&frame->locals, variable, &val))
            {
                FILL_IC(IC_TAG_LOCALS, &frame->locals, variable);
                pushV(vm, val);
                DISPATCH();
            }
            if (lookupEnclosing(vm, frame->function->frame, variable, &val))
            {
                // Not copied into our locals: the enclosing variable may
                // change (nonlocal, or the outer function reassigning it).
                pushV(vm, val);
                DISPATCH();
            }
        }
        if (getEntryV(&vm->globals, variable, &val))
        {
            FILL_IC(IC_TAG_GLOBALS, &vm->globals, variable);
            pushV(vm, val);
            DISPATCH();
        }
        else
        {
        builtinvars:
            if (getEntryV(&vm->builtins, variable, &val))
            {
                pushV(vm, val);
                DISPATCH();
            }
        }
        runtimeError(vm, "NameError: Undefined variable '%s'.", STRING_VAL(variable));
        goto _runtime_error;
    }
    OP_SETV:
    {
        MyMoObject *variable = ReadObject();
        u8 *icp = ip;
        u8 ic_tag = icp[0];
        u16 ic_idx = (u16)icp[2] | ((u16)icp[3] << 8);
        u32 ic_ver = (u32)icp[4] | ((u32)icp[5] << 8) | ((u32)icp[6] << 16) | ((u32)icp[7] << 24);
        ip += IC_BYTES;
        if (!vm->currentClass)
        {
            MyMoDict *target = NULL;
            u8 hit_tag = IC_TAG_COLD;
            if (!vm->fiber->parent && vm->fiber->frameCount == 0)
            {
                target = &vm->globals; hit_tag = IC_TAG_GLOBALS;
            }
            else
            {
                target = &frame->locals; hit_tag = IC_TAG_LOCALS;
            }
            // Cache hit: same dict shape, same key at same slot.
            if (ic_tag == hit_tag && target->modifyCount == ic_ver
                && (int)ic_idx <= target->capacity
                && target->entries[ic_idx].key == variable)
            {
                // Direct Value-native write — no boxing of inline ints.
                target->entries[ic_idx].value = peekV(vm, 0);
                DISPATCH();
            }
            // Slow path: do the set, then refresh the cache.
            setEntryV(vm, target, variable, peekV(vm, 0));
            Entry *es = target->entries;
            int cap = target->capacity;
            if (es && cap >= 0)
            {
                u32 h = variable->hash & (u32)cap;
                while (es[h].key != variable) h = (h + 1) & (u32)cap;
                icp[0] = hit_tag;
                icp[1] = 0;
                icp[2] = (u8)(h & 0xff);
                icp[3] = (u8)((h >> 8) & 0xff);
                icp[4] = (u8)(target->modifyCount & 0xff);
                icp[5] = (u8)((target->modifyCount >> 8) & 0xff);
                icp[6] = (u8)((target->modifyCount >> 16) & 0xff);
                icp[7] = (u8)((target->modifyCount >> 24) & 0xff);
            }
            DISPATCH();
        }
        // Class-context path: don't cache (rare, stays as-is).
        setEntry(vm, vm->currentClass->variables, variable, peek(vm, 0));
        DISPATCH();
    }
    OP_DELV:
    {
        MyMoObject *variable = ReadObject();
        if (deleteEntry(vm, &frame->locals, variable) || deleteEntry(vm, &vm->globals, variable))
        {
            DISPATCH();
        }
        SAVE();
        runtimeError(vm, "NameError: Undefined variable '%s'.", AS_STRING(variable)->value);
        goto _runtime_error;
    }
    OP_MET:
    {
        MyMoObject *method = ReadObject();
        MyMoObject *name = ReadObject();
        MyMoClass *klass = AS_CLASS(peek(vm, 0));
        if (AS_STRING(name)->length == 8 && memcmp(AS_STRING(name)->value, "__init__", 8) == 0)
        {
            klass->init = method;
        }
        installMethod(vm, klass, name, V_OBJ_VAL(method));
        AS_FUNCTION(method)->klass = AS_OBJECT(klass);
        DISPATCH();
    }
    OP_FN:
    {
        // Functions defined at script/module top level are created once
        // and their defining frame lives forever, so the compile-time
        // constant can be used directly. Inside a call, each execution
        // makes a closure: a copy of the constant (so two calls of the
        // outer fn don't overwrite each other's `frame`) bound to this
        // frame, which is marked captured so OP_FRET won't recycle it.
        MyMoFunction *function = AS_FUNCTION(ReadObject());
        FunctionType ft = frame->function->type;
        if (ft != FN_SCRIPT && ft != FN_MODULE && ft != FN_COMPILED)
        {
            function = cloneFunction(vm, function);
            frame->captured = true;
        }
        function->frame = frame;
        push(vm, AS_OBJECT(function));
        DISPATCH();
    }
    OP_CALL:
    {
        GC_SAFEPOINT();
        u8 argCount = ReadByte();
        // Fast path: OBJ_FUNCTION call. Skips the caller()/callFunction()
        // indirection and inlines the frame setup. Covers the dominant
        // case for tight recursion (fib, fact, etc.). Falls back to the
        // generic caller() for builtins, classes, methods, bound methods.
        Value calleeV = lpeek((int)argCount);
        if (V_IS_OBJ(calleeV))
        {
            MyMoObject *cobj = V_AS_OBJ(calleeV);
            if (cobj->type == OBJ_FUNCTION && !AS_FUNCTION(cobj)->isargs)
            {
                MyMoFunction *function = AS_FUNCTION(cobj);
                {
                    int fill = missingDefaults(function, argCount);
                    if (fill < 0)
                    {
                        SAVE();
                        arityError(vm, function, argCount);
                        goto _runtime_error;
                    }
                    for (int i = 0; i < fill; i++)
                        *sp++ = function->defaults[function->defaultCount - fill + i];
                    argCount += fill;
                }
                if (function->type == FN_SCRIPT || function->type == FN_MODULE)
                {
                    SAVE();
                    runtimeError(vm, "TypeError: <%s '%s'> is not callable.",
                                 function->type == FN_SCRIPT ? "Script" : "module",
                                 function->name->value);
                    goto _runtime_error;
                }
                if (STACK_EXHAUSTED(sp - vm->fiber->stack.values))
                {
                    SAVE();
                    runtimeError(vm, "RecursionError: maximum recursion depth exceeded.");
                    goto _runtime_error;
                }
                // Frame capacity check (off-by-one fix from Phase 5e).
                if (vm->fiber->frameCapacity < (uint)(vm->fiber->frameCount + 2))
                {
                    SAVE();
                    u32 capacity = vm->fiber->frameCapacity;
                    vm->fiber->frameCapacity = ResizeCapacity(vm->fiber->frameCapacity);
                    while (vm->fiber->frameCapacity < (uint)(vm->fiber->frameCount + 2))
                        vm->fiber->frameCapacity = ResizeCapacity(vm->fiber->frameCapacity);
                    vm->fiber->callFrames = ResizeArray(vm, CallFrame *, vm->fiber->callFrames, capacity, vm->fiber->frameCapacity);
                }
                // Pull a frame from the pool, fall back to malloc.
                CallFrame *newFrame;
                if (vm->fiber->freeFramesHead != NULL)
                {
                    newFrame = vm->fiber->freeFramesHead;
                    vm->fiber->freeFramesHead = (CallFrame *)newFrame->function;
                }
                else
                {
                    newFrame = New(CallFrame, 1);
                    initDict(&newFrame->locals);
                    newFrame->gcEpoch = 0;
                    newFrame->nextRetired = NULL;
                }
                newFrame->function = function;
                newFrame->captured = false;
                newFrame->calleeSlot = true;
                newFrame->ip = function->chunk->code;
                vm->fiber->callFrames[++vm->fiber->frameCount] = newFrame;
                // Pop args from operand stack (using local sp register)
                // directly into the new frame's slot array. No dict.
                if (argCount)
                {
                    if ((int)argCount <= CALLFRAME_ARGS_INLINE)
                    {
                        for (int i = (int)argCount - 1; i >= 0; i--)
                            newFrame->args[i] = *--sp;
                    }
                    else
                    {
                        for (int i = (int)argCount - 1; i >= 0; i--)
                            setEntry(vm, &newFrame->locals, AS_OBJECT(function->argv[i]), V_AS_OBJ(*--sp));
                    }
                }
                // Save caller frame ip and switch.
                frame->ip = ip;
                frame = newFrame;
                ip = frame->ip;
                DISPATCH();
            }
        }
        // Slow path: builtins, classes, bound methods, etc. Go through
        // the generic dispatch with full SAVE/LOAD.
        SAVE();
        if (!caller(vm, V_AS_OBJ(calleeV), argCount))
            goto _runtime_error;
        LOAD();
        DISPATCH();
    }
    OP_EXCMATCH:
    {
        // [exc, types] -> [bool]: does `catch types` handle exc?
        Value types = lpop();
        Value exc = lpop();
        SAVE();
        bool match;
        if (!exceptionMatches(vm, exc, types, &match))
            goto _runtime_error;
        lpush(V_BOOL_VAL(match));
        DISPATCH();
    }
    OP_RERAISE:
    {
        // End of a `final:` block: [pending] -> []; raises it again
        // unless it is Nil (no exception was in flight).
        Value pending = lpeek(0);
        if (valueIsNil(pending))
        {
            lpop();
            DISPATCH();
        }
        goto raise_value;
    }
    OP_FORMAT:
    {
        // [value, spec] -> [text]; conversion 'r' / 's' applies repr/str
        // before the spec.
        u8 conversion = ReadByte();
        MyMoString *spec = AS_STRING(V_AS_OBJ(lpeek(0)));
        Value value = lpeek(1);
        SAVE();
        if (conversion)
        {
            value = conversion == 'r' ? valueToRepr(vm, value) : valueToStr(vm, value);
            if (V_IS_EMPTY(value))
                goto _runtime_error;
            sp[-2] = value; // keep it rooted
        }
        Value text = formatValueToString(vm, value, spec->value, spec->length);
        if (V_IS_EMPTY(text))
            goto _runtime_error;
        sp -= 2;
        lpush(text);
        DISPATCH();
    }
    OP_SET:
    {
        // [x0, ..., xn-1] -> [set]; elements stay rooted on the stack.
        u32 count = ReadByte();
        Value *items = sp - count;
        MyMoSet *set = newSet(vm);
        SAVE();
        for (u32 i = 0; i < count; i++)
            if (!setAdd(vm, set, items[i]))
                goto _runtime_error;
        sp = items;
        lpush(V_OBJ_VAL(AS_OBJECT(set)));
        DISPATCH();
    }
    OP_CALLKW:
    {
        GC_SAFEPOINT();
        u32 argc = ReadByte();
        int kwc = ReadByte();
        MyMoString *names[255];
        for (int i = 0; i < kwc; i++)
            names[i] = AS_STRING(V_AS_OBJ(frame->function->chunk->constants.values[ReadShort()]));
        Value calleeV = lpeek((int)argc);
        SAVE();
        if (!callWithKeywords(vm, calleeV, argc, kwc, names))
            goto _runtime_error;
        LOAD();
        DISPATCH();
    }
    OP_DUPUNDER:
    {
        Value b = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = b;
        lpush(b);
        DISPATCH();
    }
    OP_NIP:
    {
        sp[-2] = sp[-1];
        sp--;
        DISPATCH();
    }
    OP_GETG:
    {
        MyMoObject *name = ReadObject();
        Value value;
        if (!getEntryV(&vm->globals, name, &value) && !getEntryV(&vm->builtins, name, &value))
        {
            SAVE();
            runtimeError(vm, "NameError: Undefined variable '%s'.", AS_STRING(name)->value);
            goto _runtime_error;
        }
        lpush(value);
        DISPATCH();
    }
    OP_SETG:
    {
        MyMoObject *name = ReadObject();
        setEntryV(vm, &vm->globals, name, lpeek(0));
        DISPATCH();
    }
    OP_SETNL:
    {
        // Assign in the nearest enclosing function that has the variable
        // (as a local or a parameter).
        MyMoObject *name = ReadObject();
        MyMoString *key = AS_STRING(name);
        Value value = lpeek(0), unused;
        bool done = false;
        for (CallFrame *f = frame->function->frame; f && !done; f = f->function->frame)
        {
            if (getEntryV(&f->locals, name, &unused))
            {
                setEntryV(vm, &f->locals, name, value);
                done = true;
                break;
            }
            MyMoFunction *fn = f->function;
            if (fn->argc > CALLFRAME_ARGS_INLINE)
                continue;
            for (int i = 0; i < fn->argc; i++)
                if (fn->argv[i] && fn->argv[i]->length == key->length &&
                    memcmp(fn->argv[i]->value, key->value, (size_t)key->length) == 0)
                {
                    f->args[i] = value;
                    done = true;
                    break;
                }
        }
        if (!done)
        {
            SAVE();
            runtimeError(vm, "NameError: no enclosing variable '%s' for nonlocal", key->value);
            goto _runtime_error;
        }
        DISPATCH();
    }
    OP_LEXTEND:
    {
        // [list, iterable] -> [list]   (f(*xs) arguments)
        Value items = lpeek(0);
        MyMoList *list = AS_LIST(V_AS_OBJ(lpeek(1)));
        SAVE();
        if (!appendIterable(vm, "*", items, &list->values))
            goto _runtime_error;
        sp--;
        DISPATCH();
    }
    OP_DADD:
    {
        // [dict, key, value] -> [dict]   (f(name=v) with a splat call)
        Value value = lpeek(0);
        MyMoObject *key = V_AS_OBJ(lpeek(1));
        MyMoDict *dict = AS_DICT(V_AS_OBJ(lpeek(2)));
        Value unused;
        if (getEntryV(dict, key, &unused))
        {
            SAVE();
            runtimeError(vm, "TypeError: got multiple values for keyword argument '%s'", AS_STRING(key)->value);
            goto _runtime_error;
        }
        setEntryV(vm, dict, key, value);
        sp -= 2;
        DISPATCH();
    }
    OP_DMERGE:
    {
        // [dict, mapping] -> [dict]   (f(**d) arguments; keys must be strings)
        Value mapping = lpeek(0);
        MyMoDict *dict = AS_DICT(V_AS_OBJ(lpeek(1)));
        SAVE();
        if (!V_IS_OBJ_TYPE(mapping, OBJ_DICT))
        {
            runtimeError(vm, "TypeError: argument after ** must be a dict, not %s", valueTypeName(mapping));
            goto _runtime_error;
        }
        Entry *e;
        DICT_FOREACH(AS_DICT(V_AS_OBJ(mapping)), e)
        {
            Value unused;
            if (e->key->type != OBJ_STRING)
            {
                runtimeError(vm, "TypeError: keywords must be strings, not %s", getType(e->key));
                goto _runtime_error;
            }
            if (getEntryV(dict, e->key, &unused))
            {
                runtimeError(vm, "TypeError: got multiple values for keyword argument '%s'", AS_STRING(e->key)->value);
                goto _runtime_error;
            }
            setEntryV(vm, dict, e->key, e->value);
        }
        sp--;
        DISPATCH();
    }
    OP_CALLEX:
    {
        // [callee, positional list, keyword dict] -> [result]
        GC_SAFEPOINT();
        MyMoDict *keywords = AS_DICT(V_AS_OBJ(lpeek(0)));
        MyMoList *positional = AS_LIST(V_AS_OBJ(lpeek(1)));
        int n = positional->values.count, kwc = keywords->count;
        if (n + kwc > 255)
        {
            SAVE();
            runtimeError(vm, "TypeError: too many arguments (%d; at most 255)", n + kwc);
            goto _runtime_error;
        }
        sp -= 2; // the list and dict stay alive: no collection until the call
        Value calleeV = lpeek(0);
        for (int i = 0; i < n; i++)
            lpush(positional->values.values[i]);
        MyMoString *names[255];
        int k = 0;
        Entry *e;
        DICT_FOREACH(keywords, e)
        {
            names[k++] = AS_STRING(e->key);
            lpush(e->value);
        }
        SAVE();
        bool ok = kwc ? callWithKeywords(vm, calleeV, (u32)(n + kwc), kwc, names)
                      : caller(vm, V_AS_OBJ(calleeV), (u32)n);
        if (!ok)
            goto _runtime_error;
        LOAD();
        DISPATCH();
    }
    OP_PITHRU:
    {
        MyMoObject *fn = pop(vm);
        if (!(IS_FUNCTION(fn) || IS_CLASS(fn) || IS_BUILTIN_FUNCTION(fn) || IS_BUILTIN_METHOD(fn) || IS_BOUND_METHOD(fn) || IS_BUILTIN_CLASS(fn)))
        {
            runtimeError(vm, "TypeError: Cannot Pipe Through '%s'.", getType(fn));
            goto _runtime_error;
        }
        MyMoObject *arg = pop(vm);
        push(vm, fn);
        push(vm, arg);
        DISPATCH();
    }
    OP_RET:
    {
        if (vm->currentModule && vm->currentModule->parent)
        {
            // Copy the module's top-level names into both the module
            // object's `variables` (so callers can read them as
            // `mod.name`) and leave them in `frame->locals` — any
            // function the module exported via OP_FN captured this
            // exact `frame` for closure-style state lookup. Freeing
            // the locals here would dangle those captures.
            //
            // Keeping the frame alive (no free, no freeDict) leaks
            // exactly one CallFrame per imported module. Negligible
            // — modules don't churn — and worth the simplicity.
            copyDict(vm, &frame->locals, vm->currentModule->variables);
            vm->currentModule = vm->currentModule->parent;
            frame = vm->fiber->callFrames[--vm->fiber->frameCount];
            ip = frame->ip;
            DISPATCH();
        }
        SAVE();  // sync state out before returning from the run loop
        return OK;
    }
    OP_FRET:
    {
        // A `return` inside a try body skips OP_ENDTRY: drop the handlers
        // this frame installed so they can't leak (or catch errors
        // raised after we've left).
        while (vm->fiber->handlerCount > 0
               && vm->fiber->handlers[vm->fiber->handlerCount - 1].frameCount >= vm->fiber->frameCount)
            vm->fiber->handlerCount--;
        MyMoObject *ret = pop(vm);
        if (vm->classCall && frame->function->type == FN_INIT)
        {
            vm->classCall--;
            // `self` is conventionally argv[0]. With slot-based arg storage
            // it lives in frame->args[0] now (rather than the locals dict),
            // so read it directly from there. The dict lookup remains as a
            // fallback for the spill case (argc > CALLFRAME_ARGS_INLINE),
            // which __init__ will basically never hit.
            if (frame->function->argc <= CALLFRAME_ARGS_INLINE)
                ret = V_AS_OBJ(frame->args[0]);
            else
                ret = getEntry(vm, &frame->locals, AS_OBJECT(frame->function->argv[0]));
        }
        if (frame->calleeSlot)
            pop(vm);
        if (vm->fiber->frameCount == 0)
        {
            vm->fiber->state = FIBER_DEAD;
            // Sync our register state back to the dying child fiber, then
            // pivot to the parent fiber and refresh registers.
            SAVE();
            vm->fiber = vm->fiber->parent;
            LOAD();
            pop(vm);
            push(vm, ret);
            DISPATCH();
        }
        // Module frames must survive their own OP_FRET: any function
        // defined inside the module (via OP_FN) captured `frame` for
        // closure-style lookup of module-level state through
        // function->frame->locals. Recycling the frame would free
        // those locals and clobber frame->function (it's repurposed
        // as the pool's next-link), so later calls to exported
        // functions would crash. The same holds for any frame a closure
        // captured (see OP_FN). Those are retired: the GC frees them once
        // no function references them.
        if (frame->function->type == FN_MODULE || frame->captured)
            retireFrame(vm, frame);
        else
        {
            // Recycle into the fiber's frame pool instead of free()ing.
            // Skip freeDict when the locals dict was never grown
            // (count==0 with capacity==-1) — the common case for
            // arg-only functions where all params live in frame->args[].
            // Saves a function call per OP_FRET on the hot path.
            if (frame->locals.count > 0 || frame->locals.entries != NULL)
                freeDict(vm, &frame->locals);
            frame->function = (MyMoFunction *)vm->fiber->freeFramesHead;
            vm->fiber->freeFramesHead = frame;
        }
        // Frame transition: re-load ip from the new (caller) frame. Stack
        // top stays as our local sp.
        frame = vm->fiber->callFrames[--vm->fiber->frameCount];
        ip = frame->ip;
        push(vm, ret);
        // mymo_call boundary: the host-invoked frame just returned.
        if ((int)vm->fiber->frameCount == vm->exitFrame && vm->fiber == vm->exitFiber)
        {
            SAVE();
            return OK;
        }
        DISPATCH();
    }
    OP_CLASS:
    {
        MyMoString *name = AS_STRING(ReadObject());
        MyMoClass *klass = newClass(vm, name);
        klass->enclosing = vm->currentClass;
        vm->currentClass = klass;
        push(vm, AS_OBJECT(klass));
        DISPATCH();
    }
    OP_SUPERARGS:
    {
        // Compiler emits the superclass count as an inline V_INT_VAL.
        int argc = V_AS_INT(ReadConstant());
        for (size_t i = 0; i < argc; i++)
        {
            MyMoObject *superClass = pop(vm);
            if (IS_CLASS(superClass) || IS_BUILTIN_CLASS(superClass))
            {
                writeMyMoObjectArray(vm, &vm->currentClass->superClasses, superClass);
                if (IS_CLASS(superClass))
                {
                    copyDict(vm, AS_CLASS(superClass)->methods, vm->currentClass->methods);
                    copyDict(vm, AS_CLASS(superClass)->fields, vm->currentClass->fields);
                    copyDict(vm, AS_CLASS(superClass)->variables, vm->currentClass->variables);
                    vm->currentClass->init = AS_CLASS(superClass)->init;
                }
                else
                {
                    copyDict(vm, AS_BUILTIN_CLASS(superClass)->methods, vm->currentClass->methods);
                }
            }
            else
            {
                runtimeError(vm, "TypeError: only class can be inherated");
            }
        }
        DISPATCH();
    }
    OP_ENDCLASS:
    {
        vm->currentClass = vm->currentClass->enclosing;
        DISPATCH();
    }
    OP_SETP:
    {
        // [target, value] -> [value]; target.name = value.
        Value value = lpeek(0);
        Value targetV = lpeek(1);
        MyMoObject *name = ReadObject();
        MyMoDict *dest = NULL;
        if (V_IS_OBJ(targetV))
        {
            MyMoObject *target = V_AS_OBJ(targetV);
            switch (target->type)
            {
            case OBJ_INSTANCE: dest = AS_INSTANCE(target)->fields; break;
            case OBJ_CLASS:    dest = AS_CLASS(target)->variables; break;
            case OBJ_MODULE:   dest = AS_MODULE(target)->variables; break;
            // JS-style dot-write: `d.status = v` is `d["status"] = v`.
            case OBJ_DICT:     dest = AS_DICT(target); break;
            case OBJ_BUILTIN_CLASS:
                SAVE();
                runtimeError(vm, "TypeError: can't set attributes of built-in/extension type 'object'");
                goto _runtime_error;
            default: break;
            }
        }
        if (!dest)
        {
            SAVE();
            runtimeError(vm, "TypeError: Only classes and instances have properties.");
            goto _runtime_error;
        }
        setEntryV(vm, dest, name, value);
        sp -= 2;
        lpush(value);
        DISPATCH();
    }
    OP_AGETP:
    {
        agp = 1;
    }
    OP_GETP:
    {
        // Property read. Lookups use the Value API (no boxing); the
        // receiver stays on the stack when `agp` is set (compound
        // assignment reads it again for OP_SETP).
#define GETP_FOUND(v)                          \
        do {                                   \
            Value found_ = (v);                \
            if (!agp) lpop();                  \
            agp = 0;                           \
            pushV(vm, found_);                 \
            DISPATCH();                        \
        } while (0)
        Value recvV = lpeek(0);
        if (!V_IS_OBJ(recvV))
        {
            // Inline receiver: ints/doubles may have builtin methods; nil
            // and bool have none.
            MyMoObject *variable = ReadObject();
            MyMoObjectType t = valueLooksLikeInt(recvV) ? OBJ_INT
                             : valueLooksLikeDouble(recvV) ? OBJ_DOUBLE : OBJ_OBJECT;
            MyMoObject *fn = (t != OBJ_OBJECT && vm->builtInClasses[t])
                ? getEntry(vm, vm->builtInClasses[t]->methods, variable) : NULL;
            if (!fn)
            {
                SAVE();
                runtimeError(vm, "AttributeError: %s has no attribute '%s'.", valueTypeName(recvV), STRING_VAL(variable));
                goto _runtime_error;
            }
            MyMoBuiltInFunction *tmpl = AS_BUILTIN_FUNCTION(fn);
            MyMoBuiltInFunction *bound = newBuiltInFunction(vm, tmpl->name, tmpl->function, fn->type);
            bound->self = valueToBoxedObject(vm, recvV);
            lpop();
            agp = 0;
            pushV(vm, V_OBJ_VAL(AS_OBJECT(bound)));
            DISPATCH();
        }
        MyMoObject *recv = V_AS_OBJ(recvV);
        Value value;
        switch (recv->type)
        {
        case OBJ_INSTANCE:
        {
            MyMoInstance *instance = AS_INSTANCE(recv);
            MyMoObject *variable = ReadObject();
            if (getEntryV(instance->fields, variable, &value))
                GETP_FOUND(value);
            if (getEntryV(instance->klass->variables, variable, &value))
                GETP_FOUND(value);
            if (getEntryV(instance->klass->methods, variable, &value))
            {
                if (V_IS_OBJ_TYPE(value, OBJ_FUNCTION))
                {
                    MyMoBoundMethod *bound = newBoundMethod(vm, recv, AS_FUNCTION(V_AS_OBJ(value)));
                    value = V_OBJ_VAL(AS_OBJECT(bound));
                    setEntryV(vm, instance->fields, variable, value);
                }
                GETP_FOUND(value);
            }
            if (getEntryV(vm->builtInClasses[OBJ_OBJECT]->methods, variable, &value))
            {
                if (agp)
                {
                    SAVE();
                    runtimeError(vm, "TypeError: can't set attributes of built-in/extension type 'object'");
                    goto _runtime_error;
                }
                GETP_FOUND(value);
            }
            SAVE();
            runtimeError(vm, "AttributeError: undefined property '%s'", STRING_VAL(variable));
            goto _runtime_error;
        }
        case OBJ_SUPER:
        {
            MyMoSuper *super = AS_SUPER(recv);
            MyMoObject *variable = ReadObject();
            MyMoObject *method = getEntry(vm, super->klass->methods, variable);
            if (!method || agp)
            {
                SAVE();
                runtimeError(vm, agp ? "TypeError: can't assign through super()."
                                     : "AttributeError: parent class '%s' has no method '%s'.",
                             super->klass->name->value, STRING_VAL(variable));
                goto _runtime_error;
            }
            if (IS_FUNCTION(method))
            {
                // A parent __init__ reached through super() returns `self`
                // via OP_FRET's classCall path; balance the counter it
                // decrements so the child's own __init__ still does.
                if (AS_FUNCTION(method)->type == FN_INIT)
                    vm->classCall++;
                method = AS_OBJECT(newBoundMethod(vm, super->self, AS_FUNCTION(method)));
            }
            lpop(); // the super proxy
            pushV(vm, V_OBJ_VAL(method));
            DISPATCH();
        }
        case OBJ_CLASS:
        {
            MyMoClass *klass = AS_CLASS(recv);
            MyMoObject *variable = ReadObject();
            if (getEntryV(klass->variables, variable, &value))
                GETP_FOUND(value);
            if (getEntryV(klass->methods, variable, &value))
                GETP_FOUND(value);
            if (getEntryV(vm->builtInClasses[OBJ_OBJECT]->methods, variable, &value))
            {
                if (agp)
                {
                    SAVE();
                    runtimeError(vm, "TypeError: can't set attributes of built-in/extension type 'object'");
                    goto _runtime_error;
                }
                GETP_FOUND(value);
            }
            SAVE();
            runtimeError(vm, "AttributeError: undefined property '%s'", STRING_VAL(variable));
            goto _runtime_error;
        }
        case OBJ_MODULE:
        {
            MyMoModule *module = AS_MODULE(recv);
            MyMoObject *variable = ReadObject();
            if (getEntryV(module->variables, variable, &value))
                GETP_FOUND(value);
            SAVE();
            runtimeError(vm, "AttributeError: module '%s' has no attribute '%s'.", module->name->value, STRING_VAL(variable));
            goto _runtime_error;
        }
        case OBJ_BUILTIN_CLASS:
        {
            MyMoBuiltInClass *klass = AS_BUILTIN_CLASS(recv);
            MyMoObject *variable = ReadObject();
            if (getEntryV(klass->methods, variable, &value))
                GETP_FOUND(value);
            SAVE();
            runtimeError(vm, "AttributeError: built-in/extension type '%s' has no attribute '%s'.", klass->name->value, STRING_VAL(variable));
            goto _runtime_error;
        }
        case OBJ_DICT:
        {
            // JS-style dot-read: `r.status` is sugar for `r["status"]`;
            // otherwise a dict method, bound to this dict.
            MyMoDict *dict = AS_DICT(recv);
            MyMoObject *variable = ReadObject();
            if (getEntryV(dict, variable, &value))
                GETP_FOUND(value);
            if (vm->builtInClasses[OBJ_DICT])
            {
                MyMoObject *fn = getEntry(vm, vm->builtInClasses[OBJ_DICT]->methods, variable);
                if (fn)
                {
                    // A fresh bound copy per lookup: sharing the class's
                    // template method would alias its `self` field.
                    MyMoBuiltInFunction *tmpl = AS_BUILTIN_FUNCTION(fn);
                    MyMoBuiltInFunction *bound = newBuiltInFunction(vm, tmpl->name, tmpl->function, fn->type);
                    bound->self = recv;
                    lpop();
                    agp = 0;
                    pushV(vm, V_OBJ_VAL(AS_OBJECT(bound)));
                    DISPATCH();
                }
            }
            SAVE();
            runtimeError(vm, "KeyError: dict has no key '%s'.", STRING_VAL(variable));
            goto _runtime_error;
        }
        default:
        {
            MyMoObject *variable = ReadObject();
            // Types without a registered builtin class (functions,
            // modules, ...) have no methods; fall through to the error.
            MyMoObject *fn = vm->builtInClasses[recv->type]
                ? getEntry(vm, vm->builtInClasses[recv->type]->methods, variable)
                : NULL;
            if (fn)
            {
                // Fresh bound copy (see OBJ_DICT above).
                MyMoBuiltInFunction *tmpl = AS_BUILTIN_FUNCTION(fn);
                MyMoBuiltInFunction *bound = newBuiltInFunction(vm, tmpl->name, tmpl->function, fn->type);
                bound->self = recv;
                lpop();
                pushV(vm, V_OBJ_VAL(AS_OBJECT(bound)));
                DISPATCH();
            }
            SAVE();
            runtimeError(vm, "AttributeError: %s has no attribute '%s'.", getType(recv), STRING_VAL(variable));
            goto _runtime_error;
        }
        }
#undef GETP_FOUND
    }
    OP_OGETP:
    {
        // Optional chain `r?.prop`. Short-circuits to Nil in the two
        // cases where a normal `.` would explode for ergonomic use:
        //   1. receiver is Nil (classic JS `obj?.x` when obj is null)
        //   2. receiver is a dict and the key is missing (so users
        //      can chain through optional fields in JSON-shaped data
        //      without first guarding every step)
        // For instances / classes / modules / etc. it falls back to
        // the regular OP_GETP path — `?.` is not a blanket "make all
        // errors disappear" operator.
        Value recvV = lpeek(0);
        if (valueIsNil(recvV))
        {
            ReadObject(); // skip property name
            lpop();       // drop the Nil receiver
            lpush(V_NIL_VAL);
            DISPATCH();
        }
        if (V_IS_OBJ_TYPE(recvV, OBJ_DICT))
        {
            // Mirror OP_GETP's dict lookup chain: key first, then
            // built-in method fallback (with a fresh bound copy so
            // `self` aliasing doesn't bite). Difference from plain
            // `.` is that a final miss returns Nil instead of
            // raising. Matters for chains like `dict?.get(...)` —
            // the `?.` shouldn't suppress method dispatch, only
            // suppress the "missing" error.
            MyMoDict *dict = AS_DICT(V_AS_OBJ(recvV));
            MyMoObject *variable = ReadObject();
            Value value;
            if (getEntryV(dict, variable, &value))
            {
                lpop();
                lpush(value);
                DISPATCH();
            }
            if (vm->builtInClasses[OBJ_DICT])
            {
                MyMoObject *fn = getEntry(vm, vm->builtInClasses[OBJ_DICT]->methods, variable);
                if (fn)
                {
                    MyMoBuiltInFunction *tmpl = AS_BUILTIN_FUNCTION(fn);
                    MyMoBuiltInFunction *bound = newBuiltInFunction(
                        vm, tmpl->name, tmpl->function, fn->type);
                    bound->self = AS_OBJECT(dict);
                    lpop();
                    lpush(V_OBJ_VAL(AS_OBJECT(bound)));
                    DISPATCH();
                }
            }
            lpop();
            lpush(V_NIL_VAL);
            DISPATCH();
        }
        goto OP_GETP;
    }
    OP_IS:
    {
        // Python-style `is`: identity for objects, inline-tag
        // equality otherwise. Bit-pattern equality covers the inline
        // singletons (V_NIL_VAL, V_TRUE_VAL, V_FALSE_VAL, V_INT_VAL,
        // V_DOUBLE_VAL) and same-pointer object identity in one
        // check. Cross-form (inline tag vs boxed singleton) needs
        // explicit handling for nil and bool because the language
        // freely round-trips them through dict storage as heap
        // singletons — same mechanism the `==` operator uses via
        // valuesEqual's valueIsNilAny / valueIsBoolAny branches.
        // After step 1.5b deletes MyMoNil / MyMoBool the cross-form
        // branches become dead.
        u32 inplace = ReadByte();
        Value vb = popV(vm);
        Value va = popV(vm);
        bool result = (va == vb);
        if (!result)
        {
            bool a_nil = V_IS_NIL(va)
                || (V_IS_OBJ(va) && V_AS_OBJ(va) && V_AS_OBJ(va)->type == OBJ_NIL);
            bool b_nil = V_IS_NIL(vb)
                || (V_IS_OBJ(vb) && V_AS_OBJ(vb) && V_AS_OBJ(vb)->type == OBJ_NIL);
            if (a_nil && b_nil) { result = true; }
            else
            {
                int ab = V_IS_TRUE(va)  ? 1
                       : V_IS_FALSE(va) ? 0
                       : (V_IS_OBJ(va) && V_AS_OBJ(va) && V_AS_OBJ(va)->type == OBJ_BOOL)
                             ? (((MyMoBool *)V_AS_OBJ(va))->value ? 1 : 0)
                             : -1;
                int bb = V_IS_TRUE(vb)  ? 1
                       : V_IS_FALSE(vb) ? 0
                       : (V_IS_OBJ(vb) && V_AS_OBJ(vb) && V_AS_OBJ(vb)->type == OBJ_BOOL)
                             ? (((MyMoBool *)V_AS_OBJ(vb))->value ? 1 : 0)
                             : -1;
                if (ab >= 0 && ab == bb) result = true;
            }
        }
        if (inplace) result = !result;
        pushV(vm, V_BOOL_VAL(result));
        DISPATCH();
    }
    OP_LAPPEND:
    {
        // Stack-in:  [..., list, value]
        // Stack-out: [..., list]   (value appended; list stays on top)
        // Bypasses OP_GETP "append" because the built-in method dispatch
        // shares a single MyMoBuiltInFunction object across all receivers
        // and rebinds `self` on every lookup — nested list comprehensions
        // would all alias the same bound method, last writer wins. Direct
        // append avoids that whole hazard.
        Value v = popV(vm);
        MyMoObject *listObj = V_AS_OBJ(lpeek(0));
        if (listObj->type != OBJ_LIST)
        {
            runtimeError(vm, "OP_LAPPEND: expected list under value, got %s.", getType(listObj));
            goto _runtime_error;
        }
        writeValueArrayObject(vm, &AS_LIST(listObj)->values, valueToBoxedObject(vm, v));
        DISPATCH();
    }
    OP_TOSTRING:
    {
        // Pop a Value, push its str() form (f-string interpolation).
        // Strings pass through; instances may run their __str__.
        Value v = lpeek(0);
        if (V_IS_OBJ_TYPE(v, OBJ_STRING))
            DISPATCH();
        SAVE(); // __str__ runs on this stack, above v (which stays rooted)
        Value text = valueToStr(vm, v);
        if (V_IS_EMPTY(text))
            goto _runtime_error;
        lpop();
        lpush(text);
        DISPATCH();
    }
    OP_DELP:
    {
        // del obj.name: [obj] -> []
        MyMoObject *name = ReadObject();
        Value target = lpop();
        if (!V_IS_OBJ_TYPE(target, OBJ_INSTANCE) || !deleteEntry(vm, AS_INSTANCE(V_AS_OBJ(target))->fields, name))
        {
            SAVE();
            runtimeError(vm, "AttributeError: %s has no attribute '%s'", valueTypeName(target), AS_STRING(name)->value);
            goto _runtime_error;
        }
        DISPATCH();
    }
    OP_USE:
    {
        MyMoString *modulePathUse = AS_STRING(ReadObject());
        MyMoObject *isUse = pop(vm);
        MyMoString *moduleName = modulePathUse;
        char *path = pathResolver(vm, modulePathUse->value);
        if (path == NULL)
        {
            MyMoObject *module = getEntry(vm, &vm->builtInModules, AS_OBJECT(modulePathUse));
            if (module)
            {
                push(vm, module);
                if (AS_BOOL(isUse)->value)
                {
                    push(vm, AS_OBJECT(moduleName));
                }
                DISPATCH();
            }
            module = loadBuiltInModule(vm, modulePathUse);
            if (IS_EMPTY(module))
            {
                runtimeError(vm, "Module '%s' not found.", modulePathUse->value);
                goto _runtime_error;
            }
            push(vm, module);
            if (AS_BOOL(isUse)->value)
            {
                push(vm, AS_OBJECT(moduleName));
            }
            DISPATCH();
        }
        char *name = strrchr(modulePathUse->value, '/');
        if (name)
        {
            name++;
            moduleName = newString(vm, name, strlen(name));
        }
        MyMoString *modulePath;
        if (path[strlen(path) - 1] == 'c')
        {
            modulePath = newString(vm, path, strlen(path) - 1);
        }
        else
        {
            modulePath = newString(vm, path, strlen(path));
        }
        MyMoObject *module = getEntry(vm, &vm->modules, AS_OBJECT(modulePath));
        if (module)
        {
            push(vm, module);
            if (AS_BOOL(isUse)->value)
            {
                push(vm, AS_OBJECT(moduleName));
            }
            free(path);
            DISPATCH();
        }
        MyMoFunction *function = runFile(vm, path);
        if (function == NULL)
        {
            runtimeError(vm, "ImportError: syntax error in module '%s'.", path);
            free(path);
            goto _runtime_error;
        }
        free(path);
        function->name = moduleName;
        function->type = FN_MODULE;
        SAVE();
        callFunction(vm, function, 0, false); // module bodies have no callee slot
        LOAD();
        setEntry(vm, &frame->locals, NEW_STRING(vm, "__name__", 8), AS_OBJECT(moduleName));
        MyMoModule *currentModule = newModule(vm, moduleName, modulePath);
        currentModule->parent = vm->currentModule;
        vm->currentModule = currentModule;
        module = AS_OBJECT(currentModule);
        push(vm, module);
        if (AS_BOOL(isUse)->value)
        {
            push(vm, AS_OBJECT(AS_MODULE(module)->name));
        }
        DISPATCH();
    }
    OP_SETM:
    {
        MyMoObject *name = pop(vm);
        MyMoObject *value = pop(vm);
        if ((IS_FIBER_ROOT(vm->fiber)) && vm->fiber->frameCount == 0)
        {
            setEntry(vm, &vm->globals, name, value);
        }
        else
        {
            setEntry(vm, &frame->locals, name, value);
        }
        DISPATCH();
    }
    OP_COPY:
    {
        MyMoModule *module = AS_MODULE(pop(vm));
        copyDict(vm, module->variables, (((IS_FIBER_ROOT(vm->fiber)) && vm->fiber->frameCount == 0) ? &vm->globals : &frame->locals));
        DISPATCH();
    }
    OP_WILDCARD:
    {
        lpushObj(vm->wildcard);
        DISPATCH();
    }
    OP_GETARG:
    {
        // Direct array index into frame->args. ~3 instructions: load slot,
        // load Value, push. No dict probe, no IC, no name compare.
        u8 slot = ReadByte();
        lpush(frame->args[slot]);
        DISPATCH();
    }
    OP_SETARG:
    {
        u8 slot = ReadByte();
        frame->args[slot] = lpeek(0);
        DISPATCH();
    }
    OP_INVOKE_GLOBAL:
    {
        GC_SAFEPOINT();
        // Fused OP_GETV + OP_CALL. Layout: opcode | name_idx u8 | IC[8] | argc u8.
        // Pre-condition: argc args sit on the operand stack in call order.
        // Post-condition: args consumed, return value pushed.
        MyMoObject *variable = ReadObject();
        u8 *icp = ip;
        u8 ic_tag = icp[0];
        u16 ic_idx = (u16)icp[2] | ((u16)icp[3] << 8);
        u32 ic_ver = (u32)icp[4] | ((u32)icp[5] << 8) | ((u32)icp[6] << 16) | ((u32)icp[7] << 24);
        ip += IC_BYTES;
        u8 argCount = ReadByte();

        // IC fast path: globals dict cache hit.
        Value calleeV;
        bool found = false;
        if (ic_tag == IC_TAG_GLOBALS)
        {
            MyMoDict *d = &vm->globals;
            if (d->modifyCount == ic_ver && (int)ic_idx <= d->capacity
                && d->entries[ic_idx].key == variable)
            {
                calleeV = d->entries[ic_idx].value;
                found = true;
            }
        }
        else if (ic_tag == IC_TAG_LOCALS)
        {
            MyMoDict *d = &frame->locals;
            if (d->modifyCount == ic_ver && (int)ic_idx <= d->capacity
                && d->entries[ic_idx].key == variable)
            {
                calleeV = d->entries[ic_idx].value;
                found = true;
            }
        }

        // Slow path: walk the same lookup chain OP_GETV uses, fill cache.
        if (!found)
        {
            Value val;
            bool have = false;
            u8 hitTag = IC_TAG_COLD;
            MyMoDict *hitDict = NULL;
            if (vm->currentClass)
                have = getEntryV(vm->currentClass->variables, variable, &val)
                    || getEntryV(vm->currentClass->methods, variable, &val);
            if (!have)
            {
                if ((IS_FIBER_ROOT(vm->fiber)) && vm->fiber->frameCount == 0)
                {
                    if ((have = getEntryV(&vm->globals, variable, &val))) { hitTag = IC_TAG_GLOBALS; hitDict = &vm->globals; }
                }
                else
                {
                    if ((have = getEntryV(&frame->locals, variable, &val))) { hitTag = IC_TAG_LOCALS; hitDict = &frame->locals; }
                    if (!have)
                        have = lookupEnclosing(vm, frame->function->frame, variable, &val);
                    if (!have && (have = getEntryV(&vm->globals, variable, &val))) { hitTag = IC_TAG_GLOBALS; hitDict = &vm->globals; }
                }
                if (!have) have = getEntryV(&vm->builtins, variable, &val);
            }
            if (!have)
            {
                SAVE();
                runtimeError(vm, "NameError: Undefined variable '%s'.", STRING_VAL(variable));
                goto _runtime_error;
            }
            calleeV = val;
            // Refresh IC if this came from a cacheable dict.
            if (hitDict)
            {
                Entry *es = hitDict->entries;
                int cap = hitDict->capacity;
                if (es && cap >= 0)
                {
                    u32 h = variable->hash & (u32)cap;
                    while (es[h].key != variable) h = (h + 1) & (u32)cap;
                    icp[0] = hitTag;
                    icp[1] = 0;
                    icp[2] = (u8)(h & 0xff);
                    icp[3] = (u8)((h >> 8) & 0xff);
                    icp[4] = (u8)(hitDict->modifyCount & 0xff);
                    icp[5] = (u8)((hitDict->modifyCount >> 8) & 0xff);
                    icp[6] = (u8)((hitDict->modifyCount >> 16) & 0xff);
                    icp[7] = (u8)((hitDict->modifyCount >> 24) & 0xff);
                }
            }
        }

        // Specialized dispatch by callee type — bypass caller() and the
        // memmove for the two common cases. The args sit at the top of
        // the stack with no callee underneath (OP_INVOKE_GLOBAL fused
        // away the OP_GETV).

        MyMoObject *cobj = V_AS_OBJ(calleeV);

        // ── User function fast path ───────────────────────────────────
        // Pop args directly into newFrame->args[], then push calleeV so
        // OP_FRET's "pop callee" balances. Same end state as the
        // OP_GETV+OP_CALL path but skips the memmove + caller() chain.
        if (cobj->type == OBJ_FUNCTION && !AS_FUNCTION(cobj)->isargs)
        {
            MyMoFunction *function = AS_FUNCTION(cobj);
            {
                int fill = missingDefaults(function, argCount);
                if (fill < 0)
                {
                    SAVE();
                    arityError(vm, function, argCount);
                    goto _runtime_error;
                }
                for (int i = 0; i < fill; i++)
                    *sp++ = function->defaults[function->defaultCount - fill + i];
                argCount += fill;
            }
            if (function->type == FN_SCRIPT || function->type == FN_MODULE)
            {
                SAVE();
                runtimeError(vm, "TypeError: <%s '%s'> is not callable.",
                             function->type == FN_SCRIPT ? "Script" : "module",
                             function->name->value);
                goto _runtime_error;
            }
            if (STACK_EXHAUSTED(sp - vm->fiber->stack.values))
            {
                SAVE();
                runtimeError(vm, "RecursionError: maximum recursion depth exceeded.");
                goto _runtime_error;
            }
            if (vm->fiber->frameCapacity < (uint)(vm->fiber->frameCount + 2))
            {
                SAVE();
                u32 capacity = vm->fiber->frameCapacity;
                vm->fiber->frameCapacity = ResizeCapacity(vm->fiber->frameCapacity);
                while (vm->fiber->frameCapacity < (uint)(vm->fiber->frameCount + 2))
                    vm->fiber->frameCapacity = ResizeCapacity(vm->fiber->frameCapacity);
                vm->fiber->callFrames = ResizeArray(vm, CallFrame *, vm->fiber->callFrames, capacity, vm->fiber->frameCapacity);
            }
            CallFrame *newFrame;
            if (vm->fiber->freeFramesHead != NULL)
            {
                newFrame = vm->fiber->freeFramesHead;
                vm->fiber->freeFramesHead = (CallFrame *)newFrame->function;
            }
            else
            {
                newFrame = New(CallFrame, 1);
                initDict(&newFrame->locals);
                newFrame->gcEpoch = 0;
                newFrame->nextRetired = NULL;
            }
            newFrame->function = function;
            newFrame->captured = false;
            newFrame->calleeSlot = true;
            newFrame->ip = function->chunk->code;
            vm->fiber->callFrames[++vm->fiber->frameCount] = newFrame;
            if (argCount)
            {
                if ((int)argCount <= CALLFRAME_ARGS_INLINE)
                {
                    for (int i = (int)argCount - 1; i >= 0; i--)
                        newFrame->args[i] = *--sp;
                }
                else
                {
                    for (int i = (int)argCount - 1; i >= 0; i--)
                        setEntry(vm, &newFrame->locals, AS_OBJECT(function->argv[i]), V_AS_OBJ(*--sp));
                }
            }
            // Push callee so OP_FRET's "pop callee" branch (FN_FUNCTION>FN_METHOD)
            // has something to consume. One push, no shift, no malloc.
            lpush(calleeV);
            frame->ip = ip;
            frame = newFrame;
            ip = frame->ip;
            DISPATCH();
        }

        // ── Builtin function/method fast path ─────────────────────────
        // Materialize argv directly from the operand stack (no shift),
        // call the C function, push result. The builtin pops its argc
        // items via the legacy pop() — SAVE/LOAD syncs sp around it.
        if (cobj->type == OBJ_BUILTIN_FUNCTION || cobj->type == OBJ_BUILTIN_METHOD)
        {
            BuiltInfunction fn = AS_BUILTIN_FUNCTION(cobj)->function;
            // A copy, not a pointer into the stack: a builtin that calls back
        // into MyMo (mymo_call) pushes over these slots.
        Value argCopy[256];
            // Insert callee UNDER the args so any peek(argc) inside
            // the builtin (used by methods to find their `self`)
            // finds the right object. Without this insert, calling a
            // bound method indirectly via a local — e.g.
            //   a = xs.append
            //   a(3)
            // — would route through OP_INVOKE_GLOBAL's fused path
            // and the builtin would peek past the args into stale
            // stack memory, reading self=NULL.
            if (cobj->type == OBJ_BUILTIN_METHOD && argCount > 0)
            {
                memmove(sp - argCount + 1, sp - argCount, argCount * sizeof(Value));
                sp[-(int)argCount] = V_OBJ_VAL(cobj);
                sp++;
            }
            else if (cobj->type == OBJ_BUILTIN_METHOD)
            {
                *sp++ = V_OBJ_VAL(cobj);
            }
            SAVE();
            Value *vargs = vm->fiber->stack.values + vm->fiber->stack.count - argCount;
            memcpy(argCopy, vargs, sizeof(Value) * argCount);
            MyMoFiber *callerFiber = vm->fiber;
            Value result = fn(vm, argCount, argCopy);
            if (V_IS_EMPTY(result)) goto _runtime_error;
            if (vm->fiber != callerFiber)
            {
                // run/resume/yield switched fibers. Match caller()'s
                // convention: the fiber we left keeps a callee slot
                // (consumed when control returns to it) and the fiber
                // we entered gives up its pending one. Methods already
                // left their inserted callee behind; plain functions
                // (e.g. `yield()` via OP_INVOKE_GLOBAL) have none.
                if (cobj->type == OBJ_BUILTIN_FUNCTION)
                    callerFiber->stack.values[callerFiber->stack.count++] = V_OBJ_VAL(cobj);
                LOAD();
                sp--;
            }
            else
            {
                LOAD();   // builtin popped argc items via legacy pop()
                if (cobj->type == OBJ_BUILTIN_METHOD)
                {
                    // Pop the callee we inserted (still on the stack
                    // since the builtin only pops `argc` items).
                    sp--;
                }
            }
            *sp++ = result;
            DISPATCH();
        }

        // ── Slow path for class / bound method / etc. ─────────────────
        // Insert callee under args, fall back to caller() dispatch.
        if (argCount > 0)
        {
            memmove(sp - argCount + 1, sp - argCount, argCount * sizeof(Value));
        }
        sp[-(int)argCount] = calleeV;
        sp++;
        SAVE();
        if (!caller(vm, cobj, argCount))
            goto _runtime_error;
        LOAD();
        DISPATCH();
    }
    OP_INCR_VAR:
    {
        // Super-instruction: dict[name] += delta. Replaces
        // GETV+CONST+ADD+SETV+POP for `name += int_literal`.
        // Layout: opcode | name_idx | IC[8] | delta[4]
        MyMoObject *variable = ReadObject();
        u8 *icp = ip;
        u8 ic_tag = icp[0];
        u16 ic_idx = (u16)icp[2] | ((u16)icp[3] << 8);
        u32 ic_ver = (u32)icp[4] | ((u32)icp[5] << 8) | ((u32)icp[6] << 16) | ((u32)icp[7] << 24);
        ip += IC_BYTES;
        int32_t delta = (int32_t)((u32)ip[0] | ((u32)ip[1] << 8) | ((u32)ip[2] << 16) | ((u32)ip[3] << 24));
        ip += 4;

        // Resolve target dict (same logic as OP_SETV).
        MyMoDict *target = NULL;
        u8 hit_tag = IC_TAG_COLD;
        if (!vm->currentClass)
        {
            if (!vm->fiber->parent && vm->fiber->frameCount == 0)
            {
                target = &vm->globals; hit_tag = IC_TAG_GLOBALS;
            }
            else
            {
                target = &frame->locals; hit_tag = IC_TAG_LOCALS;
            }
        }
        else
        {
            target = vm->currentClass->variables;
        }

        // IC fast path.
        if (target && hit_tag != IC_TAG_COLD
            && ic_tag == hit_tag && target->modifyCount == ic_ver
            && (int)ic_idx <= target->capacity
            && target->entries[ic_idx].key == variable)
        {
            Value cur = target->entries[ic_idx].value;
            if (V_IS_INT(cur))
            {
                long sum = (long)V_AS_INT(cur) + (long)delta;
                if (sum >= INT32_MIN && sum <= INT32_MAX)
                {
                    Value next = V_INT_VAL((int32_t)sum);
                    target->entries[ic_idx].value = next;
                    lpush(next);
                    DISPATCH();
                }
                // Overflow: store as heap MyMoInt and push it.
                MyMoObject *boxed = AS_OBJECT(newInt(vm, sum));
                target->entries[ic_idx].value = V_OBJ_VAL(boxed);
                lpushObj(boxed);
                DISPATCH();
            }
            // Cur is a heap MyMoInt or other type — fall through to slow path.
        }
        // Slow path: do the read/add/write through the legacy dict API.
        Value cur;
        if (!getEntryV(target, variable, &cur))
        {
            SAVE();
            runtimeError(vm, "NameError: Undefined variable '%s'.", STRING_VAL(variable));
            goto _runtime_error;
        }
        Value next;
        long sum;
        if (valueLooksLikeInt(cur) && !__builtin_add_overflow(valueToLong(cur), (long)delta, &sum))
            next = sum >= INT32_MIN && sum <= INT32_MAX ? V_INT_VAL((int32_t)sum) : V_OBJ_VAL(AS_OBJECT(newInt(vm, sum)));
        else
        {
            // Doubles, bools, strings (TypeError), 64-bit overflow: the
            // general + rules.
            SAVE();
            if (V_IS_OBJ_TYPE(cur, OBJ_INSTANCE))
            {
                runtimeError(vm, "TypeError: `%s += %d` on an instance: write `%s = %s + %d`",
                             STRING_VAL(variable), delta, STRING_VAL(variable), STRING_VAL(variable), delta);
                goto _runtime_error;
            }
            next = addValues(vm, cur, V_INT_VAL(delta));
            if (V_IS_EMPTY(next))
                goto _runtime_error;
        }
        setEntryV(vm, target, variable, next);
        // Refresh cache.
        Entry *es = target->entries;
        int cap = target->capacity;
        if (es && cap >= 0 && hit_tag != IC_TAG_COLD)
        {
            u32 h = variable->hash & (u32)cap;
            while (es[h].key != variable) h = (h + 1) & (u32)cap;
            icp[0] = hit_tag;
            icp[1] = 0;
            icp[2] = (u8)(h & 0xff);
            icp[3] = (u8)((h >> 8) & 0xff);
            icp[4] = (u8)(target->modifyCount & 0xff);
            icp[5] = (u8)((target->modifyCount >> 8) & 0xff);
            icp[6] = (u8)((target->modifyCount >> 16) & 0xff);
            icp[7] = (u8)((target->modifyCount >> 24) & 0xff);
        }
        lpush(next);
        DISPATCH();
    }
    }

_runtime_error:
    // Uncaught in this fiber but caught by one that resumed it: the fiber
    // dies and the exception moves up to its resumer.
    while (!hasActiveHandler(vm) && vm->fiber->parent != NULL && vm->fiber != vm->exitFiber && handlerInChain(vm))
    {
        MyMoFiber *child = vm->fiber;
        MyMoObject *exc = child->exception;
        child->exception = NULL;
        child->state = FIBER_DEAD;
        child->handlerCount = 0;
        vm->fiber = child->parent;
        vm->fiber->exception = exc;
    }
    // Every per-handler error path inside the dispatch loop jumps
    // here. If there's an active `try` handler, unwind the fiber's
    // frames + operand stack to the saved state, push the raised
    // exception value (the message string, or whatever
    // OP_RAISE put on the fiber), and resume dispatching at the
    // catch arm. Otherwise propagate up to interpreter().
    if (hasActiveHandler(vm))
    {
        TryHandler h = vm->fiber->handlers[--vm->fiber->handlerCount];
        // Drop any frames pushed since the try-block started.
        while (vm->fiber->frameCount > h.frameCount)
        {
            CallFrame *dead = vm->fiber->callFrames[vm->fiber->frameCount--];
            if (dead->function->type == FN_MODULE || dead->captured)
                retireFrame(vm, dead);
            else
            {
                if (dead->locals.count > 0 || dead->locals.entries != NULL)
                    freeDict(vm, &dead->locals);
                dead->function = (MyMoFunction *)vm->fiber->freeFramesHead;
                vm->fiber->freeFramesHead = dead;
            }
        }
        vm->fiber->stack.count = h.stackCount;
        frame = vm->fiber->callFrames[vm->fiber->frameCount];
        frame->ip = h.handlerIp;
        // Re-load `ip` and `sp` from the unwound state. The catch
        // body opens with OP_SETV bound-name (compiler emits this)
        // so push the exception value first; if no value is in
        // flight (rare; only when a slow-path runtimeError fires
        // without OP_RAISE), use a Nil placeholder.
        MyMoObject *exc = vm->fiber->exception;
        vm->fiber->exception = NULL;
        ip = frame->ip;
        sp = vm->fiber->stack.values + vm->fiber->stack.count;
        lpush(exc ? objectToValue(exc) : V_NIL_VAL);
        DISPATCH();
    }
#undef ReadByte
#undef ReadConstant
#undef DISPATCH
#undef NUMBER_VAL
#undef BitwiseOp
#undef UnaryOp
#undef OperatorOverLoad
    // Only reached from _runtime_error with no handler to jump to.
    SAVE();
    return RUNTIME_ERROR;
}

// Drop every frame above `depth` on the current fiber (after an error
// escaped them), returning them to the frame pool or the GC.
void unwindFrames(MVM *vm, uint depth)
{
    MyMoFiber *fiber = vm->fiber;
    while (fiber->frameCount > depth)
    {
        CallFrame *dead = fiber->callFrames[fiber->frameCount--];
        if (dead->function->type == FN_MODULE || dead->captured)
            retireFrame(vm, dead);
        else
        {
            if (dead->locals.count > 0 || dead->locals.entries != NULL)
                freeDict(vm, &dead->locals);
            dead->function = (MyMoFunction *)fiber->freeFramesHead;
            fiber->freeFramesHead = dead;
        }
    }
    while (fiber->handlerCount > 0 && fiber->handlers[fiber->handlerCount - 1].frameCount > depth)
        fiber->handlerCount--;
}

I_Result interpreter(MVM *vm, MyMoFunction *main_)
{
    if (main_ == NULL)
        return COMPILE_ERROR;
    vm->fiber->callFrames[0]->function = main_;
    vm->fiber->callFrames[0]->ip = main_->chunk->code;
    // set __name__ to __main__
    MyMoObject *name = AS_OBJECT(newString(vm, "__name__", 8));
    MyMoObject *main = AS_OBJECT(newString(vm, "__main__", 8));
    setEntry(vm, &vm->globals, name, main);
    vm->runDepth++;
    I_Result i = runMVM(vm);
    vm->runDepth--;
    if (i == RUNTIME_ERROR)
    {
        // Leave the VM usable for the next script / REPL line: back on
        // the root fiber with an empty stack and no stale frames.
        vm->fiber = vm->rootFiber;
        unwindFrames(vm, 0);
        vm->fiber->stack.count = 0;
        vm->fiber->handlerCount = 0;
        vm->fiber->exception = NULL;
    }
    return i;
}
