#include "common.h"
#include "vm.h"
#include "stack.h"
#include "operations.h"
#include "datatypes/datatypes.h"
#include "utils.h"
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
    vm->currentModule = NULL;
    vm->currentClass = NULL;
    vm->objectClass = NULL;
    vm->classCall = 0;
    vm->fiber = newFiber(vm, NULL);
    initDict(&vm->globals);
    initDict(&vm->builtins);
    initDict(&vm->strings);
    initDict(&vm->numbers);
    initDict(&vm->integers);
    initDict(&vm->doubles);
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
    freeDict(vm, &vm->modules);
    freeDict(vm, &vm->builtInModules);
    freeObjects(vm);
    free(vm);
}

void runtimeError(MVM *vm, const char *format, ...)
{
    printf("Traceback (most recent call last): \n");
    MyMoFiber *fiber = vm->fiber;
    while (fiber != NULL)
    {
        if (fiber->parent != NULL)
        {
            printf("  while running fiber of %s \n", fiber->callFrames[0]->function->name->value);
        }
        for (u32 i = 0; i <= fiber->frameCount; i++)
        {
            CallFrame *frame = fiber->callFrames[i];
            MyMoFunction *function = frame->function;
            size_t instruction = frame->ip - function->chunk->code - 1;
            fprintf(stderr, "  [%d : %d] in ",
                    function->chunk->lines[instruction], function->chunk->cols[instruction]);
            if (function->type == FN_MODULE)
            {
                fprintf(stderr, "<module %s>\n", function->name->value);
            }
            else
            {
                fprintf(stderr, "%s\n", function->name->value);
            }

            if (!fiber->parent && i == fiber->frameCount)
            {
                va_list args;
                va_start(args, format);
                vfprintf(stderr, format, args);
                va_end(args);
                fputs("\n", stderr);
            }
        }
        fiber = fiber->parent;
        // resetStack(vm);
    }
}

bool callFunction(MVM *vm, MyMoFunction *function, int argc)
{
    if (function->argc != argc)
    {
        runtimeError(vm, "TypeError : %s() Takes %d arguments but got %d.", function->name->value, function->argc, argc);
        return false;
    }
    else
    {
        // Off-by-one fix: callFrames is indexed up to and including
        // frameCount (slot 0 is the script/initial frame, slot frameCount
        // is the most recently pushed). We're about to write slot
        // frameCount+1, so we need capacity >= frameCount+2.
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
        }
        frame->function = function;
        frame->ip = function->chunk->code;
        vm->fiber->callFrames[++vm->fiber->frameCount] = frame;
        if (argc && !function->isargs)
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

bool caller(MVM *vm, MyMoObject *callee, u32 argc)
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
        MyMoObject *legacy_argv[256];
        Value *vargs = vm->fiber->stack.values + vm->fiber->stack.count - argc;
        for (u32 i = 0; i < argc; i++) legacy_argv[i] = valueToBoxedObject(vm, vargs[i]);
        MyMoObject *result = function(vm, argc, legacy_argv);
        if (IS_EMPTY(result))
            return false;
        pop(vm);
        push(vm, result);
        return true;
    }
    case OBJ_FUNCTION:
    {

        MyMoFunction *function = AS_FUNCTION(callee);
        if (function->type == FN_SCRIPT)
        {
            runtimeError(vm, "TypeError : <Script '%s'> is not callable.", function->name->value);
            return false;
        }
        else if (function->type == FN_GENERATOR || function->type == FN_GEN_METHOD)
        {
            runtimeError(vm, "TypeError : <generator '%s'> is not callable.", function->name->value);
            return false;
        }
        else if (function->type == FN_MODULE)
        {
            runtimeError(vm, "TypeError : <module '%s'> is not callable.", function->name->value);
            return false;
        }
        return callFunction(vm, function, argc);
    }
    case OBJ_CLASS:
    {
        MyMoObject *newMethed = NEW_STRING(vm, "__new__", 7);
        MyMoClass *klass = AS_CLASS(callee);
        MyMoObject *__new__ = getEntry(vm, vm->builtInClasses[OBJ_OBJECT]->methods, newMethed);
        BuiltInfunction function = AS_BUILTIN_FUNCTION(__new__)->function;
        push(vm, callee);
        MyMoObject *result = function(vm, 1, &callee);
        if (IS_EMPTY(result))
            return false;
        if (klass->init)
        {
            vm->classCall++;
            vm->fiber->stack.values[vm->fiber->stack.count - argc - 1] = V_OBJ_VAL(result);
            return caller(vm, klass->init, argc + 1);
        }
        else
        {
            if (argc)
            {
                runtimeError(vm, "TypeError : %s() Takes 0 arguments but got %d.", AS_CLASS(callee)->name->value, argc);
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
        MyMoObject *legacy_argv[256];
        Value *vargs = vm->fiber->stack.values + vm->fiber->stack.count - argc;
        for (u32 i = 0; i < argc; i++) legacy_argv[i] = valueToBoxedObject(vm, vargs[i]);
        MyMoObject *result = function(vm, argc, legacy_argv);
        if (IS_EMPTY(result))
            return false;
        pop(vm);
        push(vm, result);
        return true;
    }
    case OBJ_BOUND_METHOD:
    {
        MyMoBoundMethod *bound = AS_BOUND_METHOD(callee);
        vm->fiber->stack.values[vm->fiber->stack.count - argc - 1] = V_OBJ_VAL(bound->self);
        return caller(vm, AS_OBJECT(bound->method), argc + 1);
    }
    default:
        printObject(callee);
        runtimeError(vm, "Can only call functions and classes.");
        return false;
    }
}

#define BitwiseOp(a, b, op)                                             \
    do                                                                  \
    {                                                                   \
        if (!IS_INT(a) || !IS_INT(b))                                   \
        {                                                               \
            runtimeError(vm, "TypeError : Operands must be integers."); \
            return false;                                               \
        }                                                               \
        push(vm, NEW_INT(vm, INT_VAL(a) op INT_VAL(b)));                \
    } while (0);

#define UnaryOp(op, a)                                     \
    do                                                     \
    {                                                      \
        if (IS_INT(a))                                     \
            push(vm, NEW_INT(vm, op(INT_VAL(a))));         \
        else if (IS_DOUBLE(a))                             \
            push(vm, NEW_DOUBLE(vm, op(DOUBLE_VAL(a))));   \
        else                                               \
        {                                                  \
            runtimeError(vm, "Operand must be a number."); \
            return RUNTIME_ERROR;                          \
        }                                                  \
    } while (0);

#define OperatorOverLoad(a, b, op)                                                          \
    do                                                                                      \
    {                                                                                       \
        MyMoObject *method = getMethod(vm, a, op);                                          \
        if (IS_EMPTY(method))                                                               \
        {                                                                                   \
            runtimeError(vm, "MethodNotFound: %s does not have method %s", getType(a), op); \
            return RUNTIME_ERROR;                                                           \
        }                                                                                   \
        push(vm, method);                                                                   \
        push(vm, a);                                                                        \
        if (b != NULL)                                                                      \
            push(vm, b);                                                                    \
        SAVE();                                                                             \
        if (!caller(vm, method, b != NULL ? 2 : 1))                                         \
        {                                                                                   \
            return RUNTIME_ERROR;                                                           \
        }                                                                                   \
        LOAD();                                                                             \
        DISPATCH();                                                                         \
    } while (0)

bool isFalsey(MyMoObject *obj)
{
    return IS_NIL(obj) ||
           (IS_BOOL(obj) && !BOOL_VAL(obj)) ||
           (IS_INT(obj) && INT_VAL(obj) == 0) ||
           (IS_DOUBLE(obj) && DOUBLE_VAL(obj) == 0) ||
           (IS_STRING(obj) && STRING_VAL(obj)[0] == '\0');
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
#include "dispatch.h"
#define ReadByte()     (*ip++)
#define ReadShort()    (ip += 2, (u16)(ip[-2] << 8) | ip[-1])
#define ReadConstant() (frame->function->chunk->constants.values[ReadByte()])
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

// Legacy push/pop/peek calls inside dispatch handlers must use the local
// `sp`; redefine them as macros that override the global function names.
// The originals in stack.c remain used outside runMVM.
#define push(vm_, obj)  (lpushObj(obj))
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
            writeMyMoObjectArray(vm, &list->values, peek(vm, count - i - 1));
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
            writeMyMoObjectArray(vm, &tuple->values, peek(vm, count - i - 1));
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
        u32 count = ReadByte();
        MyMoDict *dict = newDict(vm);
        for (u32 i = 0; i < count; i++)
        {
            MyMoObject *value = pop(vm);
            MyMoObject *key = pop(vm);
            if (!IS_NIL(key) && !IS_BOOL(key) && !IS_INT(key) && !IS_DOUBLE(key) && !IS_STRING(key))
            {
                runtimeError(vm, "TypeError: Dictionary keys must be immutable.");
                return RUNTIME_ERROR;
            }
            setEntry(vm, dict, key, value);
        }
        push(vm, AS_OBJECT(dict));
        DISPATCH();
    }
    OP_SUBSCR:
    {
        MyMoObject *index = pop(vm);
        if (IS_DICT(peek(vm, 0)))
        {
            MyMoDict *dict = AS_DICT(pop(vm));
            MyMoObject *value = getEntry(vm, dict,index);
            if (!(value))
            {
                runtimeError(vm, "KeyError: value not found ");
                return RUNTIME_ERROR;
            }
            push(vm, value);
            DISPATCH();
        }
        if (!IS_INT(index))
        {
            runtimeError(vm, "TypeError: Indices must be integers.");
            return RUNTIME_ERROR;
        }
        int indexValue = INT_VAL(index);
        MyMoObject *object = pop(vm);
        switch (object->type)
        {
        case OBJ_STRING:
        {
            MyMoString *string = AS_STRING(object);
            if (indexValue >= string->length || -indexValue > string->length)
            {
                runtimeError(vm, "TypeError: String index out of bounds.");
                return RUNTIME_ERROR;
            }
            if (indexValue < 0)
            {
                indexValue += string->length;
            }
            push(vm, NEW_STRING(vm, &string->value[indexValue], 1));
            break;
        }
        case OBJ_LIST:
        {
            MyMoList *list = AS_LIST(object);
            if (indexValue >= list->values.count || -indexValue > list->values.count)
            {
                runtimeError(vm, "TypeError: List Index out of bounds.");
                return RUNTIME_ERROR;
            }
            if (indexValue < 0)
            {
                push(vm, list->values.objects[list->values.count + indexValue]);
            }
            else
            {
                push(vm, list->values.objects[indexValue]);
            }
            break;
        }
        case OBJ_TUPLE:
        {
            MyMoTuple *tuple = AS_TUPLE(object);
            if (indexValue >= tuple->values.count || -indexValue > tuple->values.count)
            {
                runtimeError(vm, "Tuple Index out of bounds.");
                return RUNTIME_ERROR;
            }
            if (indexValue < 0)
            {
                push(vm, tuple->values.objects[tuple->values.count + indexValue]);
            }
            else
            {
                push(vm, tuple->values.objects[indexValue]);
            }
            break;
        }
        default:
            runtimeError(vm, "TypeError: Index operator cannot  applied to %s", getType(object));
            return RUNTIME_ERROR;
        }
        DISPATCH();
    }
    OP_SETSUBSCR:
    {
        MyMoObject *value = pop(vm);
        MyMoObject *index = pop(vm);
        MyMoObject *object = pop(vm);
        if (IS_LIST(object))
        {
            if (!IS_INT(index))
            {
                runtimeError(vm, "TypeError: Indices must be integers.");
                return RUNTIME_ERROR;
            }
            int indexValue = INT_VAL(index);
            MyMoList *list = AS_LIST(object);
            if (indexValue >= list->values.count || -indexValue > list->values.count)
            {
                runtimeError(vm, "TypeError: List Index out of bounds.");
                return RUNTIME_ERROR;
            }
            if (indexValue < 0)
            {
                indexValue += list->values.count;
            }
            list->values.objects[indexValue] = value;
            push(vm, value);
            DISPATCH();
        }
        else if (IS_DICT(object))
        {
            MyMoDict *dict = AS_DICT(object);
            if (!IS_STRING(index) && !IS_INT(index) && !IS_BOOL(index) && !IS_NIL(index) && !IS_DOUBLE(index))
            {
                runtimeError(vm, "TypeError: Dictionary keys must be hashable.");
                return RUNTIME_ERROR;
            }
            setEntry(vm, dict, index, value);
            push(vm, value);
            DISPATCH();
        }
        else
        {
            runtimeError(vm, "TypeError: '%s' does not support item assignment.", getType(object));
            return RUNTIME_ERROR;
        }
    }
    OP_UNPACK:
    {
        // u32 count = ReadByte();
        // MyMoObject *object = pop(vm);
        // if (IS_LIST(object))
        // {
        //     MyMoList *list = AS_LIST(object);
        // }
        DISPATCH();
    }
    OP_SLICE:
    {
        MyMoObject *st = pop(vm);
        MyMoObject *ed = pop(vm);
        MyMoObject *sta = pop(vm);
        if (IS_NIL(st) && IS_NIL(ed) && IS_NIL(sta))
        {
            DISPATCH();
        }
        if ((!IS_INT(st) && !IS_NIL(st)) || (!IS_INT(ed) && !IS_NIL(ed)) || (!IS_INT(sta) && !IS_NIL(sta)))
        {
            runtimeError(vm, "TypeError: expect integer as index to slice");
            return RUNTIME_ERROR;
        }
        int step = IS_NIL(st) ? 1 : AS_INT(st)->value;
        if (step < 0)
        {
            st = sta;
            sta = ed;
            ed = st;
        }
        int start = IS_NIL(sta) ? 0 : AS_INT(sta)->value;
        if (!step)
        {
            runtimeError(vm, "TypeError: slice step cannot be zero");
            return RUNTIME_ERROR;
        }
        MyMoObject *object = pop(vm);
        switch (object->type)
        {
        case OBJ_STRING:
        {
            MyMoString *str = AS_STRING(object);
            char _str[str->length];
            int end = IS_NIL(ed) ? (step < 0 ? str->length - 1 : str->length) : AS_INT(ed)->value;
            int j = 0;
            if (start < 0)
            {
                start += str->length;
            }
            if (end < 0)
            {
                end += str->length;
            }
            if (end > str->length)
            {
                end = step < 0 ? str->length - 1 : str->length;
            }
            if (start > end || (start >= str->length && end > str->length) || start >= str->length)
            {
                push(vm, NEW_STRING(vm, "", 0));
                break;
            }
            if (step > 0)
            {
                for (int i = start; i < end; i += step)
                {
                    _str[j++] = str->value[i];
                }
                _str[j] = '\0';
                push(vm, NEW_STRING(vm, _str, j));
                break;
            }
            else
            {
                start = IS_NIL(sta) ? -1 : start;
                for (int i = end; i > start; i += step)
                {
                    _str[j++] = str->value[i];
                }
                _str[j] = '\0';
                push(vm, NEW_STRING(vm, _str, j));
                break;
            }
        }
        case OBJ_LIST:
        {
            MyMoList *lst = AS_LIST(object);
            MyMoList *list = newList(vm);
            int end = IS_NIL(ed) ? (step < 0 ? lst->values.count - 1 : lst->values.count) : AS_INT(ed)->value;
            if (start < 0)
            {
                start += lst->values.count;
            }
            if (end < 0)
            {
                end += lst->values.count;
            }
            else if (end > lst->values.count)
            {
                end = step < 0 ? lst->values.count - 1 : lst->values.count;
            }
            if (start > end || (start >= lst->values.count && end > lst->values.count) || start >= lst->values.count)
            {
                push(vm, AS_OBJECT(list));
                break;
            }
            if (step > 0)
            {
                for (int i = start; i < end; i += step)
                {
                    writeMyMoObjectArray(vm, &list->values, lst->values.objects[i]);
                }
                push(vm, AS_OBJECT(list));
                break;
            }
            else
            {
                start = IS_NIL(sta) ? -1 : start;
                for (int i = end; i > start; i += step)
                {
                    writeMyMoObjectArray(vm, &list->values, lst->values.objects[i]);
                }
                push(vm, AS_OBJECT(list));
                break;
            }
        }
        case OBJ_TUPLE:
        {
            MyMoTuple *tpl = AS_TUPLE(object);
            MyMoTuple *tuple = newTuple(vm);
            int end = IS_NIL(ed) ? (step < 0 ? tpl->values.count - 1 : tpl->values.count) : AS_INT(ed)->value;
            if (start < 0)
            {
                start += tpl->values.count;
            }
            if (end < 0)
            {
                end += tpl->values.count;
            }
            else if (end > tpl->values.count)
            {
                end = step < 0 ? tpl->values.count - 1 : tpl->values.count;
            }

            if (start > end || (start >= tpl->values.count && end > tpl->values.count) || start >= tpl->values.count)
            {
                push(vm, AS_OBJECT(tuple));
                break;
            }
            if (step > 0)
            {
                for (int i = start; i < end; i += step)
                {
                    writeMyMoObjectArray(vm, &tuple->values, tpl->values.objects[i]);
                }
                push(vm, AS_OBJECT(tuple));
                break;
            }
            else
            {
                start = IS_NIL(sta) ? -1 : start;
                for (int i = end; i > start; i += step)
                {
                    writeMyMoObjectArray(vm, &tuple->values, tpl->values.objects[i]);
                }
                push(vm, AS_OBJECT(tuple));
                break;
            }
        }
        default:
            runtimeError(vm, "TypeError: can only slice on List and String but got %s", getType(object));
            return RUNTIME_ERROR;
            break;
        }
        DISPATCH();
    }
    OP_NOT:
    {
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, NULL, "!");
        }
        push(vm, NEW_BOOL(isFalsey(a)));
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
        pop(vm);
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
            push(vm, NEW_BOOL(eq));
            DISPATCH();
        }
        // Slow path: instance with __eq__/__ne__ overload. Fall through to
        // the legacy heap-object dispatch.
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        MyMoObject *method = getMethod(vm, a, inplace ? "!=" : "==");
        if (IS_EMPTY(method))
        {
            push(vm, NEW_BOOL(isEqual(a, b)));
            DISPATCH();
        }
        if (inplace)
        {
            UNUSED(ReadByte());
        }
        push(vm, method);
        push(vm, a);
        push(vm, b);
        SAVE();
        if (!caller(vm, method, 2))
        {
            return RUNTIME_ERROR;
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
            lpushObj(NEW_BOOL(a > b));
            DISPATCH();
        }
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long a = valueToLong(va);
            long b = valueToLong(vb);
            sp -= 2;
            lpushObj(NEW_BOOL(a > b));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double a = valueAsNumber(va);
            double b = valueAsNumber(vb);
            sp -= 2;
            lpushObj(NEW_BOOL(a > b));
            DISPATCH();
        }
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            if (inplace) UNUSED(ReadByte());
            OperatorOverLoad(a, b, inplace ? "<=" : ">");
        }
        if (!IS_NUMBER(a) || !IS_NUMBER(b))
        {
            runtimeError(vm, "Operands must be numbers.");
            return RUNTIME_ERROR;
        }
        push(vm, NEW_BOOL(NUMBER_VAL(a) > NUMBER_VAL(b)));
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
            lpushObj(NEW_BOOL(a < b));
            DISPATCH();
        }
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long a = valueToLong(va);
            long b = valueToLong(vb);
            sp -= 2;
            lpushObj(NEW_BOOL(a < b));
            DISPATCH();
        }
        if (valueLooksLikeNumber(va) && valueLooksLikeNumber(vb))
        {
            double a = valueAsNumber(va);
            double b = valueAsNumber(vb);
            sp -= 2;
            lpushObj(NEW_BOOL(a < b));
            DISPATCH();
        }
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            if (inplace) UNUSED(ReadByte());
            OperatorOverLoad(a, b, inplace ? ">=" : "<");
        }
        if (!IS_NUMBER(a) || !IS_NUMBER(b))
        {
            runtimeError(vm, "Operands must be numbers.");
            return RUNTIME_ERROR;
        }
        push(vm, NEW_BOOL(NUMBER_VAL(a) < NUMBER_VAL(b)));
        DISPATCH();
    }
    OP_BAND:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "&=" : "&");
        }
        BitwiseOp(a, b, &);
        DISPATCH();
    }
    OP_BOR:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "|=" : "|");
        }
        BitwiseOp(a, b, |);
        DISPATCH();
    }
    OP_BXOR:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
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
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long r = valueToLong(va) + valueToLong(vb);
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
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "+=" : "+");
        }
        MyMoObject *result = addition(vm, a, b);
        if (IS_EMPTY(result))
            return RUNTIME_ERROR;
        push(vm, result);
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
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long r = valueToLong(va) - valueToLong(vb);
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
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "-=" : "-");
        }
        MyMoObject *result = subtraction(vm, a, b);
        if (IS_EMPTY(result))
            return RUNTIME_ERROR;
        push(vm, result);
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
        if (valueLooksLikeInt(va) && valueLooksLikeInt(vb))
        {
            long r = valueToLong(va) * valueToLong(vb);
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
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "*=" : "*");
        }
        MyMoObject *result = multiplication(vm, a, b);
        if (IS_EMPTY(result))
            return RUNTIME_ERROR;
        push(vm, result);
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
                return RUNTIME_ERROR;
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
                return RUNTIME_ERROR;
            }
            double r = valueAsNumber(va) / bn;
            sp -= 2;
            lpush(V_DOUBLE_VAL(r));
            DISPATCH();
        }
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "/=" : "/");
        }
        MyMoObject *result = division(vm, a, b);
        if (IS_EMPTY(result))
            return RUNTIME_ERROR;
        push(vm, result);
        DISPATCH();
    }
    OP_POW:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "**=" : "**");
        }
        if (!IS_NUMBER(a) || !IS_NUMBER(b))
        {
            runtimeError(vm, "Operands must be numbers.");
            return RUNTIME_ERROR;
        }
        double result = pow(NUMBER_VAL(a), NUMBER_VAL(b));
        // if (isInteger(result))
        // {
        //     push(vm, NEW_INT(vm, (long)result));
        // }
        // else
        {
            push(vm, NEW_DOUBLE(vm, result));
        }
        DISPATCH();
    }
    OP_MOD:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "%=" : "%");
        }
        if (!IS_NUMBER(a) || !IS_NUMBER(b))
        {
            runtimeError(vm, "Operands must be numbers.");
            return RUNTIME_ERROR;
        }
        double result = fmod(NUMBER_VAL(a), NUMBER_VAL(b));
        // if (isInteger(result))
        // {
        //     push(vm, NEW_INT(vm, (int)result));
        // }
        // else
        {
            push(vm, NEW_DOUBLE(vm, result));
        }
        DISPATCH();
    }
    OP_LSFT:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "<<=" : "<<");
        }
        BitwiseOp(a, b, <<);
        DISPATCH();
    }
    OP_RSFT:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? ">>=" : ">>");
        }
        BitwiseOp(a, b, >>);
        DISPATCH();
    }
    OP_IDIV:
    {
        u32 inplace = ReadByte();
        MyMoObject *b = pop(vm);
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, b, inplace ? "//=" : "//");
        }
        if (!IS_NUMBER(a) || !IS_NUMBER(b))
        {
            runtimeError(vm, "Operands must be numbers.");
            return RUNTIME_ERROR;
        }
        long int r = NUMBER_VAL(a) / NUMBER_VAL(b);
        push(vm, NEW_INT(vm, r));
        DISPATCH();
    }
    OP_NEG:
    {
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, NULL, "-@");
        }
        UnaryOp(-, a);
        DISPATCH();
    }
    OP_POS:
    {
        MyMoObject *a = pop(vm);
        if (IS_INSTANCE(a))
        {
            OperatorOverLoad(a, NULL, "+@");
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
        DISPATCH();
    }
    OP_ITER:
    {
        u16 offset = ReadShort();
        MyMoIter *iterator = AS_ITER(peek(vm, 0));
        MyMoObject *object = nextIter(vm, iterator);
        if (IS_EMPTY(object))
        {
            ip += offset;
        }
        else
        {
            push(vm, object);
        }
        DISPATCH();
    }
    OP_GETI:
    {
        MyMoObject *iterator = pop(vm);
        switch (iterator->type)
        {
        case OBJ_STRING:
        case OBJ_LIST:
        case OBJ_TUPLE:
            // case OBJ_DICT:
            // case OBJ_INSTANCE: TODO
            {
                push(vm, AS_OBJECT(newIter(vm, iterator)));
                DISPATCH();
            }
        default:
        {
            runtimeError(vm, "TypeError: cannot iterate on %s.", getType(iterator));
            return RUNTIME_ERROR;
        }
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
        MyMoObject *value = NULL;
        if (vm->currentClass)
        {
            value = getEntry(vm, vm->currentClass->variables, variable);
            if (value)
            {
                push(vm, value);
                DISPATCH();
            }
            value = getEntry(vm, vm->currentClass->methods, variable);
            if (value)
            {
                push(vm, value);
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
            value = getEntry(vm, &vm->globals, variable);
            if (value)
            {
                FILL_IC(IC_TAG_GLOBALS, &vm->globals, variable);
                push(vm, value);
                DISPATCH();
            }
            goto builtinvars;
        }
        else
        {
            value = getEntry(vm, &frame->locals, variable);
            if (value)
            {
                FILL_IC(IC_TAG_LOCALS, &frame->locals, variable);
                push(vm, value);
                DISPATCH();
            }
            CallFrame *parent = frame->function->frame;
            while (parent)
            {
                value = getEntry(vm, &parent->locals, variable);
                if (value)
                {
                    push(vm, value);
                    setEntry(vm, &frame->locals, variable, value);
                    DISPATCH();
                }
                parent = parent->function->frame;
            }
        }
        value = getEntry(vm, &vm->globals, variable);
        if (value)
        {
            FILL_IC(IC_TAG_GLOBALS, &vm->globals, variable);
            push(vm, value);
            DISPATCH();
        }
        else
        {
        builtinvars:
            value = getEntry(vm, &vm->builtins, variable);
            if (value)
            {
                push(vm, value);
                DISPATCH();
            }
        }
        runtimeError(vm, "Name Error: Undefined variable '%s'.", STRING_VAL(variable));
        return RUNTIME_ERROR;
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
        if (deleteEntry(vm, &frame->locals, variable))
        {
            DISPATCH();
        }
        runtimeError(vm, "Name Error: Undefined variable '%s'.", AS_STRING(variable)->value);
        return RUNTIME_ERROR;
    }
    OP_MET:
    {
        MyMoObject *method = ReadObject();
        MyMoObject *name = ReadObject();
        MyMoClass *klass = AS_CLASS(peek(vm, 0));
        if (memcmp(AS_STRING(name)->value, "__init__", 8) == 0)
        {
            klass->init = method;
        }
        setEntry(vm, klass->methods, name, method);
        AS_FUNCTION(method)->klass = AS_OBJECT(klass);
        DISPATCH();
    }
    OP_FN:
    {
        MyMoObject *function = ReadObject();
        AS_FUNCTION(function)->frame = frame;
        push(vm, function);
        DISPATCH();
    }
    OP_CALL:
    {
        u8 argCount = ReadByte();
        // Fast path: OBJ_FUNCTION call. Skips the caller()/callFunction()
        // indirection and inlines the frame setup. Covers the dominant
        // case for tight recursion (fib, fact, etc.). Falls back to the
        // generic caller() for builtins, classes, methods, bound methods.
        Value calleeV = lpeek((int)argCount);
        if (V_IS_OBJ(calleeV))
        {
            MyMoObject *cobj = V_AS_OBJ(calleeV);
            if (cobj->type == OBJ_FUNCTION)
            {
                MyMoFunction *function = AS_FUNCTION(cobj);
                if (function->argc != argCount)
                {
                    SAVE();
                    runtimeError(vm, "TypeError : %s() Takes %d arguments but got %d.",
                                 function->name->value, function->argc, argCount);
                    return RUNTIME_ERROR;
                }
                if (function->type == FN_SCRIPT || function->type == FN_GENERATOR
                    || function->type == FN_GEN_METHOD || function->type == FN_MODULE)
                {
                    SAVE();
                    runtimeError(vm, "TypeError : <%s '%s'> is not callable.",
                                 function->type == FN_SCRIPT ? "Script" :
                                 function->type == FN_MODULE ? "module" : "generator",
                                 function->name->value);
                    return RUNTIME_ERROR;
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
                }
                newFrame->function = function;
                newFrame->ip = function->chunk->code;
                vm->fiber->callFrames[++vm->fiber->frameCount] = newFrame;
                // Pop args from operand stack (using local sp register)
                // directly into the new frame's slot array. No dict.
                if (argCount && !function->isargs)
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
            return RUNTIME_ERROR;
        LOAD();
        DISPATCH();
    }
    OP_PITHRU:
    {
        MyMoObject *fn = pop(vm);
        if (!(IS_FUNCTION(fn) || IS_CLASS(fn) || IS_BUILTIN_FUNCTION(fn) || IS_BUILTIN_METHOD(fn) || IS_BOUND_METHOD(fn) || IS_BUILTIN_CLASS(fn)))
        {
            runtimeError(vm, "Type Error: Cannot Pipe Through '%s'.", getType(fn));
            return RUNTIME_ERROR;
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
            copyDict(vm, &frame->locals, vm->currentModule->variables);
            freeDict(vm, &frame->locals);
            free(frame);
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
        if (vm->currentClass || frame->function->type > FN_METHOD)
        {
            pop(vm);
        }
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
        // Recycle into the fiber's frame pool instead of free()ing. Skip
        // freeDict when the locals dict was never grown (count==0 with
        // capacity==-1) — the common case for arg-only functions where
        // all params live in frame->args[]. Saves a function call per
        // OP_FRET on the hot path.
        if (frame->locals.count > 0 || frame->locals.entries != NULL)
            freeDict(vm, &frame->locals);
        frame->function = (MyMoFunction *)vm->fiber->freeFramesHead;
        vm->fiber->freeFramesHead = frame;
        // Frame transition: re-load ip from the new (caller) frame. Stack
        // top stays as our local sp.
        frame = vm->fiber->callFrames[--vm->fiber->frameCount];
        ip = frame->ip;
        push(vm, ret);
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
        if (IS_INSTANCE(peek(vm, 1)))
        {
            MyMoInstance *instance = AS_INSTANCE(peek(vm, 1));
            setEntry(vm, instance->fields, ReadObject(), peek(vm, 0));
            MyMoObject *value = pop(vm);
            pop(vm);
            push(vm, value);
            DISPATCH();
        }
        else if (IS_CLASS(peek(vm, 1)))
        {
            MyMoClass *klass = AS_CLASS(peek(vm, 1));
            setEntry(vm, klass->variables, ReadObject(), peek(vm, 0));
            MyMoObject *value = pop(vm);
            pop(vm);
            push(vm, value);
            DISPATCH();
        }
        else if (IS_MODULE(peek(vm, 1)))
        {
            MyMoModule *module = AS_MODULE(peek(vm, 1));
            setEntry(vm, module->variables, ReadObject(), peek(vm, 0));
            MyMoObject *value = pop(vm);
            pop(vm);
            push(vm, value);
            DISPATCH();
        }
        else if (IS_DICT(peek(vm, 1)))
        {
            // JS-style dot-write: `d.status = v` is sugar for
            // `d["status"] = v`. The property name is already a
            // string in the constant pool.
            MyMoDict *dict = AS_DICT(peek(vm, 1));
            setEntry(vm, dict, ReadObject(), peek(vm, 0));
            MyMoObject *value = pop(vm);
            pop(vm);
            push(vm, value);
            DISPATCH();
        }
        else if (IS_BUILTIN_CLASS(peek(vm, 1)))
        {
            runtimeError(vm, "TypeError: can't set attributes of built-in/extension type 'object'");
            return RUNTIME_ERROR;
        }
        runtimeError(vm, "TypeError: Only classes and instances have properties.");
        return RUNTIME_ERROR;
    }
    OP_AGETP:
    {
        agp = 1;
    }
    OP_GETP:
    {
        MyMoObjectType type = peek(vm, 0)->type;
        switch (type)
        {
        case OBJ_INSTANCE:
        {
            MyMoInstance *instance = AS_INSTANCE(peek(vm, 0));
            MyMoObject *variable = ReadObject();
            MyMoObject *value = getEntry(vm, instance->fields, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the instance
                push(vm, value);
                if (agp)
                    agp = 0;
                DISPATCH();
            }
            value = getEntry(vm, instance->klass->variables, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the instance
                push(vm, value);
                if (agp)
                    agp = 0;
                DISPATCH();
            }
            value = getEntry(vm, instance->klass->methods, variable);
            if (value)
            {
                if (IS_FUNCTION(value))
                {
                    MyMoBoundMethod *bound = newBoundMethod(vm, peek(vm, 0), AS_FUNCTION(value));
                    setEntry(vm, instance->fields, variable, AS_OBJECT(bound));
                    value = AS_OBJECT(bound);
                }
                if (!agp)
                    pop(vm); // pop the instance
                push(vm, value);
                if (agp)
                    agp = 0;
                DISPATCH();
            }
            value = getEntry(vm, vm->builtInClasses[OBJ_OBJECT]->methods, variable);
            if (value)
            {
                pop(vm); // pop the instance
                push(vm, value);
                if (agp)
                {
                    runtimeError(vm, "TypeError: can't set attributes of built-in/extension type 'object'");
                    return RUNTIME_ERROR;
                }
                DISPATCH();
            }
            runtimeError(vm, "Undefined property '%s'.", STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        case OBJ_CLASS:
        {
            MyMoClass *klass = AS_CLASS(peek(vm, 0));
            MyMoObject *variable = ReadObject();
            MyMoObject *value = getEntry(vm, klass->variables, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the class
                if (agp)
                    agp = 0;
                push(vm, value);
                DISPATCH();
            }
            value = getEntry(vm, klass->methods, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the class
                if (agp)
                    agp = 0;
                push(vm, value);
                DISPATCH();
            }
            value = getEntry(vm, vm->builtInClasses[OBJ_OBJECT]->methods, variable);
            if (value)
            {
                pop(vm); // pop the instance
                push(vm, value);
                if (agp)
                {
                    runtimeError(vm, "TypeError: can't set attributes of built-in/extension type 'object'");
                }
                DISPATCH();
            }
            runtimeError(vm, "Undefined property '%s'.", STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        case OBJ_MODULE:
        {
            MyMoModule *module = AS_MODULE(peek(vm, 0));
            MyMoObject *variable = ReadObject();
            MyMoObject *value = getEntry(vm, module->variables, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the module
                if (agp)
                    agp = 0;
                push(vm, value);
                DISPATCH();
            }
            runtimeError(vm, "AttributeError: module '%s' has no attribute '%s'.", module->name->value, STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        case OBJ_BUILTIN_CLASS:
        {
            MyMoBuiltInClass *klass = AS_BUILTIN_CLASS(peek(vm, 0));
            MyMoObject *variable = ReadObject();
            MyMoObject *value = getEntry(vm, klass->methods, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the class
                if (agp)
                    agp = 0;
                push(vm, value);
                DISPATCH();
            }
            runtimeError(vm, "AttributeError: built-in/extension type '%s' has no attribute '%s'.", klass->name->value, STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        case OBJ_DICT:
        {
            // JS-style dot-read: `r.status` is sugar for `r["status"]`.
            // If a built-in dict class is registered, fall through to
            // its method table when no such key exists; otherwise
            // just error out cleanly (dict methods aren't currently
            // registered in vm->builtInClasses[OBJ_DICT]).
            MyMoDict *dict = AS_DICT(peek(vm, 0));
            MyMoObject *variable = ReadObject();
            MyMoObject *value = getEntry(vm, dict, variable);
            if (value)
            {
                if (!agp)
                    pop(vm); // pop the dict
                push(vm, value);
                if (agp)
                    agp = 0;
                DISPATCH();
            }
            if (vm->builtInClasses[OBJ_DICT])
            {
                MyMoObject *fn = getEntry(vm, vm->builtInClasses[OBJ_DICT]->methods, variable);
                if (fn)
                {
                    MyMoObject *self = pop(vm); // pop the dict
                    MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(fn);
                    function->self = self;
                    push(vm, fn);
                    if (agp) agp = 0;
                    DISPATCH();
                }
            }
            runtimeError(vm, "KeyError: dict has no key '%s'.", STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        default:
        {
            MyMoObject *self = pop(vm);
            MyMoObject *variable = ReadObject();
            MyMoObject *fn = getEntry(vm, vm->builtInClasses[type]->methods, variable);
            if (fn)
            {
                MyMoBuiltInFunction *function = AS_BUILTIN_FUNCTION(fn);
                function->self = self;
                push(vm, fn);
                DISPATCH();
            }
            runtimeError(vm, "AttributeError: %s has no attribute '%s'.", getType(self), STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        }
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
        MyMoObject *recv = peek(vm, 0);
        if (IS_NIL(recv))
        {
            ReadObject(); // skip property name
            pop(vm);      // drop the Nil receiver
            push(vm, NEW_NIL);
            DISPATCH();
        }
        if (recv->type == OBJ_DICT)
        {
            MyMoDict *dict = AS_DICT(recv);
            MyMoObject *variable = ReadObject();
            MyMoObject *value = getEntry(vm, dict, variable);
            pop(vm); // drop receiver
            push(vm, value ? value : NEW_NIL);
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
        push(vm, NEW_BOOL(result));
        DISPATCH();
    }
    OP_TOSTRING:
    {
        // Pop a Value, push its MyMoString representation. Used by
        // f-string interpolation so `f"x={x}"` works regardless of
        // x's type. Inline primitives format via snprintf; strings
        // pass through; everything else falls back to `getType()` so
        // we don't dereference arbitrary objects.
        Value v = popV(vm);
        char buf[64];
        const char *src = NULL;
        int srclen = 0;
        if (V_IS_INT(v))
        {
            srclen = snprintf(buf, sizeof(buf), "%d", V_AS_INT(v));
            src = buf;
        }
        else if (V_IS_DOUBLE(v))
        {
            srclen = snprintf(buf, sizeof(buf), "%g", V_AS_DOUBLE(v));
            src = buf;
        }
        else if (V_IS_NIL(v))   { src = "Nil"; srclen = 3; }
        else if (V_IS_TRUE(v))  { src = "True"; srclen = 4; }
        else if (V_IS_FALSE(v)) { src = "False"; srclen = 5; }
        else if (V_IS_OBJ(v))
        {
            MyMoObject *o = V_AS_OBJ(v);
            switch (o->type)
            {
            case OBJ_STRING:
            {
                MyMoString *s = AS_STRING(o);
                src = s->value; srclen = s->length;
                break;
            }
            case OBJ_INT:
                srclen = snprintf(buf, sizeof(buf), "%ld", AS_INT(o)->value);
                src = buf; break;
            case OBJ_DOUBLE:
                srclen = snprintf(buf, sizeof(buf), "%g", AS_DOUBLE(o)->value);
                src = buf; break;
            case OBJ_NIL:   src = "Nil";   srclen = 3; break;
            case OBJ_BOOL:
                if (AS_BOOL(o)->value) { src = "True";  srclen = 4; }
                else                   { src = "False"; srclen = 5; }
                break;
            default:
                src = getType(o);
                srclen = (int)strlen(src);
                break;
            }
        }
        else
        {
            src = "<unknown>"; srclen = 9;
        }
        push(vm, AS_OBJECT(newString(vm, src, srclen)));
        DISPATCH();
    }
    OP_DELP:
        DISPATCH();
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
                return RUNTIME_ERROR;
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
            runtimeError(vm, "Syntax error in module '%s'.", path);
            free(path);
            return RUNTIME_ERROR;
        }
        free(path);
        function->name = moduleName;
        function->type = FN_MODULE;
        SAVE();
        callFunction(vm, function, 0);
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
            MyMoObject *value = NULL;
            u8 hitTag = IC_TAG_COLD;
            MyMoDict *hitDict = NULL;
            if (vm->currentClass)
            {
                value = getEntry(vm, vm->currentClass->variables, variable);
                if (!value) value = getEntry(vm, vm->currentClass->methods, variable);
            }
            if (!value)
            {
                if ((IS_FIBER_ROOT(vm->fiber)) && vm->fiber->frameCount == 0)
                {
                    value = getEntry(vm, &vm->globals, variable);
                    if (value) { hitTag = IC_TAG_GLOBALS; hitDict = &vm->globals; }
                }
                else
                {
                    value = getEntry(vm, &frame->locals, variable);
                    if (value) { hitTag = IC_TAG_LOCALS; hitDict = &frame->locals; }
                    if (!value)
                    {
                        CallFrame *parent = frame->function->frame;
                        while (parent && !value)
                        {
                            value = getEntry(vm, &parent->locals, variable);
                            parent = parent->function->frame;
                        }
                    }
                    if (!value)
                    {
                        value = getEntry(vm, &vm->globals, variable);
                        if (value) { hitTag = IC_TAG_GLOBALS; hitDict = &vm->globals; }
                    }
                }
                if (!value) value = getEntry(vm, &vm->builtins, variable);
            }
            if (!value)
            {
                SAVE();
                runtimeError(vm, "Name Error: Undefined variable '%s'.", STRING_VAL(variable));
                return RUNTIME_ERROR;
            }
            calleeV = V_OBJ_VAL(value);
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
        if (cobj->type == OBJ_FUNCTION)
        {
            MyMoFunction *function = AS_FUNCTION(cobj);
            if (function->argc != argCount)
            {
                SAVE();
                runtimeError(vm, "TypeError : %s() Takes %d arguments but got %d.",
                             function->name->value, function->argc, argCount);
                return RUNTIME_ERROR;
            }
            if (function->type == FN_SCRIPT || function->type == FN_GENERATOR
                || function->type == FN_GEN_METHOD || function->type == FN_MODULE)
            {
                SAVE();
                runtimeError(vm, "TypeError : <%s '%s'> is not callable.",
                             function->type == FN_SCRIPT ? "Script" :
                             function->type == FN_MODULE ? "module" : "generator",
                             function->name->value);
                return RUNTIME_ERROR;
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
            }
            newFrame->function = function;
            newFrame->ip = function->chunk->code;
            vm->fiber->callFrames[++vm->fiber->frameCount] = newFrame;
            if (argCount && !function->isargs)
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
            MyMoObject *legacy_argv[256];
            SAVE();
            Value *vargs = vm->fiber->stack.values + vm->fiber->stack.count - argCount;
            for (u32 i = 0; i < argCount; i++)
                legacy_argv[i] = valueToBoxedObject(vm, vargs[i]);
            MyMoObject *result = fn(vm, argCount, legacy_argv);
            if (IS_EMPTY(result)) return RUNTIME_ERROR;
            LOAD();   // builtin popped argc items via legacy pop()
            lpushObj(result);
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
            return RUNTIME_ERROR;
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
        bool found = getEntryV(target, variable, &cur);
        long curLong = 0;
        if (found && valueLooksLikeInt(cur))
        {
            curLong = valueToLong(cur);
        }
        else if (!found)
        {
            // Treat as 0 if uninitialized — semantically wrong (should be
            // NameError) but matches what `0 + delta` would do for bare ints.
            // Fall back to error path: simulate the legacy GETV failure.
            runtimeError(vm, "Name Error: Undefined variable '%s'.", STRING_VAL(variable));
            return RUNTIME_ERROR;
        }
        else
        {
            // Non-int target: emit a TypeError. Could fall back to the slow
            // OP_ADD path, but += of a non-int with a literal int is rare.
            runtimeError(vm, "TypeError: cannot += int to %s.", valueTypeName(cur));
            return RUNTIME_ERROR;
        }
        long sum = curLong + (long)delta;
        Value next;
        if (sum >= INT32_MIN && sum <= INT32_MAX)
            next = V_INT_VAL((int32_t)sum);
        else
            next = V_OBJ_VAL(AS_OBJECT(newInt(vm, sum)));
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

#undef ReadByte
#undef ReadConstant
#undef DISPATCH
#undef NUMBER_VAL
#undef BitwiseOp
#undef UnaryOp
#undef OperatorOverLoad
    return OK;
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
    I_Result i = runMVM(vm);
    // free(frame);
    return i;
}
