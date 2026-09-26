#include "memory.h"
#include "vm.h"
#include "datatypes/datatypes.h"

// #define PRIME_CACHE_SIZE 8

// // Pre-calculated prime numbers
// const int primeCache[PRIME_CACHE_SIZE] = { 3, 13, 173, 347, 701, 1409, 45061, 90121};
// int getNewCapacity(int capacity)
// {
//     for (int i = 0; i < PRIME_CACHE_SIZE; i++)
//     {
//         if (primeCache[i] > capacity)
//         {
//             return primeCache[i];
//         }
//     }
//     return capacity;
// }

void *reallocate(MVM *vm, void *pointer, size_t oldSize, size_t newSize)
{
    if (newSize == 0)
    {
        free(pointer);
        return NULL;
    }
    return realloc(pointer, newSize);
}

MyMoObject *allocateObject(MVM *vm, size_t size, MyMoObjectType type)
{
    MyMoObject *object = (MyMoObject *)reallocate(vm, NULL, 0, size);
    // Zero the whole object: the GC traces every pointer field, so a
    // constructor that forgets one must leave NULL, not heap garbage.
    // This also clears the header's refCount, used as the GC mark bit.
    memset(object, 0, size);
    object->type = type;
    vm->objectCount++;
    object->next = vm->objects;
    vm->objects = object;
    return object;
}

void freeObject(MVM *vm, MyMoObject *object)
{
#ifdef DEBUG
    printf("%p free ", (void *)object);
    printObject(object);
    printf("\n");
#endif
    switch (object->type)
    {
    case OBJ_OBJECT:
    {
        Free(vm, MyMoEmpty, AS_EMPTY(object));
        break;
    }
    case OBJ_NIL:
    {
        Free(vm, MyMoNil, AS_NIL(object));
        break;
    }
    case OBJ_BOOL:
    {
        Free(vm, MyMoBool, AS_BOOL(object));
        break;
    }
    case OBJ_INT:
    {
        Free(vm, MyMoInt, AS_INT(object));
        break;
    }
    case OBJ_DOUBLE:
    {
        Free(vm, MyMoDouble, AS_DOUBLE(object));
        break;
    }
    case OBJ_STRING:
    {
        MyMoString *string = AS_STRING(object);
        FreeArray(vm, char, string->value, string->length + 1);
        Free(vm, MyMoString, object);
        break;
    }
    case OBJ_TUPLE:
    {
        freeMyMoObjectArray(vm, &AS_TUPLE(object)->values);
        Free(vm, MyMoTuple, object);
        break;
    }
    case OBJ_ITER:
    {
        Free(vm, MyMoIter, object);
        break;
    }
    case OBJ_CLOUSER:
    {
        Free(vm, MyMoClouser, object);
        break;
    }
    case OBJ_WILDCARD:
    {
        Free(vm, MyMoObject, object);
        break;
    }
    case OBJ_LIST:
    {
        MyMoList *list = AS_LIST(object);
        freeMyMoObjectArray(vm, &list->values);
        Free(vm, MyMoList, object);
        break;
    }
    case OBJ_DICT:
    {
        freeDictionary(vm, AS_DICT(object));
        break;
    }
    case OBJ_FIBER:
    {
        freeFiber(vm, AS_FIBER(object));
        break;
    }
    case OBJ_FUNCTION:
    {
        MyMoFunction *function = AS_FUNCTION(object);
        if (!function->proto) // closure copies share the prototype's chunk/argv
        {
            freeChunk(vm, function->chunk);
            free(function->argv);
        }
        Free(vm, MyMoFunction, object);
        break;
    }
    case OBJ_SUPER:
    {
        Free(vm, MyMoSuper, object);
        break;
    }
    case OBJ_BOUND_METHOD:
    {
        Free(vm, MyMoBoundMethod, object);
        break;
    }
    case OBJ_BUILTIN_FUNCTION:
    {
        Free(vm, OBJ_BUILTIN_FUNCTION, object);
        break;
    }
    case OBJ_BUILTIN_METHOD:
    {
        Free(vm, OBJ_BUILTIN_METHOD, object);
        break;
    }
    case OBJ_CLASS:
    {
        freeMyMoObjectArray(vm, &AS_CLASS(object)->superClasses);
        Free(vm, MyMoClass, AS_CLASS(object));
        break;
    }
    case OBJ_BUILTIN_CLASS:
    {
        Free(vm, MyMoBuiltInClass, AS_BUILTIN_CLASS(object));
        break;
    }
    case OBJ_INSTANCE:
    {
        Free(vm, MyMoInstance, AS_INSTANCE(object));
        break;
    }
    case OBJ_MODULE:
    {
        Free(vm, MyMoModule, AS_MODULE(object));
        break;
    }
    case OBJ_CODE:
    {
        freeCode(AS_CODE(object));
        break;
    }
    default:
        break;
    }
}

void freeObjects(MVM *vm)
{
    MyMoObject *object = vm->objects;
    while (object != NULL)
    {
        // // printObject(object);
        // printf("\n");
        MyMoObject *next = object->next;
        freeObject(vm, object);
        object = next;
    }
}