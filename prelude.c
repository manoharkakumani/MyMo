// prelude.c — MyMo code run when a VM starts: the built-in exception
// classes. They are moved from the globals into the builtins, so they
// behave like print or len (visible everywhere, shadowable).

#include "prelude.h"
#include "compiler.h"
#include "vm.h"
#include "datatypes/datatypes.h"
#include <stdio.h>
#include <string.h>

static const char *PRELUDE =
    "class Exception:\n"
    "    fn __init__(self, message=\"\"):\n"
    "        self.message = message\n"
    "    fn __str__(self):\n"
    "        if self.message == \"\":\n"
    "            return typename(self)\n"
    "        return typename(self) + \": \" + str(self.message)\n"
    "\n"
    "class ArithmeticError(Exception):\n"
    "    pass\n"
    "class ZeroDivisionError(ArithmeticError):\n"
    "    pass\n"
    "class OverflowError(ArithmeticError):\n"
    "    pass\n"
    "class LookupError(Exception):\n"
    "    pass\n"
    "class KeyError(LookupError):\n"
    "    pass\n"
    "class IndexError(LookupError):\n"
    "    pass\n"
    "class ValueError(Exception):\n"
    "    pass\n"
    "class TypeError(Exception):\n"
    "    pass\n"
    "class NameError(Exception):\n"
    "    pass\n"
    "class AttributeError(Exception):\n"
    "    pass\n"
    "class AssertionError(Exception):\n"
    "    pass\n"
    "class RuntimeError(Exception):\n"
    "    pass\n"
    "class RecursionError(RuntimeError):\n"
    "    pass\n"
    "class NotImplementedError(RuntimeError):\n"
    "    pass\n"
    "class OSError(Exception):\n"
    "    pass\n"
    "class IOError(OSError):\n"
    "    pass\n"
    "class ImportError(Exception):\n"
    "    pass\n"
    "class MemoryError(Exception):\n"
    "    pass\n"
    "class StopIteration(Exception):\n"
    "    pass\n"
    "\n"
    "fn __drain_fiber(f):\n"
    "    return [x for x in f]\n"
    "\n";

static const char *PRELUDE_NAMES[] = {
    "Exception", "ArithmeticError", "ZeroDivisionError", "OverflowError", "LookupError", "KeyError",
    "IndexError", "ValueError", "TypeError", "NameError", "AttributeError", "AssertionError", "RuntimeError",
    "RecursionError", "NotImplementedError", "OSError", "IOError", "ImportError", "MemoryError",
    "StopIteration", "__drain_fiber", NULL};

void loadPrelude(MVM *vm)
{
    MyMoModule *savedModule = vm->currentModule;
    if (vm->currentModule == NULL)
        vm->currentModule = newModule(vm, newString(vm, "__prelude__", 11), newString(vm, "<prelude>", 9));
    MyMoFunction *function = compile(vm, PRELUDE, "<prelude>", COMPILE_SCRIPT);
    if (function == NULL || interpreter(vm, function) != OK)
    {
        fprintf(stderr, "mymo: internal error: the prelude failed to load\n");
        vm->currentModule = savedModule;
        return;
    }
    vm->currentModule = savedModule;
    for (const char **name = PRELUDE_NAMES; *name; name++)
    {
        MyMoObject *key = AS_OBJECT(newString(vm, *name, (int)strlen(*name)));
        Value value;
        if (getEntryV(&vm->globals, key, &value))
        {
            setEntryV(vm, &vm->builtins, key, value);
            deleteEntry(vm, &vm->globals, key);
        }
    }
    deleteEntry(vm, &vm->globals, AS_OBJECT(newString(vm, "__name__", 8)));
}
