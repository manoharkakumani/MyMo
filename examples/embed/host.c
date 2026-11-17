// host.c — a C program embedding MyMo through include/mymo.h.
//
//   make embed-example && ./examples/embed/host
//
// Shows: exposing C functions to MyMo, running scripts, calling MyMo
// functions and closures from C, a C builtin that calls back into MyMo,
// passing lists/dicts across, error handling, and keeping a value alive
// across garbage collections with mymo_retain.

#include "mymo.h"
#include <stdio.h>

// host_log(msg) — a C function MyMo code can call.
static Value host_log(MVM *vm, uint argc, Value argv[])
{
    const char *msg;
    if (!mymo_parse(vm, "host_log", argc, argv, "s", &msg))
        return MYMO_ERROR;
    printf("[host] %s\n", msg);
    return MYMO_NIL;
}

// apply_twice(fn, x) — calls a MyMo callable from C: fn(fn(x)).
static Value apply_twice(MVM *vm, uint argc, Value argv[])
{
    if (argc != 2)
    {
        runtimeError(vm, "apply_twice() takes 2 arguments (%u given)", argc);
        return MYMO_ERROR;
    }
    Value fn = argv[0];
    Value x = argv[1];
    for (uint i = 0; i < argc; i++) // builtins pop their own arguments
        popV(vm);
    Value once, twice;
    if (mymo_call(vm, fn, 1, &x, &once) != MYMO_OK || mymo_call(vm, fn, 1, &once, &twice) != MYMO_OK)
        return MYMO_ERROR; // the error was already reported
    return twice;
}

static const char *SCRIPT =
    "host_log(\"script starting\")\n"
    "fn greet(name):\n"
    "    return f\"hello, {name}!\"\n"
    "\n"
    "fn make_counter(start):\n"
    "    state = {\"n\": start}\n"
    "    fn next():\n"
    "        state[\"n\"] = state[\"n\"] + 1\n"
    "        return state[\"n\"]\n"
    "    return next\n"
    "\n"
    "fn total(xs):\n"
    "    s = 0\n"
    "    for x in xs:\n"
    "        s = s + x\n"
    "    return s\n"
    "\n"
    "fn fails():\n"
    "    return {}[\"missing\"]\n"
    "\n"
    "print(\"apply_twice:\", apply_twice(x => x * 3, 2))\n";

int main(void)
{
    MVM *vm = mymo_new();
    mymo_define_function(vm, "host_log", host_log);
    mymo_define_function(vm, "apply_twice", apply_twice);

    if (mymo_run_string(vm, SCRIPT, "host-script") != MYMO_OK)
    {
        fprintf(stderr, "script failed: %s\n", mymo_last_error(vm));
        return 1;
    }

    // Call a MyMo function with a C string.
    Value greet, out;
    mymo_get_global(vm, "greet", &greet);
    Value name = mymo_string(vm, "embedder");
    if (mymo_call(vm, greet, 1, &name, &out) == MYMO_OK)
        printf("greet -> %s\n", mymo_val_cstring(out));

    // Get a closure back and keep it across a GC-heavy script.
    Value make_counter, counter, n;
    mymo_get_global(vm, "make_counter", &make_counter);
    Value start = MYMO_INT(10);
    mymo_call(vm, make_counter, 1, &start, &counter);
    mymo_retain(vm, counter);
    mymo_run_string(vm,
                    "i = 0\n"
                    "while i < 50000:\n"
                    "    junk = [str(i), {\"k\": i}]\n"
                    "    i = i + 1\n",
                    "churn");
    mymo_call(vm, counter, 0, NULL, &n);
    mymo_call(vm, counter, 0, NULL, &n);
    long value = 0;
    mymo_val_as_long(n, &value);
    printf("counter after churn -> %ld (expected 12)\n", value);
    mymo_release(vm, counter);

    // Pass a list built in C.
    Value total, list = mymo_list(vm);
    for (int i = 1; i <= 4; i++)
        mymo_list_append(vm, list, MYMO_INT(i * 10));
    mymo_get_global(vm, "total", &total);
    if (mymo_call(vm, total, 1, &list, &out) == MYMO_OK && mymo_val_as_long(out, &value))
        printf("total([10,20,30,40]) -> %ld\n", value);

    // Errors come back as MYMO_RUNTIME_ERROR; the VM stays usable.
    Value fails;
    mymo_get_global(vm, "fails", &fails);
    if (mymo_call(vm, fails, 0, NULL, NULL) == MYMO_RUNTIME_ERROR)
        printf("fails() -> error: %s\n", mymo_last_error(vm));
    if (mymo_run_string(vm, "print(\"still running:\", greet(\"again\"))\n", "after-error") == MYMO_OK)
        printf("VM still usable after an error\n");

    mymo_free(vm);
    return 0;
}
