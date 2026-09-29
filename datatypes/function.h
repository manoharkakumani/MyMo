#ifndef __FUNCTION_H__
#define __FUNCTION_H__

#include "object.h"
#include "string.h"
#include "../chunk.h"
#include "dict.h"

#define AS_FUNCTION(object) ((MyMoFunction *)object)
#define IS_FUNCTION(object) (object->type == OBJ_FUNCTION)

#define AS_BUILTIN_FUNCTION(object) ((MyMoBuiltInFunction *)object)
#define IS_BUILTIN_FUNCTION(object) (object->type == OBJ_BUILTIN_FUNCTION)

#define AS_BUILDIN_METHOD(object) ((MyMoBuiltInFunction *)object)
#define IS_BUILTIN_METHOD(object) (object->type == OBJ_BUILTIN_METHOD)

#define AS_CLOUSER(object) ((MyMoClouser *)object)
#define IS_CLOUSER(object) (object->type == OBJ_CLOUSER)

#define AS_BOUND_METHOD(object) ((MyMoBoundMethod *)object)
#define IS_BOUND_METHOD(object) (object->type == OBJ_BOUND_METHOD)


typedef struct CallFrame CallFrame;

typedef enum
{
  FN_INIT,
  FN_OPERATOR,
  FN_GEN_METHOD,
  FN_METHOD,
  FN_FUNCTION,
  FN_GENERATOR,
  FN_ARROWFN,
  FN_MODULE,
  FN_COMPILED,
  FN_SCRIPT
} FunctionType;

#define VARARGS_REST 1
#define VARARGS_KW 2
// Has keyword-only parameters (after `*rest` or a bare `*`). Only set so
// calls skip the fast paths; the count is MyMoFunction.kwonly.
#define VARARGS_KWONLY 4
// Parameters before *rest / **kw.
#define FIXED_PARAMS(fn) ((fn)->argc - (((fn)->isargs & VARARGS_REST) != 0) - (((fn)->isargs & VARARGS_KW) != 0))

typedef struct MyMoFunction
{
  MyMoObject object;
  FunctionType type;
  int argc;
  MyMoString *name;
  MyMoString **argv; // 256 parameter-name slots, owned by the prototype
  MyMoDict *assiginedParameters;
  MyMoDict *variables;
  // VARARGS_* flags: the function ends with a `*rest` parameter (a tuple
  // of the extra positional arguments) and/or a `**kw` parameter (a dict
  // of the extra keyword arguments), in that order.
  u8 isargs;
  // Keyword-only parameters: the last `kwonly` of the FIXED_PARAMS. They
  // are declared after *rest in the source but stored before it, so the
  // frame is always [positional..., kwonly..., rest, kw].
  u8 kwonly;
  // A trailing block (`card:` + indented lines, `button "x": stmt`): a
  // closure whose assignments go to the enclosing scope (OP_SETB) and
  // which pins enclosing loop variables in `bound` (OP_BLOCKVAR).
  bool block;
  // A block's pinned loop variables (NULL if none); checked right after
  // the block's own locals.
  struct MyMoDict *bound;
  Chunk *chunk;
  // Defining frame, walked for free-variable lookup (see OP_FN).
  struct CallFrame *frame;
  MyMoObject *klass;
  // Non-NULL for a per-closure copy made by OP_FN; the copy shares the
  // prototype's chunk/argv, so only the prototype frees them.
  struct MyMoFunction *proto;
  // Default values for the last `defaultCount` parameters, evaluated when
  // the `fn` statement runs (OP_DEFAULTS). V_EMPTY_VAL marks a required
  // keyword-only parameter that follows one with a default. Owned by each function object
  // (closure copies get their own copy).
  int defaultCount;
  Value *defaults;
} MyMoFunction;

// Function arg slots — covers up to 8 parameters with direct array access.
// Keeps CallFrame small (≈100 B) so malloc/free per call stays cheap. The
// vast majority of functions take ≤4 args; >8 spills to the locals dict.
// Bumped opportunistically if profiling justifies the larger frame.
#define CALLFRAME_ARGS_INLINE 8

struct CallFrame {
    MyMoFunction *function;
    MyMoDict locals;
    u8 *ip;
    // Set when a closure was created in this frame. A captured frame
    // outlives its call (never recycled into the frame pool) because the
    // closure still reads its locals/args after it returns.
    bool captured;
    // True when the callee value sits on the stack below the arguments
    // (a normal call), so OP_FRET must pop it. False when that slot was
    // reused for `self` (bound methods, constructors) or never pushed
    // (module bodies). Decided at the call site, not by function type:
    // the same function can be called either way.
    bool calleeSlot;
    // GC bookkeeping: epoch of the last collection that reached this
    // frame, and the link on vm->retiredFrames once it left the stack.
    u32 gcEpoch;
    struct CallFrame *nextRetired;
    Value args[CALLFRAME_ARGS_INLINE];
};

typedef struct MyMoClouser
{
  MyMoObject object;
  MyMoFunction *function;
  MyMoDict *variables;
} MyMoClouser;

// Builtins return a Value so nil/bool/int results stay inline on the
// operand stack. V_EMPTY_VAL signals "runtimeError already raised".
typedef Value (*BuiltInfunction)(MVM *vm, uint argc, Value argv[]);

typedef struct MyMoBuiltInFunction
{
  MyMoObject object;
  MyMoString *name;
  MyMoObject *self;
  BuiltInfunction function;
} MyMoBuiltInFunction;

typedef struct MyMoBoundMethod
{
  MyMoObject object;
  MyMoObject *self;
  MyMoFunction *method;
} MyMoBoundMethod;


MyMoBuiltInFunction *newBuiltInFunction(MVM *vm, MyMoString *name, BuiltInfunction function, MyMoObjectType type);

MyMoFunction *newFunction(MVM *vm);
MyMoFunction *cloneFunction(MVM *vm, MyMoFunction *proto);
MyMoClouser *newClouser(MVM *vm, MyMoFunction *function);
MyMoBoundMethod *newBoundMethod(MVM *vm, MyMoObject *self, MyMoFunction *method);

void defineMethod(MVM *vm, MyMoObjectType type, const char *name, BuiltInfunction function);

// For builtin methods: validate arity (min..max args), fetch the bound
// receiver from the callee slot under the args, and pop the args (they
// stay readable through argv[]). Returns NULL after raising on error.
MyMoObject *methodEnter(MVM *vm, const char *name, uint argc, uint min, uint max);

// Calls may omit trailing parameters that have defaults. Returns how many
// defaults the caller must push (0 if every argument was given), or -1
// if `argc` is out of range (then raise with arityError).
int missingDefaults(MyMoFunction *function, int argc);
void arityError(MVM *vm, MyMoFunction *function, int argc);

void printFunction(MyMoFunction *function);
void printClouser(MyMoClouser *clouser);
void printBuiltInFunction(MyMoBuiltInFunction *builtInFunction);
void printBuiltInMethod(MyMoBuiltInFunction *builtInFunction);
void printBoundMethod(MyMoBoundMethod *boundMethod);

#endif