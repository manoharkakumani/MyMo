# MyMo

MyMo is a small, dynamically typed scripting language with a stack-based
bytecode VM written in C. It is inspired by
[Crafting Interpreters](https://craftinginterpreters.com/) and
[Wren](https://wren.io/).

The syntax is indentation-based and Python-like, with a few ideas borrowed
from elsewhere: arrow functions, a `|>` pipe operator, LISP-style `cond`,
pattern-matching `case`, fibers for cooperative concurrency, and decorators
for building web backends Hono/Express-style. It has a garbage collector, a
bytecode cache, a C API for extensions and for embedding, a concurrent
web framework (`mono`) and an app UI library (`ui`) that runs the same
code in a browser, a desktop window, on a phone or in the terminal.

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

- [Install](#install)
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
  - [Command calls and blocks](#command-calls-and-blocks)
  - [Classes](#classes)
  - [Errors: `try` / `catch` / `raise`](#errors-try--catch--raise)
  - [Fibers](#fibers)
  - [Modules](#modules)
- [Building web backends with `mono`](#building-web-backends-with-mono)
- [Building apps with `ui`](#building-apps-with-ui)
- [Built-in functions](#built-in-functions)
- [Standard modules](#standard-modules)
- [Writing C extensions](#writing-c-extensions)
- [Embedding MyMo in C](#embedding-mymo-in-c)
- [How it works](#how-it-works)
- [Development](#development)
- [Known limitations](#known-limitations)

---

## Install

On Linux or macOS, from a checkout:

```sh
git clone https://github.com/manoharkakumani/MyMo.git
cd MyMo
./install.sh            # user install into ~/.mymo, no sudo
```

The script checks build dependencies (and prints the install command for
your package manager if something is missing), builds MyMo, installs it,
and adds `PATH` and `MYMO_HOME` to your shell startup files. Open a new
terminal, then run `mymo --version`.

| Option              | Effect                                                         |
| ------------------- | -------------------------------------------------------------- |
| `--system`          | Install into `/opt/mymo` and link `/usr/local/bin/mymo` (uses sudo) |
| `--prefix DIR`      | Install into `DIR`                                              |
| `--with-examples`   | Also install the example extension modules                      |
| `--no-modify-path`  | Don't edit shell startup files (source `$MYMO_HOME/env` yourself) |
| `--uninstall`       | Remove the install and the shell-file lines (same `--prefix`/`--system`) |

What gets installed (`$MYMO_HOME`):

| Path              | Contents                                                        |
| ----------------- | --------------------------------------------------------------- |
| `bin/mymo`        | the interpreter                                                 |
| `bin/mymo-config` | compiler flags for extensions and embedding                      |
| `lib/`            | `libmymo.a`, plus extension modules (`<name>mod.so`/`.dylib`), which `use` finds here |
| `include/mymo/`   | headers: `include/mymo_module.h` (extensions), `include/mymo.h` (embedding) |
| `env`             | sets `MYMO_HOME` and `PATH`                                      |

Build against an installed MyMo with `mymo-config`:

```sh
# an extension module, installed where `from "shout" use ...` finds it
cc $(mymo-config --cflags) $(mymo-config --ext-ldflags) \
   -o $(mymo-config --moddir)/shoutmod.$(mymo-config --ext) shout.c
# a C program embedding MyMo
cc $(mymo-config --cflags) -o host host.c $(mymo-config --libs)
```

Tested on macOS, Ubuntu 24.04 (glibc) and Alpine 3.20 (musl).

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
| `make lib`   | Build `libmymo.a` for [embedding](#embedding-mymo-in-c)        |
| `make clean` | Remove binaries and object files                               |

Running `foo.my` writes a bytecode cache to `__mycache__/foo.myc` in the
same directory, like Python's `__pycache__`. Later runs skip compiling while
the source (and the interpreter build) are unchanged. A `.myc` file can
also be run on its own (`./mymo foo.myc`), for example to ship bytecode
without source. Set `MYMO_NOCACHE=1` to disable the cache.

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

Operators: `+ - * / // % **`, comparisons `== != < <= > >=` (chainable:
`0 <= i < n`), membership `in` / `not in`, identity `is` / `is not`,
bitwise `& | ^ << >>`, and logical `and or not`. Ints are 64-bit; an
overflowing `+ - *` gives a double instead of wrapping. `/` on two ints gives an int when
the result is whole (`6 / 2` is `3`) and a double otherwise (`7 / 2` is
`3.5`). `//` and `%` follow Python: `-7 // 2` is `-4`, `-7 % 3` is `2`, and
both raise `ZeroDivisionError` on zero. Ints and doubles compare by value
(`0 == 0.0`). Convert with `int(x)` (also `int("ff", 16)`), `float(x)` (or
`double(x)`) and `str(x)`.

`Nil`, `False`, `0`, `0.0` and empty strings, lists, tuples, dicts, sets and
ranges are falsy; everything else is truthy.

Several names can be assigned at once, from any sequence:

```python
a, b = 1, 2
a, b = b, a                 # swap
first, second = "hi"
```

### Strings

Strings use `'single'`, `"double"` or `` `backtick` `` quotes. Backtick
strings can span lines. Strings are UTF-8, and `len`, indexing, slicing and
iteration count characters (code points): `len("日本語")` is 3.

```python
name = "MyMo"
print(name + "!", name[0], name[1:], len(name))

# f-strings interpolate any expression inside {...}
x = 3
print(f"x = {x}, x squared = {x * x}")

# format specs, like Python: fill/align, sign, width, grouping, precision, type
pi = 3.14159
print(f"{pi:.2f} {1234567:,} {42:08d} {255:#x} {"hi":>6} {0.25:%} {[1]!r}")
print("{} is {age:>3}".format("bo", age=7), format(pi, ".1f"))
print("%s has %d items (%.1f%%)" % ("cart", 3, 12.5))

# Escapes: \n \t \r \0 \\ \' \" \` \xHH \uXXXX \UXXXXXXXX, plus \{ \} for literal braces
print("line one\nline two\ttabbed \"quoted\"")
print(f"literal \{braces\} next to {x}")

# Methods
s = "  Hello, World  "
print(s.strip(), s.upper(), s.lower())
print("a,b,,c".split(","), "one  two".split(), "-".join(["x", "y"]))
print("hello".find("ll"), "hello".contains("ell"), "hello".replace("l", "L"))
print("file.my".startswith("file"), "file.my".endswith(".my"))
print("apple" < "banana")   # strings compare lexicographically
print("hello"[::-1], "a-b-c".split("-", 1), "k=v".partition("="), "7".zfill(3))
```

A `:` at the top level of an f-string field starts the format spec, so
write a `c ? a : b` ternary in parentheses there.

Unknown escapes keep their backslash, so `"C:\dir"` stays as written.

### Collections

```python
nums = [3, 1, 2]
nums.append(4)
nums.sort()
print(nums, nums[0], len(nums), nums + [5, 6], [0] * 3)
print(nums.pop(), nums.index(2), nums.contains(3))
nums.insert(0, 9)
nums.remove(9)
nums.extend([7, 8])
nums.reverse()
print(nums)

point = (3, 4)            # tuple; the parentheses are optional: 3, 4
single = (5,)             # one-element tuple
print(point[0], len(point), point + single)

ages = {"ana": 31, "bo": 27}
ages["cy"] = 40
print(ages["ana"], ages.get("zed"), ages.has("bo"))
print(ages.keys(), ages.values(), len(ages))
ages.delete("bo")

for name in ages:         # iterates keys
    print(name, ages[name])
```

`dict.get(key[, default])` returns `Nil` (or the default) for a missing
key, while `dict[key]` raises `KeyError`. Dicts keep insertion order. Dict
fields can also be read with a dot: `ages.ana`. Keys may be strings,
numbers, bools, `Nil` or tuples of those.

```python
print(nums[1:3], nums[::-1], nums[-1])      # slicing like Python
del nums[0]
counts = {}
for word in "a b a".split():
    counts[word] = counts.get(word, 0) + 1   # or: counts[word] += 1
for word, n in counts.items():               # unpacking in for
    print(word, n)
grid = {(0, 0): "start"}                     # tuple keys

squares = [x * x for x in range(10) if x % 2 == 0]
pairs = [(x, y) for x in range(3) for y in "ab"]
index = {name: i for i, name in enumerate(["a", "b"])}

seen = {1, 2, 3}                             # sets: | & - ^, add, remove, ...
print(seen | {4}, seen & {2, 9}, 2 in seen, set("hello"))
```

`range(stop)` / `range(start, stop[, step])` is lazy, so
`for i in range(1000000)` doesn't build a list.

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
`assert cond, "message"` raises `AssertionError` when `cond` is false.

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

# default parameter values (evaluated once, when the fn statement runs)
fn greet(name, greeting="hello", punct="!"):
    return greeting + ", " + name + punct

print(greet("ana"), greet("bo", "hi"))
```

Parameters with defaults must come after those without. As in Python, a
mutable default such as a list is shared between calls. Long parameter
lists, like long argument lists, can span several lines (a trailing comma
is fine).

Arguments can be passed by name, extra ones collected, and sequences or
dicts unpacked into a call:

```python
print(greet(punct="?", name="cy"))

fn log(level, *messages, **context):     # tuple and dict of the extras
    print(level, messages, context)

log("info", "started", "ok", user="ana")
args = ["warn", "disk"]
log(*args, **{"free": "2%"})
```

Parameters after `*rest`, or after a bare `*`, are keyword-only: they can
only be passed by name, and may be required even when they come after one
with a default:

```python
fn show(*items, sep=" ", end=""):
    return sep.join([str(x) for x in items]) + end

fn connect(host, *, port=80, secure):
    ...

print(show(1, 2, 3, sep=", "))
connect("example.com", secure=True)
```

A decorator can forward everything with `fn wrapper(*args, **kwargs):
return f(*args, **kwargs)`.

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

Assigning to a name creates a local variable. Declare `nonlocal n` to
assign the enclosing function's `n`, or `global n` to assign a module-level
variable:

```python
fn make_counter():
    n = 0
    fn bump():
        nonlocal n
        n += 1
        return n
    return bump

total = 0
fn add(x):
    global total
    total += x
```

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

Decorators work on top-level functions, nested functions and class
methods. A method decorator receives the function, and a wrapper it returns
is called with `self` as its first argument:

```python
fn logged(f):
    return (self, x) => "[log] " + f(self, x)

class Service:
    @logged
    fn handle(self, x):
        return "handled " + x

print(Service().handle("req"))
```

### Command calls and blocks

A statement can call a function without parentheses, and a trailing
`:` passes a block of code to it:

```python
print "hello", name              # print("hello", name)
show "Hi {name}"                 # strings with {...} interpolate here
save "notes.txt" force           # a bare word is a flag: force=True
save "notes.txt" mode="a"        # keyword arguments: key=value

fn repeat(n, body):              # the block arrives as `body`
    for i in range(n):
        body()

repeat 3:
    count += 1                   # assigns the outer `count`

card(title="x"):                 # a parenthesized call can take a block too
    ...
```

A block is a function passed as the keyword argument `body`. It reads and
assigns the variables around it (no `global` or `nonlocal` needed). The
loop variables of the `for` loops around it are fixed when the block is
created, so blocks made in a loop each keep their own item. A name alone
on a line calls it if it's a function (`divider`); in the REPL it prints
the value as before.

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

Operators can also be defined with Python's names: `__add__`, `__sub__`,
`__mul__`, `__eq__`, `__lt__`, `__le__`, `__neg__`, `__iadd__`, ... An
in-place operator without its own method uses the plain one. Other hooks:
`__str__` / `__repr__` (printing), `__len__`, `__contains__` (`in`),
`__getitem__` / `__setitem__` / `__delitem__` (`obj[k]`), `__call__`
(calling the object), and `__iter__` / `__next__` (for loops; raise
`StopIteration` to finish).

`super()` inside a method reaches the parent class's methods, bound to the
same `self`:

```python
class Animal:
    fn __init__(self, name):
        self.name = name
    fn speak(self):
        return self.name + " makes a sound"

class Dog(Animal):
    fn __init__(self, name, breed):
        super().__init__(name)
        self.breed = breed
    fn speak(self):
        return super().speak() + " (woof)"

print(Dog("rex", "lab").speak())
```

### Errors: `try` / `catch` / `raise`

```python
class InsufficientFunds(Exception):
    pass

fn withdraw(balance, amount):
    if amount > balance:
        raise InsufficientFunds(f"short by {amount - balance}")
    return balance - amount

try:
    withdraw(10, 50)
catch InsufficientFunds as e:
    print("declined:", e.message)
catch (KeyError, IndexError) as e:
    print("lookup failed:", e)
catch e:
    print("anything else:", e)
final:
    print("always runs")
```

Runtime errors are instances of built-in classes: `Exception` and its
subclasses `ValueError`, `TypeError`, `KeyError`, `IndexError` (both
`LookupError`), `ZeroDivisionError`, `OverflowError` (both
`ArithmeticError`), `NameError`, `AttributeError`, `AssertionError`,
`RuntimeError`, `RecursionError`, `NotImplementedError`, `OSError`
(`IOError`), `ImportError`, `MemoryError` and `StopIteration`. `str(e)`
reads like `KeyError: "key"`, and `e.message` holds the message.

Clauses are tried in order; an exception no clause matches keeps
propagating (after `final:` runs). `catch e:` catches everything, and
plain `catch:` discards the exception. `raise` accepts an instance, a class
(`raise ValueError`) or any value, such as a string. `final:` runs however
the `try` is left, including by `return`, `break` or `continue`; a `return`
or `break` inside `final:` itself wins over the pending exit or exception.
Runaway recursion raises a catchable `RecursionError`.

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

A `for` loop runs a fiber to each `yield` in turn, which makes generators:

```python
fn countdown():
    n = 3
    while n > 0:
        yield(n)
        n -= 1

for n in fiber(countdown):
    print(n)
```

An error that nothing inside a fiber catches ends the fiber and reaches the
code that resumed it. `run(args...)` passes arguments to the fiber's function. `yield` can also be
called from a helper function the fiber calls: it suspends the whole fiber.
That is how `mono`'s `sleep()` works.

Anything that takes an iterable also takes a fiber and runs it to the
end: `list(fiber(countdown))`, `sorted(...)`, `sum(...)`, `", ".join(...)`,
`zip(...)`, `f(*fiber(gen))`.

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

### Concurrency

`start()` runs an event loop that serves many connections at once on a
single thread. Requests are read without blocking, so a slow client doesn't
hold up anyone else. Each request's handler runs in its own
[fiber](#fibers), and a handler can wait without blocking the server:

```python
from "mono" use get, start, sleep

@get("/slow")
fn slow(req):
    sleep(1000)          # other requests keep being served meanwhile
    return {"done": True}

@get("/fast")
fn fast(req):
    return "fast"

start("127.0.0.1", 8080)
```

Five concurrent `/slow` requests finish together in about one second, and
`/fast` answers immediately while they wait. `wait_readable(fd)` does the
same for handlers that talk to their own sockets.

Connections use HTTP keep-alive (and pipelining), so clients reuse one
connection for many requests. Connections close when the client asks, after
100 requests, or after 5 seconds of silence.

### HTTPS

`mono` speaks plain HTTP. For TLS, put a reverse proxy in front of it. With
[Caddy](https://caddyserver.com/), which gets certificates automatically:

```
example.com {
    reverse_proxy 127.0.0.1:8080
}
```

nginx works the same way with `proxy_pass http://127.0.0.1:8080;`.

Complete apps: `examples/modules/mono_app.my` (decorators),
`examples/modules/mono_async.my` (concurrency) and
`examples/modules/mono_demo.my` (explicit registration).

---

## Building apps with `ui`

> The full guide, with every component, styling, APIs, projects and dev
> mode, is in [docs/ui.md](docs/ui.md). Start a project with
> `mymo new my-app`.

`ui` builds an app out of indented blocks and runs it wherever you want:

```python
from "ui" use *

count = 0

page "/":
    center:
        text count, size="display"
        row:
            button "−": count -= 1
            button "+" primary: count += 1

run "Counter"
```

```bash
mymo counter.my              # a browser tab
mymo counter.my --desktop    # its own window (Chrome, Edge or Brave in app mode)
mymo counter.my --phone      # served on your Wi-Fi: open the printed link on
                             # your phone, then Add to Home Screen
mymo counter.my --term       # full-screen in the terminal
```

A page's block describes the screen. Each component written inside a
block becomes a child of the block's component, and ordinary `for` and
`if` work there too. A block after a button, checkbox, input and so on is
its handler. After a handler runs, the page is drawn again and only the
parts that changed are updated in the browser, so the screen always
matches your data. The browser, desktop and phone targets get a
responsive design system with light and dark themes. The terminal target
draws the same components with box-drawing characters and keyboard
focus: arrows or Tab to move, Enter to press, ←→ to change, Esc to quit.

A to-do list:

```python
from "ui" use *

todos = []
draft = state("")

fn add():
    todos.append({"text": draft.value, "done": False})
    draft.clear()

page "/":
    card "To-do":
        row:
            input draft, placeholder="What needs doing?": add()
            button "Add" primary: add()
        for t in todos:
            row:
                checkbox t["text"], t["done"]: t["done"] = not t["done"]
                spacer()
                button icon="x" ghost small: todos.remove(t)

run "To-do"
```

**Data.** Plain variables hold your data, and handlers change them
directly (`count += 1` updates the outer `count`). An input needs
something it can write back to: create it with `state(value)`, pass it
(`input draft`, `checkbox "Agree", agreed`), and read it with
`draft.value` or `"{draft}"`. A state also has `set`, `inc`, `dec`,
`toggle`, `push`, `remove` and `clear`.

**Arguments.** Strings with `{...}` interpolate. Bare words after an
argument are flags (`button "Save" primary small` means `primary=True,
small=True`). Keyword arguments are `key=value`. A handler made in a
loop keeps its own item, so each row's delete button deletes that row.
Handlers can show `toast "Saved" success` (or `error`, `warning`) or switch pages with `go "/path"`, and
`every 5: ...` runs a block on a timer while the app is open.

**Components.**

| Kind       | Components |
| ---------- | ---------- |
| Layout     | `col`, `row`, `grid cols= min=`, `center`, `card "Title"`, `section "Title"`, `spacer()`, `divider()` |
| Text       | `h1`, `h2`, `h3`, `text` (`muted`, `bold`, `size=`, `color=`), `small`, `code`, `link "Label", "/to"`, `bullets [...]`, `kv {...}` |
| Display    | `badge`, `avatar "Name"`, `image`, `icon "name"`, `stat "Label", value, delta=, icon=`, `progress value, label=`, `table rows`, `alert "msg"` (`success`, `warning`, `error`), `empty "Title", "text"`, `spinner` |
| Input      | `button "Label"` (`primary`, `danger`, `ghost`, `small`, `large`, `icon=`), `input state` (`label=`, `placeholder=`, `password`, `live`), `textarea`, `checkbox "Label", value`, `switch`, `select options, state`, `slider state, min=, max=`, `tabs options, state` |
| App chrome | `shell:` with a `sidebar "Brand":` or `navbar "Brand":` first, `nav "Label", "/to", icon=`, `modal state, "Title":` |
| Escape hatches | `html(raw)`, `el(tag, ...)` |

Layout components take `gap=` and `pad=` (a 0-10 spacing scale, or any
CSS length), `align=`, `justify=`, `width=`, `max=`, `grow=True` and
`style=`. Colors are `accent`, `blue`, `green`, `red`, `amber`, `violet`,
`pink`, `teal`, `gray` and `orange`, or any CSS color. Everything also works
as ordinary calls (`card(h1("Hi"), title="x")`, `button("+", on=f)`,
`@page("/")` above a function that returns components).

`run "Title", accent=, theme=` starts the app. `theme` is `"auto"`,
`"light"` or `"dark"`. Several `page "/path":` blocks make a multi-page app,
and `link`/`nav` switch between them without a reload. `render(path)` and
`render_text(path)` return the HTML or terminal text without starting
anything, which is handy in tests. See `examples/ui/` for a counter, a
to-do list, a multi-page dashboard, and `examples/ui/weather/`: a
multi-file weather app with live data from a public API.

Every window and device that opens the app shares its data, the same way
a desktop app has one set of data. In `--phone` mode the link carries a
random access key; requests without it are refused.

---

## Built-in functions

| Function                  | Description                                       |
| ------------------------- | ------------------------------------------------- |
| `print(*values, sep=" ", end="\n")` | Print values                          |
| `input(prompt)`           | Read a line from stdin                            |
| `len(v)`                  | Length of a string, list, tuple, dict, set, range or object with `__len__` |
| `str(v)` / `repr(v)`      | Text form / quoted, debugging form                |
| `int(v[, base])` / `float(v)` / `bool(v)` | Conversions                       |
| `list(it)` / `tuple(it)` / `dict(...)` / `set(it)` | Build containers from any iterable |
| `range(stop)` / `range(start, stop[, step])` | Lazy integer sequence      |
| `min` / `max(... , key=, default=)` | Smallest / largest                      |
| `sum(it[, start])`, `any(it)`, `all(it)` | Aggregates                         |
| `sorted(it, key=, reverse=)`, `reversed(it)` | New sorted / reversed list    |
| `enumerate(it, start=0)`, `zip(*its)` | Lists of tuples                       |
| `map(f, *its)`, `filter(f, it)` | Lists (`filter(Nil, it)` keeps truthy values) |
| `abs`, `round(x[, digits])`, `divmod`, `pow(a, b[, mod])` | Numbers      |
| `hex`, `oct`, `bin`, `ord`, `chr` | Number/character conversions            |
| `format(v, spec)`         | Apply a format spec                               |
| `isinstance(v, cls)`, `callable(v)`, `typename(v)` | Type checks                |
| `type(v)`                 | Type description, e.g. `<object 'int'>`           |
| `clock()`                 | CPU time in seconds                               |
| `fiber(fn)`, `yield(v)`   | Fibers (see [Fibers](#fibers))                    |
| `globals()`               | The global variable dict                          |
| `compile(src)` / `exec(code)` | Compile and run MyMo source at runtime       |
| `exit(code=0)`            | Flush output and exit                             |

Methods on built-in types:

- **string**: `split(sep?, maxsplit?)`, `splitlines()`, `partition(s)`, `rpartition(s)`, `join(it)`, `format(...)`, `upper()`, `lower()`, `capitalize()`, `title()`, `swapcase()`, `strip(chars?)`, `lstrip`, `rstrip`, `find(sub, start?)`, `rfind`, `index`, `rindex`, `count(sub)`, `contains(sub)`, `replace(old, new)`, `startswith(p)`, `endswith(p)`, `removeprefix`, `removesuffix`, `isdigit`, `isalpha`, `isalnum`, `isspace`, `isupper`, `islower`, `ljust(w, fill?)`, `rjust`, `center`, `zfill(w)`
- **list**: `append(v)`, `extend(it)`, `insert(i, v)`, `pop(i?)`, `remove(v)`, `index(v, start?, end?)`, `count(v)`, `contains(v)`, `sort(key=, reverse=)`, `reverse()`, `clear()`, `copy()`
- **tuple**: `index(v)`, `count(v)`
- **dict**: `get(k, default?)`, `put(k, v)`, `has(k)`, `delete(k)`, `pop(k, default?)`, `popitem()`, `setdefault(k, v?)`, `update(d)`, `keys()`, `values()`, `items()`, `clear()`, `copy()`
- **set**: `add`, `remove`, `discard`, `pop`, `clear`, `copy`, `union`, `intersection`, `difference`, `symmetric_difference`, `update`, `issubset`, `issuperset`, `isdisjoint`
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
| `os`      | `getenv`, `setenv`, `unsetenv`, `getcwd`, `exit`, `system`, `popen`, `exec`, `args`, `executable`, `platform` |
| `io`      | `read`, `write`, `append`, `exists`, `remove`, `mtime`, `isdir`, `listdir`, `mkdir` |
| `json`    | `encode`, `decode`                                                      |
| `sqlite`  | `open`, `run`, `query`, `close`                                         |
| `http`    | `get`, `post`, `put`, `patch`, `delete`, `head`, `request` with `headers=`, `json=`, `form=`, `params=`, `timeout=`; responses have `status`, `ok`, `json`, `body`, `headers`, `error` (libcurl on worker threads; doesn't block `ui`/`mono` handlers) |
| `socket`  | `connect`, `listen`, `accept`, `send`, `recv`, `close`, `gethostname`, `resolve` |
| `server`  | `listen`, `accept`, `accept_nb`, `read_request`, `respond`, `respond_json`, `close` (HTTP/1.1) |
| `runloop` | `nonblock`, `readable`, `writable`, `select`, `ready` (non-blocking I/O) |
| `term`    | `isatty`, `size`, `raw`, `key`, `write` (full-screen terminal apps)    |
| `nodes`   | `spawn`, `send`, `recv`, `kill`, `self_id`, `is_coordinator`, `children` (multi-process messaging) |
| `mono`    | web framework with routing, `sleep`, `wait_readable`; see above (written in MyMo) |
| `ui`      | app UI library; see [Building apps with `ui`](#building-apps-with-ui) (written in MyMo) |
| `strutil` | string helpers such as `starts_with` (written in MyMo)                  |

---

## Writing C extensions

Native modules are shared libraries named `<name>mod.dylib` (macOS) or
`<name>mod.so` (Linux), loaded on first `use`. A builtin receives its
arguments as NaN-boxed `Value`s and returns a `Value`:

```c
#include "mymo_module.h"

static Value greet(MVM *vm, uint argc, Value argv[])
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

> Builtins now take and return `Value`s (`Value argv[]`), and the
> `mymo_is_*` / `mymo_as_*` macros read `Value`s. Extensions compiled
> against older headers must be rebuilt.

---

## Embedding MyMo in C

`include/mymo.h` lets a C program host MyMo, as you would host Lua. You can
run scripts, call MyMo functions and closures, expose C functions, and pass
values back and forth.

```c
#include "mymo.h"
#include <stdio.h>

static Value host_add(MVM *vm, uint argc, Value argv[])
{
    long a, b;
    if (!mymo_parse(vm, "host_add", argc, argv, "ii", &a, &b))
        return MYMO_ERROR;
    return MYMO_INT(a + b);
}

int main(void)
{
    MVM *vm = mymo_new();
    mymo_define_function(vm, "host_add", host_add);
    mymo_run_string(vm, "fn twice(x):\n    return host_add(x, x)\n", "setup");

    Value twice, out, arg = MYMO_INT(21);
    mymo_get_global(vm, "twice", &twice);
    if (mymo_call(vm, twice, 1, &arg, &out) == MYMO_OK)
    {
        long n;
        mymo_val_as_long(out, &n);
        printf("%ld\n", n);                      // 42
    }
    else
        printf("error: %s\n", mymo_last_error(vm));
    mymo_free(vm);
}
```

```sh
make lib
cc -Iinclude -I. host.c libmymo.a -lm -lcurl -lsqlite3 -o host
```

- `mymo_run_string` / `mymo_run_file` run top-level code. Globals persist
  between runs.
- `mymo_call` calls any callable and returns when it finishes. It also works
  from inside a C builtin, to invoke a MyMo callback. Errors come back as
  `MYMO_RUNTIME_ERROR`, with the message in `mymo_last_error`. After an
  error the VM is still usable.
- Values are NaN-boxed: ints, doubles, nil and bools are stored inline, and
  everything else is a GC-managed object. The collector only runs while MyMo
  code executes. To keep a value across calls, `mymo_retain` it (and
  `mymo_release` it when done).
- Values can be built with `mymo_string`, `mymo_list`, `mymo_dict` and
  `MYMO_INT`, and read with the `mymo_val_*` functions.

`examples/embed/host.c` (`make embed-example`) shows all of this, including
callbacks, closures kept across garbage collections, and error handling.

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
  variable access. Constant operands are one byte. An index above 255 gets
  an `OP_WIDE` prefix carrying the high byte, so small programs pay
  nothing for it.
- **Fibers** each own an operand stack and call-frame chain. Switching
  fibers swaps `vm->fiber`.
- **Closures** are per-call copies of a function bound to their defining
  frame. A captured frame outlives its call.
- **Garbage collection** is mark-sweep (`gc.c`). It runs at safe points in
  the dispatch loop (loop back-edges and calls) once the live object count
  doubles, so C code never has to protect its temporaries. Intern tables
  are weak. A long-running server stays at constant memory.
- **Bytecode cache**: `cache.c` writes `.myc` files, which are keyed on a
  hash of the source and the interpreter build.

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
- `make test` runs every example twice: once compiled, and once loaded
  from `__mycache__`.
- `MYMO_GC_STRESS=1` collects garbage at every safe point. Combine it with
  an AddressSanitizer build to catch GC bugs.

---

## Known limitations

MyMo is an experimental language. Current gaps:

- A single function (including a script's top level) can hold at most
  65,536 distinct constants (names and literals). Beyond that there is a
  compile error.
- Ints are 32-bit when stored inline. Wider values fall back to slower heap
  objects.
- `mono` has no built-in TLS (see [HTTPS](#https)), and its non-blocking
  I/O isn't implemented on Windows yet.
- A fiber can't `yield` across a C→MyMo `mymo_call` boundary.
- `ui` serves one shared app state (no per-user sessions), and its browser,
  desktop and phone targets need non-blocking sockets, which aren't
  implemented on Windows yet. `--term` works there.
- Case mapping covers Latin, Greek, Cyrillic, Armenian and Georgian;
  other scripts' letters are uncased (`isalpha()` still knows CJK, Arabic,
  Hebrew, Indic and more).
- There are no big integers: past 64 bits, arithmetic switches to doubles.
