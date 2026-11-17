#include "class.h"

#include "../vm.h"

MyMoClass *newClass(MVM *vm, MyMoString *name)
{
    MyMoClass *klass = AllocateObject(vm, MyMoClass, OBJ_CLASS);
    klass->type = CL_CLASS;
    klass->name = name;
    klass->methods = newDict(vm);
    klass->fields = newDict(vm);
    klass->variables = newDict(vm);
    initMyMoObjectArray(vm, &klass->superClasses);
    klass->init = NULL;
    klass->enclosing = NULL;
    return klass;
}

MyMoBuiltInClass *newBuiltInClass(MVM *vm, MyMoString *name)
{
    MyMoBuiltInClass *klass = AllocateObject(vm, MyMoBuiltInClass, OBJ_BUILTIN_CLASS);
    klass->name = name;
    klass->methods = newDict(vm);
    return klass;
}

MyMoInstance *newInstance(MVM *vm, MyMoClass *klass)
{
    MyMoInstance *instance = AllocateObject(vm, MyMoInstance, OBJ_INSTANCE);
    instance->klass = klass;
    instance->fields = newDict(vm);
    return instance;
}

void printClass(MyMoClass *klass)
{
    printf("<class %s at %p>", klass->name->value, klass);
}

void printBuiltInClass(MyMoBuiltInClass *klass)
{
    printf("<built-in class %s at %p>", klass->name->value, klass);
}

void printInstance(MyMoInstance *instance)
{
    printf("<instance of %s at %p>", instance->klass->name->value, instance);
}


MyMoSuper *newSuper(MVM *vm, MyMoObject *self, MyMoClass *klass)
{
    MyMoSuper *super = AllocateObject(vm, MyMoSuper, OBJ_SUPER);
    super->self = self;
    super->klass = klass;
    return super;
}
