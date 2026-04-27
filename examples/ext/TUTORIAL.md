# Writing a MyMo extension module in C

This tutorial walks through building a MyMo extension module from
scratch. Two complete examples ship in this directory:

- `hello.c`   — three trivial functions (`greet`, `square`, `add`)
- `strings.c` — a slightly larger module showing multi-arg parsing,
  bool returns, and renaming a function on export

The public C-API lives in `include/mymo_module.h`. Read it once — it
fits on two screens.

---

## 1. The 30-second version

```c
#include "mymo_module.h"

static MyMoObject *greet(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *name;
    if (!mymo_parse(vm, "greet", argc, argv, "s", &name))
        return MYMO_ERROR;
    return mymo_strf(vm, "hello, %s!", name);
}

MYMO_MODULE(hello,
    MYMO_FN(greet),
)
```

Build and run:

```bash
# from this directory:
cc -shared -fPIC -o hellomod.dylib hello.c -I../../include   # macOS
cc -shared -fPIC -o hellomod.so    hello.c -I../../include   # Linux

MYMO_HOME=. ../../mymo  myscript.my
```

`myscript.my`:

```mymo
from "hello" use greet
print(greet("world"))   #=> hello, world!
```

That's it. The rest of this document explains what's happening.

---

## 2. Anatomy of a module file

A module is a single `.c` file that:

1. Includes `mymo_module.h`.
2. Defines one or more **functions** with the signature
   `MyMoObject *fn(MVM *vm, uint argc, MyMoObject *argv[])`.
3. Calls `MYMO_MODULE(name, ...)` to declare the entry point.

The compiled artefact must be named **`<name>mod.<ext>`** — the loader
looks for `hellomod.dylib`, `stringsmod.so`, etc. The `<name>` here
must match the first argument of `MYMO_MODULE` *and* the name used in
`from "<name>" use ...`.

### Search path

When MyMo encounters `from "x" use ...` and `x.my` doesn't exist, it
looks for `xmod.<ext>` in this order:

1. `$MYMO_HOME/lib/xmod.<ext>`
2. `./xmod.<ext>` — current working directory
3. `./modules/xmod.<ext>`
4. `/opt/mymo/lib/xmod.<ext>`
5. Bare-name fallback through the OS dynamic loader (so
   `DYLD_LIBRARY_PATH` / `LD_LIBRARY_PATH` work).

For development, `MYMO_HOME=.` plus a local `xmod.dylib` is the
simplest layout.

---

## 3. Building values

| C call                    | MyMo value             |
| ------------------------- | ---------------------- |
| `mymo_int(vm, 42)`        | `42`                   |
| `mymo_double(vm, 3.14)`   | `3.14`                 |
| `mymo_str(vm, "hi")`      | `"hi"`                 |
| `mymo_strn(vm, p, len)`   | string from buffer     |
| `mymo_strf(vm, "%d!", n)` | printf-style (≤4 KiB)  |
| `MYMO_NIL`                | `nil`                  |
| `MYMO_TRUE` / `MYMO_FALSE`| `true` / `false`       |
| `MYMO_ERROR`              | sentinel: error raised |

`MYMO_ERROR` is what you return *after* calling `runtimeError(vm,
...)` (or after `mymo_parse` returned false — it has already raised
the error for you). The VM checks for this sentinel and propagates the
failure.

---

## 4. Validating arguments — `mymo_parse`

The format string is the same idea as Python's `PyArg_ParseTuple`. Each
character represents one positional argument:

| char | C out-type        | MyMo type              |
| ---- | ----------------- | ---------------------- |
| `i`  | `long *`          | int                    |
| `d`  | `double *`        | double or int (widens) |
| `s`  | `const char **`   | string contents        |
| `n`  | `int *`           | length of preceding `s` (does not consume an arg) |
| `b`  | `int *`           | bool                   |
| `o`  | `MyMoObject **`   | any object             |
| `S`  | `MyMoString **`   | string                 |
| `L`  | `MyMoList **`     | list                   |
| `T`  | `MyMoTuple **`    | tuple                  |

Examples:

```c
// One string arg.
const char *s;
if (!mymo_parse(vm, "f", argc, argv, "s", &s)) return MYMO_ERROR;

// String + its length (the 'n' piggybacks on the previous 's').
const char *s; int slen;
if (!mymo_parse(vm, "f", argc, argv, "sn", &s, &slen)) return MYMO_ERROR;

// Two ints and a list.
long a, b; MyMoList *xs;
if (!mymo_parse(vm, "f", argc, argv, "iiL", &a, &b, &xs)) return MYMO_ERROR;
```

If `argc` doesn't match the format, or any type is wrong, `mymo_parse`
calls `runtimeError` itself and returns `false`. You just propagate
`MYMO_ERROR`.

For a polymorphic function — say one that wants to accept "int OR
double" — drop down to the predicates (`mymo_is_int`, `mymo_is_double`)
plus `mymo_check_args(vm, name, argc, expected)`. See `add()` in
`hello.c`.

---

## 5. Declaring the module — `MYMO_MODULE`

```c
MYMO_MODULE(strings,
    MYMO_FN(upper),
    MYMO_FN(starts_with),
    MYMO_FN(repeat),
    MYMO_FN_AS("len", string_length),
)
```

- The first argument is the module's **MyMo-visible name**.
- `MYMO_FN(fn)` exports the C identifier `fn` under the same name.
- `MYMO_FN_AS("alias", fn)` exports it under a different name —
  useful when the C identifier collides with a keyword/macro.

The macro generates the platform-correct entry-point symbol
(`stringsModule` here) and a static function/variable table.

If you need more than 16 functions, exported global *variables*, or
computed names, drop down to the long form:

```c
MODULE(strings)
{
    static MyMoModuleFunction fns[] = { /* ... */ };
    static MyMoModuleVariable vars[] = {
        { "VERSION", AS_OBJECT(newInt(vm, 1)) },
    };
    static MyMoModuleDef def = {
        .name = "strings",
        .functions = fns, .totalfn = sizeof(fns)/sizeof(fns[0]),
        .variables = vars, .totalvar = sizeof(vars)/sizeof(vars[0]),
    };
    return defineBuiltInModule(vm, &def);
}
```

---

## 6. Memory & lifetimes

- The objects you return from a function (made via `mymo_int`,
  `mymo_str`, …) are allocated on the VM heap. Don't `free` them;
  don't keep raw pointers across MyMo calls.
- The `argv[]` array is owned by the VM for the duration of your
  function call. Read from it freely; do **not** retain pointers past
  return.
- String contents from `mymo_as_string` / `mymo_parse %s` point into
  the corresponding `MyMoString`'s buffer. Treat as const and don't
  outlive the call.
- The C heap (e.g. a `malloc` you do internally — see
  `repeat()` in `strings.c`) is yours to manage. Free before return.

---

## 7. Error handling

```c
if (n < 0) {
    runtimeError(vm, "%s: count must be >= 0", "f");
    return MYMO_ERROR;
}
```

The format string takes the usual printf specifiers. After
`runtimeError`, you must return `MYMO_ERROR` (== `NEW_EMPTY`) so the
VM unwinds.

---

## 8. End-to-end checklist

1. Write `myextmod.c` including `mymo_module.h`.
2. Compile to `myextmod.<ext>` with `-shared -fPIC -I<mymo>/include`
   plus `-undefined dynamic_lookup` on macOS.
3. Place the `.dylib`/`.so` somewhere on the search path
   (`MYMO_HOME=.` is fine for local development).
4. From MyMo: `from "myext" use fn1, fn2; print(fn1(...))`.

If `dlopen` fails silently, run with `DYLD_PRINT_LIBRARIES=1`
(macOS) or `LD_DEBUG=libs` (Linux) to see the loader's resolution
attempts.

---

## 9. Exporting constants and variables

Module values can't live in a `static` initializer (they need a live
`vm` to allocate). Instead, register them at module-init time using
`MYMO_MODULE_EX(name, INIT_BLOCK, fn1, ...)`. Inside `INIT_BLOCK`, two
identifiers are in scope: `vm` (the running `MVM *`) and `_mod` (the
module's `MyMoObject *`).

```c
MYMO_MODULE_EX(mymath,
    {
        mymo_set_double(vm, _mod, "PI",      3.14159265358979);
        mymo_set_double(vm, _mod, "E",       2.71828182845905);
        mymo_set_double(vm, _mod, "TAU",     6.28318530717958);
        mymo_set_int   (vm, _mod, "MAX_INT", 9223372036854775807L);
        mymo_set_str   (vm, _mod, "VERSION", "1.0.0");
    },
    MYMO_FN(area),
    MYMO_FN(circumference),
)
```

Then in MyMo:

```mymo
from "mymath" use PI, E, area, circumference, VERSION
print(PI)               #=> 3.14159265358979
print(area(5))          #=> 78.5398...
print(VERSION)          #=> 1.0.0
```

Helpers available:

| Call                                              | Stores               |
| ------------------------------------------------- | -------------------- |
| `mymo_set(vm, _mod, "X", value)`                  | any `MyMoObject *`   |
| `mymo_set_int(vm, _mod, "N", 42)`                 | int                  |
| `mymo_set_double(vm, _mod, "PI", 3.14159)`        | double               |
| `mymo_set_str(vm, _mod, "VERSION", "1.0")`        | string               |

For richer values (lists, dicts, nested objects), build them yourself
and call the generic `mymo_set`:

```c
MyMoObject *colors = NEW_LIST(vm);
writeMyMoObjectArray(vm, &AS_LIST(colors)->values, mymo_str(vm, "red"));
writeMyMoObjectArray(vm, &AS_LIST(colors)->values, mymo_str(vm, "blue"));
mymo_set(vm, _mod, "COLORS", colors);
```

### Constness

MyMo doesn't currently enforce immutability on module attrs. Conventional
shouting case (`PI`, `MAX`, `VERSION`) is the only contract — `from "X"
use PI; PI = 0` would shadow `PI` in the caller's scope without
mutating the module. There's a complete example at
`examples/ext/mymath.c`.
