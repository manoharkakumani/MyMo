#include "../memory.h"
#include "object.h"
#include "datatypes.h"
#include "../vm.h"

void initMyMoObjectArray(MVM *vm, MyMoObjectArray *array)
{
    array->objects = NULL;
    array->capacity = 0;
    array->count = 0;
}

void writeMyMoObjectArray(MVM *vm, MyMoObjectArray *array, MyMoObject *object)
{
    if (array->capacity < array->count + 1)
    {
        int oldCapacity = array->capacity;
        array->capacity = ResizeCapacity(oldCapacity);
        array->objects = ResizeArray(vm, MyMoObject *, array->objects, oldCapacity, array->capacity);
    }
    array->objects[array->count] = object;
    array->count++;
}

void freeMyMoObjectArray(MVM *vm, MyMoObjectArray *array)
{
    FreeArray(vm, MyMoObject *, array->objects, array->capacity);
    initMyMoObjectArray(vm, array);
}

void printMyMoObjectArray(MyMoObjectArray *array){
    for (size_t i = 0; i < array->count; i++)
    {
        printObject(array->objects[i]);
        printf("\n");
    }
}

void printObjectList(MyMoObject *objects)
{
    MyMoObject *object = objects;
    while (object != NULL)
    {
        MyMoObject *next = object->next;
        printObject(object);
        printf(" --> %s\n", getType(object));
        object = next;
    }
}

void printObject(MyMoObject *object)
{
    if (object == NULL)
    {
        printf("NULL");
        return;
    }
    switch (object->type)
    {
    case OBJ_OBJECT:
    {
        printf("Empty");
        break;
    }
    case OBJ_NIL:
        printf("Nil");
        break;
    case OBJ_BOOL:
        printBool(AS_BOOL(object));
        break;
    case OBJ_INT:
        printInt(AS_INT(object));
        break;
    case OBJ_DOUBLE:
        printDouble(AS_DOUBLE(object));
        break;
    case OBJ_STRING:
        printString(AS_STRING(object));
        break;
    case OBJ_ARRAY:
        printf("Array");
        break;
    case OBJ_LIST:
        printList(AS_LIST(object));
        break;
    case OBJ_TUPLE:
        printTuple(AS_TUPLE(object));
        break;
    case OBJ_DICT:
        printDict(AS_DICT(object));
        break;
    case OBJ_FIBER:
        printFiber(AS_FIBER(object));
        break;
    case OBJ_FUNCTION:
        printFunction(AS_FUNCTION(object));
        break;
    case OBJ_CLOUSER:
        printClouser(AS_CLOUSER(object));
        break;
    case OBJ_BOUND_METHOD:
        printBoundMethod(AS_BOUND_METHOD(object));
        break;
    case OBJ_BUILTIN_FUNCTION:
        printBuiltInFunction(AS_BUILTIN_FUNCTION(object));
        break;
    case OBJ_BUILTIN_METHOD:
        printBuiltInMethod(AS_BUILTIN_FUNCTION(object));
        break;
    case OBJ_CLASS:
        printClass(AS_CLASS(object));
        break;
    case OBJ_BUILTIN_CLASS:
        printBuiltInClass(AS_BUILTIN_CLASS(object));
        break;
    case OBJ_INSTANCE:
        printInstance(AS_INSTANCE(object));
        break;
    case OBJ_MODULE:
        // printf("%s",AS_MODULE(object)->name->value);
        printModule(AS_MODULE(object));
        break;
    case OBJ_BUILTIN_MODULE:
        printf("BuiltinModule");
        break;
    case OBJ_CODE:
        printCode(AS_CODE(object));
        break;
    case OBJ_ITER:
        printIter(AS_ITER(object));
        break;
    case OBJ_WILDCARD:
        printf("_");
        break;
    default:
        printf("Unknown type %d", object->type);
        printf("%p", object);
        break;
    }
}

char *getType(MyMoObject *object)
{
    switch (object->type)
    {
    case OBJ_NIL:
        return "<object 'nil'>";
    case OBJ_BOOL:
        return "<object 'bool'>";
    case OBJ_INT:
        return "<object 'int'>";
    case OBJ_DOUBLE:
        return "<object 'double'>";
    case OBJ_STRING:
        return "<object 'string'>";
    case OBJ_LIST:
        return "<object 'list'>";
    case OBJ_TUPLE:
        return "<object 'tuple'>";
    case OBJ_DICT:
        return "<object 'dict'>";
    case OBJ_FIBER:
        return "<object 'fiber'>";
    case OBJ_FUNCTION:
        return "<object 'function'>";
    case OBJ_CLOUSER:
        return "<object 'clouser'>";
    case OBJ_BOUND_METHOD:
        return "<object 'bound method'>";
    case OBJ_BUILTIN_FUNCTION:
        return "<object 'builtin function'>";
    case OBJ_BUILTIN_METHOD:
        return "<object 'builtin method'>";
    case OBJ_CLASS:
        return "<object 'class'>";
    case OBJ_BUILTIN_CLASS:
        return "<object 'builtin class'>";
    case OBJ_INSTANCE:
        return "<object 'instance'>";
    case OBJ_MODULE:
        return "<object 'module'>";
    case OBJ_CODE:
        return "<object 'code'>";
    case OBJ_ITER:
        return "<object 'iter'>";
    default:
        return "<object 'unknown'>";
    }
}

void NIL_BOOL(MVM *vm)
{
    nil(vm);
    boolean(vm);
}

bool isEqual(MyMoObject *a, MyMoObject *b)
{
    // Wildcard sentinel matches any non-null operand. Symmetric, so case
    // patterns can put `_` on either side and either-direction comparison
    // (used by OP_CJMP / valuesEqual / dict findEntry — though only the
    // case-statement path should ever see WILDCARD in practice).
    if (a == NULL || b == NULL) return a == b;
    if (a->type == OBJ_WILDCARD || b->type == OBJ_WILDCARD) return true;
    if (a->type != b->type) return 0;
    switch (b->type)
    {
    case OBJ_NIL:
        return 0;
    case OBJ_INT:
        return AS_INT(a)->value == AS_INT(b)->value;
    case OBJ_DOUBLE:
        return AS_DOUBLE(a)->value == AS_DOUBLE(b)->value;
    case OBJ_TUPLE:
    {
        // Structural element-wise equality. Wildcard slots auto-match
        // (handled recursively by the WILDCARD branch above). Length
        // mismatch is a hard miss.
        MyMoTuple *ta = AS_TUPLE(a);
        MyMoTuple *tb = AS_TUPLE(b);
        if (ta->values.count != tb->values.count) return false;
        for (int i = 0; i < ta->values.count; i++)
        {
            if (!isEqual(ta->values.objects[i], tb->values.objects[i]))
                return false;
        }
        return true;
    }
    case OBJ_LIST:
    {
        MyMoList *la = AS_LIST(a);
        MyMoList *lb = AS_LIST(b);
        if (la->values.count != lb->values.count) return false;
        for (int i = 0; i < la->values.count; i++)
        {
            if (!isEqual(la->values.objects[i], lb->values.objects[i]))
                return false;
        }
        return true;
    }
    default:
        return a == b;
    }
}

MyMoObject *newObjectMethod(MVM *vm, uint argc, MyMoObject **argv)
{
    if (argc != 1)
    {
        runtimeError(vm, "__new__ method takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (!(IS_CLASS(peek(vm, 0))))
    {
        runtimeError(vm, "TypeError: object.__new__(X): X is not a type class");
        return NEW_EMPTY;
    }
    MyMoClass *klass = AS_CLASS(pop(vm));
    MyMoInstance *instance = newInstance(vm, klass);
    return AS_OBJECT(instance);
}

MyMoObject *strObjectMethod(MVM *vm, uint argc, MyMoObject **argv)
{
    UNUSED(vm);
    UNUSED(argv);
    if (argc != 1)
    {
        runtimeError(vm, "__str__ method takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    char *type = getType(pop(vm));
    return NEW_STRING(vm, type, strlen(type));
}

MyMoObject *initObjectMethod(MVM *vm, uint argc, MyMoObject **argv)
{
    UNUSED(vm);
    UNUSED(argv);
    if (!argc)
    {
        runtimeError(vm, "TypeError: descriptor '__init__' of 'object' object needs an argument");
        return NEW_EMPTY;
    }
    while (argc--)
    {
        pop(vm);
    }
    return NEW_NIL;
}

void defineObjectClassMethods(MVM *vm)
{
    defineMethod(vm, OBJ_OBJECT, "__new__", newObjectMethod);
    defineMethod(vm, OBJ_OBJECT, "__str__", strObjectMethod);
    defineMethod(vm, OBJ_OBJECT, "__init__", initObjectMethod);
}

void defineObjectClass(MVM *vm)
{
    MyMoObject *name = NEW_STRING(vm, "object", 6);
    MyMoBuiltInClass *object = newBuiltInClass(vm, AS_STRING(name));
    vm->builtInClasses[OBJ_OBJECT] = object;
    defineObjectClassMethods(vm);
    setEntry(vm, &vm->builtins, name, AS_OBJECT(object));
}

MyMoObject *getMethod(MVM *vm, MyMoObject *a, const char *name)
{
    switch (a->type)
    {
        case OBJ_INSTANCE:
        {
            MyMoObject *methodName = NEW_STRING(vm, name, strlen(name));
            MyMoObject *method = getEntry(vm, AS_INSTANCE(a)->fields, methodName);
            if (method)
            {
                return method;
            }
            method = getEntry(vm, AS_INSTANCE(a)->klass->methods, methodName);
            if (method)
            {
                return method;
            }
            return NEW_EMPTY;
        }
        case OBJ_CLASS:
        {
            MyMoObject *methodName = NEW_STRING(vm, name, strlen(name));
            MyMoObject *method = getEntry(vm, AS_CLASS(a)->methods, methodName);
            if (method)
            {
                return method;
            }
            return NEW_EMPTY;
        }
        case OBJ_LIST:
        case OBJ_TUPLE:
        case OBJ_DICT:
        {
            MyMoObject *methodName = NEW_STRING(vm, name, strlen(name));
            MyMoObject *method = getEntry(vm, vm->builtInClasses[a->type]->methods, methodName);
            if (method)
            {
                return method;
            }
            return NEW_EMPTY;
        }
        default:
            return NEW_EMPTY;
    }
}