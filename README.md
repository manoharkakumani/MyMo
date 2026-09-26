# MyMo

MyMo is a small, dynamically typed scripting language with a stack-based
bytecode VM written in C. It is inspired by
[Crafting Interpreters](https://craftinginterpreters.com/) and
[Wren](https://wren.io/).

The syntax is indentation-based and Python-like, with a few ideas borrowed
from elsewhere: arrow functions, a `|>` pipe operator, LISP-style `cond`,
pattern-matching `case`, fibers for cooperative concurrency, and decorators
for building web backends Hono/Express-style.

```python
from "mono" use get, post, start

@get("/hello/:name")
fn hello(req):
    return {"greeting": f"hello, {req["params"]["name"]}!"}

start("127.0.0.1", 8080)
```

Source files use `.my`.

---

## Contents

- [Build and run](#build-and-run)
- [Language tour](#language-tour)
  - [Values and variables](#values-and-variables)
  - [Strings](#strings)
  - [Collections](#collections)
  - [Control flow](#control-flow)
  - [`cond` and `case`](#cond-and-case)
  - [Functions, arrows and closures](#functions-arrows-and-closures)
  - [Pipes and ternaries](#pipes-and-ternaries)
  - [Decorators](#decorators)
  - [Classes](#classes)
  - [Errors: `try` / `catch` / `raise`](#errors-try--catch--raise)
  - [Fibers](#fibers)
  - [Modules](#modules)
- [Building web backends with `mono`](#building-web-backends-with-mono)
- [Built-in functions](#built-in-functions)
- [Standard modules](#standard-modules)
- [Writing C extensions](#writing-c-extensions)
- [How it works](#how-it-works)
- [Development](#development)
- [Known limitations](#known-limitations)

---

## Build and run

Requirements: a C11 compiler, `libcurl` and `libsqlite3`. On macOS these
ship with the system; on Debian/Ubuntu install `libcurl4-openssl-dev` and
`libsqlite3-dev`.

```sh
make                    # release build -> ./mymo
./mymo                  # interactive REPL
./mymo examples/loop.my # run a script
```

Other targets:

| Command      | What it does                                                   |
| ------------ | -------------------------------------------------------------- |
| `make debug` | Build `./mymo-debug` with bytecode disassembly and stack traces |
| `make test`  | Run every `examples/*.my` and diff against `tests/golden/`      |
| `make bench` | Time the scripts in `benchmarks/`                              |
| `make clean` | Remove binaries and object files                               |

---

## Language tour

### Values and variables

```python
a = 1               # int
b = 2.5             # double
c = True            # bool (True / False)
d = Nil             # nil
e = "text"          # string

x = 1
x += 2              # also -=
print(a, b, c, d, e, x, type(a))
```

Comments start with `#`. A block comment is wrapped in `##`:

```python
## this is
   a block comment ##
print("comments are ignored")   # trailing comment
```

Operators: `+ - * / // % **`, comparisons `== != < <= > >=`, bitwise
`& | ^ << >>`, and logical `and or not`. `/` on two ints gives an int when
the result is whole (`6 / 2` is `3`) and a double otherwise (`7 / 2` is
`3.5`).

### Strings

Strings use `'single'`, `"double"` or `` `backtick` `` quotes. Backtick
strings can span lines.

```python
name = "MyMo"
print(name + "!", name[0], name[1:], len(name))

# f-strings interpolate any expression inside {...}
x = 3
print(f"x = {x}, x squared = {x * x}")

# Escapes: \n \t \r \0 \\ \' \" \` \xHH, plus \{ \} for literal braces
print("line one\nline two\ttabbed \"quoted\"")
print(f"literal \{braces\} next to {x}")
```

Unknown escapes keep their backslash, so `"C:\dir"` stays as written.

### Collections

```python
nums = [1, 2, 3]
nums.append(4)
print(nums, nums[0], len(nums), nums.copy())

point = (3, 4)            # tuple; the parentheses are optional: 3, 4
print(point[0], point.__len__())

ages = {"ana": 31, "bo": 27}
ages["cy"] = 40
print(ages["ana"], ages.get("zed"), ages.has("bo"))
print(ages.keys(), ages.values(), len(ages))
ages.delete("bo")

for name in ages:         # iterates keys
    print(name, ages[name])
```

`dict.get(key)` returns `Nil` for a missing key, while `dict[key]` raises.
Dict fields can also be read with a dot: `ages.ana`.

### Control flow

```python
n = 7
if n < 0:
    print("negative")
elif n == 0:
    print("zero")
else:
    print("positive")

for ch in "abc":
    print(ch)

i = 0
while i < 3:
    i += 1
    if i == 2:
        continue
    print("i =", i)
```

`break` and `continue` work in both loops. `pass` is an empty statement.

### `cond` and `case`

`cond` is a guard chain: the first true condition wins.

```python
score = 72
cond
    score >= 90:
        print("A")
    score >= 70:
        print("B")
    True:
        print("C")
```

`case` matches a value against arms. Arms can list several values, `_` is
the default, and `fall` continues into the next arm:

```python
code = 3
case code:
    1, 3:
        print("odd and small")
    fall
    10:
        print("reached by fallthrough")
    _:
        print("default")
```

Tuples and lists match structurally, and `_` inside a pattern matches
anything:

```python
case (1, 5, 3):
    (1, _, 3):
        print("starts with 1, ends with 3")
    _:
        print("no match")
```

### Functions, arrows and closures

```python
fn area(w, h):
    return w * h

# arrow functions: one expression...
square = x => x * x
add = (a, b) => a + b

# ...or a block, ended by a blank line
clamp = (v, lo, hi) =>
    if v < lo:
        return lo
    return v if v < hi else hi

print(area(2, 3), square(4), add(1, 2), clamp(15, 0, 10))
```

Functions defined inside other functions are closures. They keep access
to the enclosing function's arguments and locals after it returns:

```python
fn adder(n):
    return x => x + n

add10 = adder(10)
print(add10(5))

fn counter():
    state = {"n": 0}
    fn next():
        state["n"] = state["n"] + 1
        return state["n"]
    return next

tick = counter()
tick()
print(tick())
```

Assigning to a captured name inside the closure creates a new local rather
than changing the outer variable. To share mutable state, keep it in a dict
or list, as `counter` does.

### Pipes and ternaries

```python
double = x => x * 2
10 |> print                 # print(10)
5 |> double |> print        # print(double(5))

size = "big" if 100 > 10 else "small"
sign = -3 < 0 ? "neg" : "pos"
print(size, sign)
```

`x |> f(a)` calls `f(x, a)`: the piped value comes first and any extra
arguments follow.

### Decorators

A line starting with `@` decorates the `fn` below it:

| You write                    | It means                   |
| ---------------------------- | -------------------------- |
| `@log` then `fn f`           | `f = log(f)`               |
| `@route("/x")` then `fn f`   | `f = route("/x", f)`       |
| `@app.get("/x")` then `fn f` | `f = app.get("/x", f)`     |

The function is passed as the **last** argument, so a decorator is an
ordinary function whose final parameter is the function being decorated.
The same helper therefore works with or without `@`. Stacked decorators
apply bottom-up.

```python
routes = {}

fn register(path, handler):
    routes[path] = handler
    return handler            # return the fn so the name stays usable

fn shout(f):
    return x => f(x) + "!"

@register("/greet")
@shout
fn greet(name):
    return "hello " + name

print(greet("ana"), routes["/greet"]("bo"))
```

Decorators work on top-level and nested functions, but not on class
methods.

### Classes

```python
class Point:
    fn __init__(self, x, y):
        self.x = x
        self.y = y

    fn norm2(self):
        return self.x * self.x + self.y * self.y

    fn +(self, other):              # operator overloading
        return Point(self.x + other.x, self.y + other.y)

class Point3(Point):                # inheritance
    fn describe(self):
        return f"point with norm2 {self.norm2()}"

p = Point(3, 4)
q = p + Point(1, 1)
print(p.norm2(), q.x, q.y, Point3(1, 2).describe())
```

### Errors: `try` / `catch` / `raise`

```python
fn divide(a, b):
    if b == 0:
        raise "division by zero"
    return a / b

try:
    print(divide(4, 0))
catch e:
    print("caught:", e)

try:
    missing = {}["key"]
catch:
    print("runtime errors are catchable too")
```

`catch e` binds the error message. Plain `catch:` discards it.

### Fibers

Fibers are coroutines. `run` starts one, `yield(v)` pauses it and hands
`v` back, and `resume(v)` continues it with `v` as the result of `yield`.

```python
fn worker():
    got = yield("first")
    print("worker received", got)
    yield("second")

f = fiber(worker)
print(f.run())
print(f.resume(42))
print(f.alive())
```

### Modules

```python
from "math" use sqrt, floor
print(sqrt(16), floor(2.7))
```

`from "name" use a, b` works for built-in modules, for the embedded
standard library (`mono`, `strutil`) and for your own `.my` files, which
are resolved relative to the running script. A file can check whether it is
the entry point with `if __name__ == "__main__":`.

---

## Building web backends with `mono`

`mono` is a small web framework written in MyMo on top of the built-in
`server` and `json` modules. Routes are registered with decorators, and a
handler's return value becomes the response:

| Handler returns          | Response                                     |
| ------------------------ | -------------------------------------------- |
| a string                 | `200` with `text/plain`                      |
| a dict, list, number ... | `200` with JSON                              |
| `(status, body)`         | that status, body encoded as above           |
| `Nil`                    | `204`, unless you already called `json()` / `text()` |

```python
from "mono" use get, post, put, delete, not_found, start

db = {"todos": {}, "next_id": 1}

@get("/todos")
fn list_todos(req):
    items = []
    for k in db["todos"]:
        items.append(db["todos"][k])
    return items

@get("/todos/:id")
fn show_todo(req):
    todo = db["todos"].get(req["params"]["id"])
    if todo == Nil:
        return (404, {"error": "todo not found"})
    return todo

@post("/todos")
fn create_todo(req):
    body = req["json"]
    if body == Nil:
        return (400, {"error": "invalid JSON body"})
    id = db["next_id"]
    db["next_id"] = id + 1
    body["id"] = id
    db["todos"][str(id)] = body
    return (201, body)

@not_found
fn missing(req):
    return (404, {"error": "no route", "path": req["path"]})

start("127.0.0.1", 8080)
```

```sh
curl -X POST -d '{"title":"buy milk"}' http://127.0.0.1:8080/todos
curl http://127.0.0.1:8080/todos/1
```

The request is a dict with:

| Key         | Contents                                            |
| ----------- | --------------------------------------------------- |
| `method`    | `"GET"`, `"POST"`, ...                              |
| `path`      | the request path                                    |
| `params`    | values bound by `:name` segments in the route       |
| `body`      | the raw body string                                 |
| `json`      | the body decoded as JSON, or `Nil`                  |
| `client_fd` | the socket, used by `text()` / `json()`             |

Details:

- Route helpers: `get`, `post`, `put`, `delete`, `patch`, and `route(method, path, handler)` for anything else.
- `text(req, status, body)` and `json(req, status, value)` send a response explicitly.
- A handler that raises is turned into a `500` JSON response, so one bad request can't take the server down.
- The decorator-free form still works: `get("/", handler)`.

Complete apps: `examples/modules/mono_app.my` (decorators) and
`examples/modules/mono_demo.my` (explicit registration).

---

## Built-in functions

| Function                  | Description                                       |
| ------------------------- | ------------------------------------------------- |
| `print(a, b, ...)`        | Print values separated by spaces                  |
| `input(prompt)`           | Read a line from stdin                            |
| `type(v)`                 | Type name, e.g. `<object 'int'>`                  |
| `len(v)`                  | Length of a string, list, tuple or dict           |
| `str(v)`                  | Convert to string                                 |
| `clock()`                 | CPU time in seconds                               |
| `fiber(fn)`               | Create a fiber (see [Fibers](#fibers))            |
| `yield(v)`                | Pause the current fiber                           |
| `globals()`               | The global variable dict                          |
| `compile(src)` / `exec(code)` | Compile and run MyMo source at runtime       |

Methods on built-in types:

- **string**: `__len__()`
- **list**: `append(v)`, `copy()`, `__len__()`
- **tuple**: `__len__()`
- **dict**: `get(k)`, `put(k, v)`, `has(k)`, `delete(k)`, `keys()`, `values()`, `__len__()`
- **fiber**: `run(...)`, `resume(v)`, `alive()`, `kill()`

---

## Standard modules

Import with `from "<module>" use name1, name2`. Each one has a runnable
demo in `examples/modules/`.

| Module    | Exports                                                                 |
| --------- | ----------------------------------------------------------------------- |
| `math`    | `floor`, `ceil`, `sqrt`, `sin`, `cos`, `tan`                            |
| `time`    | `now`, `monotonic`, `clock`, `sleep`, `format`                          |
| `date`    | `year`, `month`, `day`, `hour`, `minute`, `second`, `weekday`, `yearday`, `iso`, `parse`, `make` |
| `random`  | `seed`, `int`, `float`, `choice`                                        |
| `os`      | `getenv`, `setenv`, `unsetenv`, `getcwd`, `exit`                        |
| `io`      | `read`, `write`, `append`, `exists`, `remove`                           |
| `json`    | `encode`, `decode`                                                      |
| `sqlite`  | `open`, `run`, `query`, `close`                                         |
| `http`    | `get`, `post`, `request` (client, via libcurl)                          |
| `socket`  | `connect`, `listen`, `accept`, `send`, `recv`, `close`, `gethostname`, `resolve` |
| `server`  | `listen`, `accept`, `respond`, `respond_json`, `close` (HTTP/1.1)       |
| `runloop` | `nonblock`, `readable`, `writable`, `select` (non-blocking I/O)         |
| `nodes`   | `spawn`, `send`, `recv`, `kill`, `self_id`, `is_coordinator`, `children` (multi-process messaging) |
| `mono`    | web framework, see above (written in MyMo)                              |
| `strutil` | string helpers such as `starts_with` (written in MyMo)                  |

---

## Writing C extensions

Native modules are shared libraries named `<name>mod.dylib` (macOS) or
`<name>mod.so` (Linux), loaded on first `use`. A builtin takes its
arguments as objects and returns a `Value`:

```c
#include "mymo_module.h"

static Value greet(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *name;
    if (!mymo_parse(vm, "greet", argc, argv, "s", &name))
        return MYMO_ERROR;                      // error already raised
    return objectToValue(mymo_strf(vm, "hello, %s!", name));
}

MYMO_MODULE(hello, MYMO_FN(greet))
```

```sh
make ext-hello                      # builds examples/ext/hellomod.dylib
cd examples/ext
cat > greet.my <<'EOF'
from "hello" use greet
print(greet("world"))
EOF
../../mymo greet.my                 # hello, world!
```

The loader looks for `<name>mod.<ext>` in `$MYMO_HOME/lib/`, the current
directory, `./modules/` and `/opt/mymo/lib/`.

Return `MYMO_NIL`, `MYMO_TRUE`/`MYMO_FALSE`/`MYMO_BOOL(cond)` directly,
wrap objects from `mymo_int`, `mymo_str` and friends in `objectToValue()`,
and return `MYMO_ERROR` after `runtimeError()`. The full guide is in
[`examples/ext/TUTORIAL.md`](examples/ext/TUTORIAL.md).

> The builtin return type changed from `MyMoObject *` to `Value`, so
> extensions compiled against older headers must be rebuilt.

---

## How it works

```
source ──► lexer ──► single-pass Pratt compiler ──► bytecode chunk ──► VM
          (indent-aware)   (expression.c,                       (vm.c, computed-goto
           lexer.c)         statement.c)                          dispatch)
```

- **Values** are NaN-boxed 64-bit words: doubles, 32-bit ints, `nil`,
  `true` and `false` live inline, and everything else is a pointer to a heap
  object (`value.h`).
- **Dispatch** uses computed gotos on GCC/Clang (`dispatch.h`), with
  fused instructions such as `OP_INVOKE_GLOBAL` and inline caches for
  variable access.
- **Fibers** each own an operand stack and call-frame chain. Switching
  fibers swaps `vm->fiber`.
- **Closures** are per-call copies of a function bound to their defining
  frame. A captured frame outlives its call.
- **Memory**: every object is linked into `vm->objects` and freed when the
  VM exits. There is no garbage collector yet.

`REDESIGN.md` tracks the ongoing performance redesign (NaN-boxing, inline
caches, and next steps).

---

## Development

```sh
make clean && make && make test && make bench
```

- `make test` runs each `examples/*.my` and compares its output to
  `tests/golden/<name>.out`. A few examples are skipped; see
  `tests/KNOWN_FAILURES.md`.
- Adding an opcode takes three coordinated edits: the enum in `opcodes.h`,
  the label in `dispatch.h` (same order), and the handler in `vm.c`.
- Built-in modules live in `modules/`: add a `MODULE(...)` declaration in
  `modules/modules.h` and register it in `modules/modules.c`.
- Standard-library modules written in MyMo live in `stdlib/` and are
  embedded into the binary at build time.

---

## Known limitations

MyMo is an experimental language. Current gaps:

- No garbage collector: memory is reclaimed only when the program exits.
- Ints are 32-bit when stored inline. Wider values fall back to slower heap
  objects.
- Functions have no default parameter values.
- Strings can't be compared with `<` / `>`, and lists can't be joined with
  `+`.
- Lists only have `append`, `copy` and `__len__`.
- Decorators can't be applied to class methods.
- `super` is not reliable yet; call inherited methods through `self`.
- The `.myc` bytecode cache is disabled during the value-representation
  redesign.
