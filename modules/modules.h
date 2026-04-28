#ifndef __MODULES_H__
#define __MODULES_H__

#include "../datatypes/datatypes.h"
#include "../vm.h"

// Each MODULE(name) declares an exported function called nameModule(vm)
// that returns a registered MyMoObject*. Static built-in modules listed
// below are linked into the mymo binary; defineBuiltInModules() in
// modules.c invokes each so they end up in vm->builtInModules at startup.
MODULE(math);
MODULE(time);
MODULE(os);
MODULE(io);
MODULE(random);
MODULE(date);
MODULE(socket);
MODULE(http);
MODULE(json);
MODULE(sqlite);
MODULE(server);
MODULE(nodes);

void defineBuiltInModules(MVM *vm);

#endif