#include "datatypes.h"
void defineBuiltInClasses(MVM *vm)
{
    NIL_BOOL(vm);
    defineObjectClass(vm);
    defineNilClass(vm);
    defineBoolClass(vm);
    defineIntClass(vm);
    defineDoubleClass(vm);
    defineStringClass(vm);
    defineListClass(vm);
    defineFiberClass(vm);
    // defineTupleClass(vm);
    defineDictClass(vm);
    // defineFunctionClass(vm);
    // defineClassClass(vm);
    // defineModuleClass(vm);
}