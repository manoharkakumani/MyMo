#ifndef __OBJECT_H__
#define __OBJECT_H__

#include "../common.h"

#define AS_OBJECT(object) ((MyMoObject *)object)

#define IS_NUMBER(object) ((object)->type == OBJ_INT || (object)->type == OBJ_DOUBLE)
#define NUMBER_VAL(value) ((value->type == OBJ_INT) ? INT_VAL(value) : DOUBLE_VAL(value))

typedef enum
{
    OBJ_OBJECT,
    OBJ_NIL,
    OBJ_BOOL,
    OBJ_INT,
    OBJ_DOUBLE,
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_LIST,
    OBJ_DICT,
    OBJ_TUPLE,
    OBJ_FIBER,
    OBJ_FUNCTION,
    OBJ_CLOUSER,
    OBJ_BOUND_METHOD,
    OBJ_BUILTIN_FUNCTION,
    OBJ_BUILTIN_METHOD,
    OBJ_BUILTIN_MODULE,
    OBJ_CLASS,
    OBJ_BUILTIN_CLASS,
    OBJ_INSTANCE,
    OBJ_MODULE,
    OBJ_CODE,
    OBJ_ITER,
    OBJ_WILDCARD   // singleton sentinel for `_` in case patterns; matches anything in isEqual()
} MyMoObjectType;

typedef struct MyMoObject
{
    MyMoObjectType type;
    int refCount;
    struct MyMoObject *next;
    struct object *prev;
    u32 hash;
} MyMoObject;

typedef struct MyMoObjectArray
{
    MyMoObjectType type;
    int capacity;
    int count;
    MyMoObject **objects;
} MyMoObjectArray;

void initMyMoObjectArray(MVM *vm, MyMoObjectArray *array);
void writeMyMoObjectArray(MVM *vm, MyMoObjectArray *array, MyMoObject *object);
void printMyMoObjectArray(MyMoObjectArray *array);
void freeMyMoObjectArray(MVM *vm, MyMoObjectArray *array);

void NIL_BOOL(MVM *vm);

void printObject(MyMoObject *object);
void printObjectList(MyMoObject *objects);
char *getType(MyMoObject *object);

bool isEqual(MyMoObject *a, MyMoObject *b);

void defineObjectClass(MVM *vm);

MyMoObject *getMethod(MVM *vm, MyMoObject *a, const char *name);

#endif