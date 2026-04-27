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

typedef struct MyMoFunction
{
  MyMoObject object;
  FunctionType type;
  int argc;
  MyMoString *name;
  MyMoString *argv[256];
  MyMoDict *assiginedParameters;
  MyMoDict *variables;
  bool isargs;
  Chunk *chunk;
  struct CallFrame *frame;
  MyMoObject *klass;
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
    Value args[CALLFRAME_ARGS_INLINE];
};

typedef struct MyMoClouser
{
  MyMoObject object;
  MyMoFunction *function;
  MyMoDict *variables;
} MyMoClouser;

typedef MyMoObject *(*BuiltInfunction)(MVM *vm, uint argc, MyMoObject *argv[]);

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
MyMoClouser *newClouser(MVM *vm, MyMoFunction *function);
MyMoBoundMethod *newBoundMethod(MVM *vm, MyMoObject *self, MyMoFunction *method);

void defineMethod(MVM *vm, MyMoObjectType type, const char *name, BuiltInfunction function);

void printFunction(MyMoFunction *function);
void printClouser(MyMoClouser *clouser);
void printBuiltInFunction(MyMoBuiltInFunction *builtInFunction);
void printBuiltInMethod(MyMoBuiltInFunction *builtInFunction);
void printBoundMethod(MyMoBoundMethod *boundMethod);

#endif