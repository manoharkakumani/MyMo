#include "modules.h"

// Register every static built-in module at VM init. Adding a new C
// module that lives inside the mymo binary is two lines: declare its
// MODULE(...) in modules.h, then call its nameModule(vm) here.
//
// Dynamic .so/.dylib modules are loaded on demand by
// datatypes/module.c::loadBuiltInModule (via dlopen) and don't need to
// appear in this list.
void defineBuiltInModules(MVM *vm)
{
    mathModule(vm);
    timeModule(vm);
    osModule(vm);
    ioModule(vm);
    randomModule(vm);
    dateModule(vm);
    socketModule(vm);
    httpModule(vm);
}
