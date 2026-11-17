#include "nil.h"
#include "../memory.h"
#include "../vm.h"

MyMoNil *NilObject;
MyMoEmpty *EmptyObject;

void printNil(MyMoObject *object)
{
    printf("Nil");
}

// Process-wide singletons, created once by the first VM (compile() spins
// up extra VMs; re-creating them there would let freeing that VM free the
// objects the main VM still uses).
void nil(MVM *vm)
{
    if (NilObject != NULL)
        return;
    NilObject = AllocateObject(vm, MyMoNil, OBJ_NIL);
    NilObject->value = false;
    EmptyObject = AllocateObject(vm, MyMoEmpty, OBJ_OBJECT);
}

void defineNilClass(MVM *vm)
{
    MyMoString *name = newString(vm, "Nil", 3);
    MyMoBuiltInClass *nilClass = newBuiltInClass(vm, name);
    vm->builtInClasses[OBJ_NIL] = nilClass;
    // defineNilMethods(vm);
}