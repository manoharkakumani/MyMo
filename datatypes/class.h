#ifndef __CLASS_H__
#define __CLASS_H__

#include "object.h"
#include "dict.h"
#include "string.h"

#define AS_CLASS(object) ((MyMoClass *)object)
#define IS_CLASS(object) ((object)->type == OBJ_CLASS)

#define AS_BUILTIN_CLASS(object) ((MyMoBuiltInClass *)object)
#define IS_BUILTIN_CLASS(object) ((object)->type == OBJ_BUILTIN_CLASS)

#define AS_INSTANCE(object) ((MyMoInstance *)object)
#define IS_INSTANCE(object) ((object)->type == OBJ_INSTANCE)

typedef enum
{
    CL_CLASS,
    CL_BUILTIN
} classType;

typedef struct MyMoClass
{
    MyMoObject object;
    classType type;
    MyMoString *name;
    MyMoDict *methods;
    MyMoDict *fields;
    MyMoDict *variables;
    MyMoObject *init;
    MyMoObjectArray superClasses;
    struct MyMoClass *enclosing;
} MyMoClass;

typedef struct MyMoBuiltInClass
{
    MyMoObject object;
    MyMoString *name;
    MyMoDict *methods;
} MyMoBuiltInClass;

// Result of `super()` inside a method: attribute lookup resolves in
// `klass` (the parent of the method's defining class) and binds `self`.
typedef struct MyMoSuper
{
    MyMoObject object;
    MyMoObject *self;
    MyMoClass *klass;
} MyMoSuper;

#define AS_SUPER(object) ((MyMoSuper *)object)

typedef struct MyMoInstance
{
    MyMoObject object;
    MyMoClass *klass;
    MyMoDict *fields;
} MyMoInstance;

MyMoClass *newClass(MVM *vm, MyMoString *name);
MyMoInstance *newInstance(MVM *vm, MyMoClass *klass);
MyMoBuiltInClass *newBuiltInClass(MVM *vm, MyMoString *name);
MyMoSuper *newSuper(MVM *vm, MyMoObject *self, MyMoClass *klass);

void printClass(MyMoClass *klass);
void printInstance(MyMoInstance *instance);
void printBuiltInClass(MyMoBuiltInClass *klass);

#endif