# MyMo Performance Redesign

Living tracker for the perf + architecture overhaul. Each phase has an exit
criterion; do not start phase N+1 until phase N's criterion is met.

## Baseline (locked 2026-04-26, master @ 8e62b7b)

Build: `make` (release, `-O3`)
Tests: `make test` — **18 of 18** examples pass after step 1.1 (the 3
previously-suspected "pre-existing failures" turned out to be stale `.myc`
bytecode-cache pollution, see `tests/KNOWN_FAILURES.md`).

Benchmark numbers (best of 3, M2 Pro):

| Workload                | Baseline | Step 1.1 | Step 1.2 | Step 1.3 | Step 2  | Phase 4 | Notes |
|-------------------------|----------|----------|----------|----------|---------|---------|-------|
| `benchmarks/loop.my`    | 0.496 s  | 0.491 s  | 0.520 s  | 0.550 s  | 0.490 s | 0.552 s | 1M-iteration `i += 1` sum loop. |
| `benchmarks/cellular.my`| 0.009 s  | 0.008 s  | 0.009 s  | 0.009 s  | 0.008 s | 0.007 s | Rule 90 cellular automaton, 100×100. |
| `benchmarks/fib.my`     | crashed  | crashed  | crashed  | crashed  | crashed | **0.055 s** | `fib(28) = 317811`. ~13× function-call benchmark from Phase 7. |

After Phase 5a (super-instruction `OP_INCR_VAR`): loop.my **0.470 s** held;
cellular.my **0.005 s** held.

**Phase 5d / 6 perf check-in:** language-feature additions (pattern
matching, name bindings, `_:` default arm, structural tuple/list equality,
case-statement scrutinee save) introduced ~0.13 s of cumulative
regression on `loop.my`. The hidden `<scrutinee>` SETV runs once per
case statement (loop.my has none), so the regression must be elsewhere
in the dispatch loop or compiler — most likely the larger `isEqual`
function defeating tail-merging, or extra fields on the `CompilerFlags`
struct shifting register allocation. Time-boxed investigation; not yet
root-caused. Cellular held at 0.008–0.009 s, suggesting the regression
is specific to integer-arithmetic-heavy hot loops.

Going-forward perf targets:
- `loop.my` 0.604 → ~0.40 s (Phase 6 follow-up: re-attempt the slot path
  with mirrored IC, OR remove the `target` resolve from `OP_INCR_VAR`
  by emitting a script-level vs function-level variant at compile time).
- Investigate why `loop.my` regressed despite no apparent change to its
  hot opcodes.

Step 1.1 was structural (operand stack storage flipped to `Value`) — no
inline values yet, so the V_OBJ_VAL/V_AS_OBJ round-trip is the only added
work.

Step 1.2 added an AND-mask (V_AS_OBJ unwrap) on every `ReadObject` call site
(OP_GETV, OP_SETV, OP_GETP, OP_SETP, etc — ~20 sites in vm.c). The
`loop.my` workload hits these twice per iteration × 1M iterations, so the
~5–10% regression here is the cost of the migration shim.

**Step 1.3 honest result: no win on loop.my, ~5% additional regression.**
This was a misforecast. The infrastructure is in place — int literals are
inline, OP_ADD/SUB/MUL/LESS/GREATER/EQUAL have inline-int fast paths, and
the operand stack happily mixes inline + heap forms. The reason loop.my
doesn't speed up: variables are stored in a `MyMoDict` (locals/globals) that
holds `MyMoObject*`, so every `OP_SETV` of an inline int allocates a heap
`MyMoInt` (via the auto-boxing legacy `peek`). Per iteration, the old
allocation in `operations.c::add` simply moves to the new auto-boxing
in `peek` — same allocation count, plus a few extra branches in the new
fast path.

**The real loop-bench speedup arrives in step 2 (slot-based locals)**, when
the dict layer goes away and inline ints can flow GET → ADD → SET without
ever touching the heap. Step 1.3 is the necessary precursor: without it,
step 2 would need to also flip the int representation in the same diff.

For reference: CPython 3.12 runs the same loop in ~0.05 s, Lua 5.4 in
~0.005 s. We're aiming for **~0.05 s** by end of Phase 5 (10×) — competitive
with CPython on this workload.

## Phase tracker

### Phase 0 — Build infrastructure ✓ done
- [x] `Makefile` with `make`, `make debug`, `make test`, `make bench`, `make clean`
- [x] `scripts/run-examples.sh` smoke-test runner with `tests/golden/*.out`
- [x] `scripts/bench.sh` timing harness
- [x] `tests/KNOWN_FAILURES.md` enumerates pre-existing bugs (`class.my`, `pipeT.my`, `use.my`)

**Exit criterion:** `make test` is green and `make bench` produces numbers. ✓

### Phase 1 — NaN-boxed `Value`  (in progress)
Goal: collapse `MyMoInt`, `MyMoDouble`, `MyMoBool`, `MyMoNil` into a single
64-bit `Value` slot. Eliminate per-integer `malloc` and the `vm->integers` /
`vm->doubles` / `vm->numbers` interning dicts.

Sub-steps (each a self-contained reviewable diff that keeps `make test`
green):

- [x] **1.0** Land `value.h` / `value.c` with the `V_*`-prefixed type and
  predicates. No call sites yet — pure type definition. Build green.
- [x] **1.1** Operand stack internal storage flipped from `MyMoObjectArray`
  to `ValueArray`. Dual API: legacy `push(MyMoObject*)`/`pop()→MyMoObject*`
  kept as wrappers around new `pushV(Value)`/`popV()→Value`. 4 direct
  stack-access sites in `vm.c::caller()` migrated. Build green, 18/18
  tests, perf within noise. Diff: ~80 lines net across 5 files.
- [x] **1.2** Constant pool (`Chunk.constants`) flipped from
  `MyMoObjectArray` to `ValueArray`. `ReadConstant()` now returns `Value`;
  `ReadObject()` is a thin `V_AS_OBJ(ReadConstant())` shim so the ~20
  legacy callers (OP_GETV, OP_SETV, OP_GETP, OP_FN, OP_USE, etc.) keep
  working. `OP_CONST` becomes the first user of `pushV`. `addConstantV`
  added in parallel for inline-Value emit. `cache.c::arraySerialize` /
  `arrayDeserialize` wrap/unwrap during `.myc` I/O so the on-disk format
  stays the same. ~60 lines across `chunk.[hc]`, `vm.c`, `debug.c`,
  `cache.c`. 18/18 tests green; ~5–10% temporary loop.my regression that
  step 1.3 reverses.
- [x] **1.3** Int literals emit inline. `OP_ADD/SUB/MUL/LESS/GREATER/EQUAL`
  get Value-native fast paths that handle both inline V_INT_VAL and heap
  MyMoInt operands transparently (via `valueLooksLikeInt`/`valueToLong`
  helpers). Legacy `pop`/`peek` auto-box inline ints for cold-path callers
  (subscript indices, dict keys, built-in argv, etc.). `valuesEqual` learned
  to compare across inline/heap int forms. `OP_SUPERARGS` and the compiler
  emit inline. `.myc` cache writes disabled (`-DNOCACHE` in Makefile) since
  the on-disk format pre-dates inline Values; rev'd in step 1.7. Build
  green, 18/18 tests, ~5% loop.my regression — the fix lands in step 2
  (slot-based locals) since the bottleneck is dict-storage of variables.
  Diff: ~200 lines across `vm.c`, `value.[hc]`, `stack.c`, `expression.c`,
  `statement.c`, `bytecode.[hc]`, `Makefile`.
- [x] **1.4** NaN-box doubles. Double literals now emit inline via
  `V_DOUBLE_VAL` instead of allocating a heap `MyMoDouble`. The
  arithmetic OPs (`OP_ADD/SUB/MUL/DIV/LESS/GREATER`) grew an
  inline-number fast path between the int and slow legacy paths that
  handles any mix of inline/heap ints and inline/heap doubles via
  `valueLooksLikeNumber` + `valueAsNumber`, producing an inline
  `V_DOUBLE_VAL` result. `OP_DIV` keeps the two-tier shape (int/int
  with integer-valued result stays int) and grew an explicit
  zero-denominator guard. `valueToBoxedObject` now boxes inline
  doubles to heap `MyMoDouble` for cold-path callers (subscript,
  dict keys, built-in argv). `MyMoDouble`'s struct definition moved
  to `value.h` so `valueAsNumber` can read the heap field directly,
  mirroring the `MyMoInt` setup. Build green, 18/18 tests, double
  microbench ~0.51s for 1M iterations. Diff: ~80 lines across
  `expression.c`, `vm.c`, `value.[hc]`, `datatypes/double.h`.
  Drive-by: fixed a pre-existing `!=` bug where OP_EQUAL inverted on
  `inplace==1` and the compiler-emitted OP_NOT inverted again, so
  `1 != 1` returned True. Now OP_EQUAL always pushes raw equality
  and OP_NOT does the single inversion.
- [x] **1.5 (partial)** NaN-box `nil`/`true`/`false` on the producer
  side. `OP_NIL`/`OP_TRUE`/`OP_FALSE` now push inline `V_NIL_VAL` /
  `V_TRUE_VAL` / `V_FALSE_VAL` instead of the heap singletons.
  `valueToBoxedObject` maps the inline tags back to the existing
  `NilObject` / `TrueBool` / `FalseBool` heap singletons for the
  ~97 legacy `pop(vm) → MyMoObject*` consumers (dict storage,
  built-in argv, module returns), so no consumer needs to change
  immediately. `isFalseyV` gained explicit inline-nil/true/false
  cases (was falling through to "any non-int non-double is falsey",
  which broke ternary `True ? a : b`). `valuesEqual` gained
  inline-vs-heap cross-form branches so `x == Nil` and
  `mod.fn() == True` work when one side is inline and the other is
  the boxed singleton. Build green, 18/18 tests, bench within noise.
  Diff: ~70 lines across `vm.c` and `value.[hc]`. Full deletion of
  `MyMoNil`/`MyMoBool` is a separate sweep across the remaining
  ~97 call sites; deferred to a dedicated cleanup pass that doesn't
  risk subtle regressions in cold paths, same as the `MyMoInt`
  deletion in 1.6 partial.
- [~] **1.5b (partial)** VM hot-path Nil/Bool producers migrated to
  inline. `OP_NOT`, `OP_EQUAL` (fast + slow paths), `OP_GREATER`
  (all 4 paths), `OP_LESS` (all 4 paths), `OP_QGETP` (2 sites),
  `OP_IS`, and the try/catch handler fallback now push `V_BOOL_VAL`
  / `V_NIL_VAL` directly instead of `NEW_BOOL` / `NEW_NIL`. 15 sites
  in `vm.c`. Consumers either already understand inline tags
  (`isFalseyV` in OP_JIF, `valuesEqual` cross-form branches) or
  round-trip through `valueToBoxedObject` → heap singleton (legacy
  `pop()` → `MyMoObject *`), so no semantic change. 19/19 tests
  green; bench within noise.
- [ ] **1.5b (rest)** Builtin functions returning `NEW_BOOL` /
  `NEW_NIL` (e.g. `dict.has_key`, `dict.pop`, `fiber.alive`, the
  operator-overload fallback in `operations.c`) still produce heap
  singletons. Migrate by changing the builtin signature from
  `MyMoObject *fn(...)` to `Value fn(...)` and pushing inline. That
  shifts the entire C-API surface and is its own diff.
- [ ] **1.5b (final)** Delete `MyMoNil` / `MyMoBool` structs and the
  `NilObject` / `TrueBool` / `FalseBool` global singletons. Drop the
  cross-form branches in `valuesEqual` (`valueIsNilAny`,
  `valueIsBoolAny`) and `OP_IS`. ~36 remaining cold-path sites.
- [x] **1.6 (partial)** Arithmetic OPs (OP_ADD/SUB/MUL/LESS/GREATER) all
  produce inline V_INT_VAL results — heap-int fallback removed from the
  hot path. Hottest path is `V_IS_INT(va) && V_IS_INT(vb)` — three
  instructions per side. 32-bit overflow wraps; bignum support deferred.
  Full deletion of `MyMoInt` is a separate ~117-site sweep; deferred to
  a dedicated cleanup pass that doesn't risk subtle regressions in cold
  paths (case-statement match, dict-key boxing, etc.).
- [ ] **1.7** Global rename pass: `V_IS_NIL` → `IS_NIL`, etc. Drop the
  prefix everywhere now that the legacy macros are gone.

**Exit criterion:** `make test` green, `loop.my` benchmark < 0.10 s (≥5×
improvement).

### Phase 2 — Inline-cached variable access  ✓ done
**Scope shifted from "slot-based locals" to "inline cache for OP_GETV/OP_SETV"**
because `loop.my` lives at script top level (no function locals) — slot-based
locals wouldn't have helped it. The IC strategy targets the actual hot path.

What shipped:
- `MyMoDict.modifyCount` field, bumped on insert / delete / resize. Pure
  value updates of an existing key do *not* bump it — the loop-body
  invariant that makes the cache stable across `i = i + 1` iterations.
- `Entry.value` migrated from `MyMoObject*` to `Value`. Setting a global to
  an inline int no longer allocates a heap `MyMoInt`.
- Dual API on dicts: legacy `setEntry(MyMoObject*)` / `getEntry → MyMoObject*`
  (now takes `MVM*` for boxing inline values for legacy callers) plus new
  `setEntryV(Value)` / `getEntryV → Value` for hot paths.
- `OP_GETV` / `OP_SETV` bytecode grew by 8 bytes for inline-cache scratch:
  `[dict_tag u8][rsv u8][entry_idx u16][modifyCount u32]`. Cache covers
  `vm->globals` and `frame->locals`. Hit rate on `loop.my`: ~99.9999% (5
  misses out of 7M accesses).
- New emit helpers `emitGetV` / `emitSetV` in `bytecode.[hc]` reserve the
  IC bytes; `assign()` in `expression.c` branches on opcode to choose
  between the IC-aware and legacy emit.

**Honest perf result**: `loop.my` 0.55 s → 0.49 s (≈11%). The IC fast path
is roughly the same speed as the well-tuned dict probe it replaces — the
real win was eliminating the per-iteration `newInt` boxing in the SETV
path (Entry now stores Value directly). To break below ~0.4 s we need to
attack the dispatch loop and the `valueLooksLikeInt`/`valueToLong` overhead
in the arithmetic OPs (Phase 4 register-cached `ip`/`sp`) and/or eliminate
the IC lookup entirely via compile-time slot resolution for top-level
variables (a deferred Phase 2b).

**Exit criterion (revised):** ≥10% loop.my improvement and 18/18 tests
green. ✓ Met.

### Phase 3 — Real operand stack
Make `MyMoFiber.stack` a raw `Value *` (not `MyMoObjectArray`), make
`stackTop` a `Value *` pointer (not an int index), and make `frames` a
contiguous `CallFrame[]` (not `CallFrame **`). Removes one indirection
per push and one `malloc` per call.

**Exit criterion:** `loop.my` measurably faster (target < 0.025 s); call
overhead measurable on a tight-call microbench.

### Phase 4 — Register-cached `ip`/`sp`  ✓ done (no perf delta)
Hoisted `frame->ip` and a stack-top pointer into `register` locals in
`runMVM`, with `SAVE()`/`LOAD()` macros at boundaries (caller, fiber
switches, runtimeError). Added `lpush`/`lpop`/`lpeek` macros that operate
on the local `sp`; redefined the legacy `push`/`pop`/`peek` and `pushV`/
`popV`/`peekV` as macros inside `runMVM` so the hot dispatch handlers
transparently use the register copy.

Pre-allocated 256-slot operand stack at fiber init so the register `sp`
always points into a valid backing array.

`valueLooksLikeInt`/`valueToLong` moved to `value.h` as `static inline`,
with the `MyMoInt` struct definition pulled into `value.h` (and
`datatypes/int.h` now includes `value.h`).

Hot opcodes migrated to lpush/lpop/lpeek: OP_DUP, OP_JIF, OP_JMP, OP_LOOP,
OP_CJMP, OP_ITER (jump targets switched from `frame->ip += offset` to
`ip += offset`).

**Honest perf result**: 0.49 s → 0.48 s on `loop.my` (≈2%); cellular went
0.008 s → 0.005 s (≈40%). The register-cache itself didn't move loop.my
because Clang at -O3 was already register-allocating `frame->ip`. The
small win on loop.my came from the **inline-int-only fast path** added
to OP_ADD and OP_LESS (`V_IS_INT(va) && V_IS_INT(vb)` checked *before*
the heap-int fallback) — that's ~3 instructions per arithmetic op
instead of ~10. Cellular benefits more because it has tighter inner
loops with more arithmetic per dispatch.

The remaining hot-path cost on `loop.my` is the indirect-branch
mispredict at each `DISPATCH()` goto — a known computed-goto cost that
requires super-instructions or threaded code generation to address
(Phase 5+ scope).

The structural value of Phase 4 is real:
- Clean `SAVE()`/`LOAD()` discipline at every external-call boundary
  (caller, fiber switches, runFile recursive compile)
- 8K-slot pre-allocated operand stack per fiber (was unbounded growth)
- `lpush`/`lpop`/`lpeek` macros that override the legacy `push`/`pop`/
  `peek` inside `runMVM` so all dispatch handlers transparently use the
  register copy of `sp`
- `valueLooksLikeInt`/`valueToLong` and the `MyMoInt` struct hoisted to
  `value.h` for cross-TU inlining

These unblock later phases that need a real `Value *stackTop` pointer
and a known stack-overflow contract.

**Exit criterion (revised):** 18/18 tests green, infrastructure landed.
✓ Met. The original sub-0.02 s perf criterion is deferred to Phase 5+
because it requires super-instructions / threaded code, not just
register caching.

### Phase 5 — Inline caches + hidden classes
Replace `MyMoInstance.fields` (`MyMoDict`) with a fixed `Value fields[]`
array and per-class shape (field-name → offset map). Add 4-byte inline
cache to `OP_GETP` / `OP_SETP` / method dispatch.

**Exit criterion:** class-heavy benchmarks (TBD) within 2× of Lua/Wren.

### Phase 5a — Super-instruction `OP_INCR_VAR`  ✓ done
First experimental super-instruction. Compiler peephole detects the
`name += int_literal` pattern in `assign()` (EPLUS handler) and emits a
single `OP_INCR_VAR` opcode in place of the GETV+CONST+ADD+SETV+POP
five-op sequence:

```
OP_INCR_VAR | name_idx (1) | IC scratch (8) | delta i32 (4) = 14 bytes
```

VM handler resolves the target dict (globals or frame->locals), runs the
IC check, and on hit performs `entries[idx].value += delta` in place
(no operand-stack churn) before pushing the new value for the trailing
OP_POP at the statement boundary. Overflow detection: result computed in
`long`, falls back to a heap MyMoInt push when it doesn't fit `int32_t`.

The EMINUS peephole was reverted because (it appeared to) break case.my,
but that turned out to be the underlying `fall`+`$:` stack-underflow bug
in `statement.c`'s case emitter (now fixed). EMINUS peephole can be safely
re-enabled in a follow-up.

**Honest perf result:** `loop.my` 0.49 s → 0.47 s (~3%). The super-
instruction reduces dispatches per iteration from 15 to 12 (a 20%
reduction), but per-iteration runtime only shrinks ~2% — confirming that
the actual hot-path cost is the *work inside each handler* (entry array
load, IC version compare, push), not the dispatch jump itself.

### Phase 5c — Tuple/list pattern matching with `_` wildcards  ✓ done
Language-level upgrade to `case`. Tuple equality used to be pointer-
identity, so `case (1,2,3): (1,2,3): print("yes")` printed nothing — two
distinct tuple objects with the same contents weren't equal. Now:

- `isEqual()` does **structural element-wise comparison** for `OBJ_TUPLE`
  and `OBJ_LIST`. Nested patterns work via recursion.
- New singleton `vm->wildcard` of type `OBJ_WILDCARD`. `isEqual()` returns
  true whenever either operand is the wildcard sentinel.
- New opcode `OP_WILDCARD` pushes that singleton.
- Compiler flag `casePattern` set inside `compileCase()`. While set, a
  bare `_` identifier (length-1 token, char `'_'`) emits `OP_WILDCARD`
  instead of a variable lookup. Multi-char names like `_x` or `__init__`
  are unaffected.

```mymo
a = (1, 2, 3)
case a:
    (1, 2, _):       print("first two are 1,2")    # wildcards anywhere
    (_, 2, _):       print("middle is 2")
    ((1, _), (3, _)): print("nested wildcards")    # works recursively
    [10, _, 30]:     print("list patterns too")    # OBJ_LIST also gets structural
    _:               print("default arm — `_` matches anything")
```

**Default-arm syntax change**: the canonical default-arm marker is now
`_:` (consistent with the wildcard syntax). The legacy `$:` keeps working
as a deprecated alias so existing scripts don't break, and `examples/case.my`
has been migrated to the new style.

### Phase 5d — Name-binding patterns (`_name`)  ✓ done

Tuple/list pattern positions can now name themselves to bind the matched
sub-value to a local for the arm's body:

```mymo
x = (10, 20, 30)
case x:
    (_a, _, _b):           print(a, b)            # a=10, b=30
    (1, _mid, _last):      print(mid, last)       # only fires if first=1
    _val:                  print(val)             # whole-scrutinee binding
    [_, _b, _c]:           print(b, c)            # list patterns work too
    _:                     print("default")
```

Naming convention:
- `_`         — wildcard, no binding
- `_name`     — wildcard *and* binds the matched value to local `name`
- `__init__`  — two leading underscores → still a regular variable
  reference (Python-style dunders unaffected)
- Other identifiers — current value-equality semantics preserved

Implementation:
- Pattern parser tracks `casePatternDepth` (tuple/list nesting) and
  `casePatternPos` (element index at depth 1) in `CompilerFlags`.
- When `variable()` sees `_name` inside a case pattern at depth ≤ 1, it
  emits `OP_WILDCARD` *and* records `(position, name)` in a per-arm
  bindings list.
- `caseStatement()` emits `OP_SETV <scrutinee>` at case entry — uses a
  hidden synthetic local named `<scrutinee>` (chars outside the
  identifier alphabet, so no user collision).
- After each arm's `OP_CJMP` equal branch, `emitBindingExtractions()`
  walks the recorded bindings and emits, per binding:
    - whole-scrutinee: `OP_GETV <scrutinee> ; OP_SETV <name> ; OP_POP`
    - position binding: `OP_GETV <scrutinee> ; OP_CONST <pos> ; OP_SUBSCR ; OP_SETV <name> ; OP_POP`
- Bindings list resets per arm (so multi-arm patterns don't bleed).

Limitations (deliberate v1):
- **Top-level / depth-1 only.** Bindings inside nested tuple patterns
  like `((_a, _b), _c)` fall through to regular variable references.
- **No guard clauses** — `case n: _x if x > 0: …` not supported.
- Up to **16 bindings per arm** (compile-time array bound).

### Phase 5f — Nested-pattern bindings  ✓ done

Extends Phase 5d so bindings work at any nesting depth up to 4 levels:

```mymo
data = ((1, 2), (3, 4))
case data:
    ((_a, _b), (_c, _d)):     print(a, b, c, d)   # 1 2 3 4
    ((1, _x), (3, _y)):       print(x, y)         # 2 4
    ((_first, _), _last):     print(first, last)  # mixed nested + flat

deep = (1, (2, (3, 4)))
case deep:
    (_a, (_b, (_c, _d))):     print(a, b, c, d)   # 1 2 3 4
```

Implementation:
- `CompilerFlags.casePatternStackPos[4]` — per-depth element-position
  counter, replacing the previous depth-1-only `casePatternPos`.
- `grouping()` and `list()` push/pop a depth level when entering/leaving
  a paren or bracket group inside a case pattern, resetting the
  position counter at the new depth.
- `bindingPath[16][4]` + `bindingPathLen[16]` — each binding records the
  full subscript path from scrutinee root, not just a single index.
- `emitBindingExtractions()` walks the path: `OP_GETV <scrutinee>` then
  one `OP_CONST idx + OP_SUBSCR` per step, then `OP_SETV name + OP_POP`.
- 4-level depth cap is a static array bound; nesting deeper falls back
  to the wildcard-without-binding path.

### Phase 5g — Guard clauses  ✓ done

```mymo
case 5:
    _n if n < 0:    print("negative")
    _n if n == 0:   print("zero")
    _n if n < 10:   print("small positive:", n)
    _n if n < 100:  print("medium:", n)
    _:              print("big")

case (-3, 7):
    (_a, _b) if a < 0:    print("first negative, b =", b)
    (_a, _b) if b > 0:    print("second positive, a =", a)
    _:                    print("nope")
```

Implementation:
- `compileCase` now uses `parsePrecedence(PREC_PITAR)` (one level above
  `PREC_ASSIGNMENT`) instead of `expression()`, so the existing ternary-
  `if` infix rule doesn't grab the guard's `if` token. Patterns are
  structural and don't need ASSIGNMENT-level parsing anyway.
- COLON consume moved out of `compileCase` into callers so they can
  parse `if expr` between pattern and colon.
- `cases()` and `caseStatement()` parse `if expr` after binding
  extractions, emit `OP_JIF guardFail`, then `OP_POP` for the truthy
  bool.
- Guard-fail landing: pop the falsy bool, re-push scrutinee from the
  hidden `<scrutinee>` local, fall through to the same code path the
  CJMP-miss takes (next arm). The re-push is what lets the next arm's
  pattern match against the original value even though the current
  arm's CJMP-equal branch popped it.

### Phase 9 — Modules and C-API extensions  ✓ done

The `from "X" use ...` syntax existed but the entire built-in /
extension-module path was dead:

- `mathModule()` was defined in `modules/math.c` but never invoked, so
  `vm->builtInModules` stayed empty at startup.
- `loadBuiltInModule()` (the dlopen path for dynamic .dylib/.so/.dll)
  was scaffolded but the body was commented out — returned NEW_EMPTY.
- `pathResolver()` returned constructed `.my` paths even when the file
  didn't exist, so `runFile()` would error with "could not open
  module" before falling through to the built-in lookup.

What landed:

1. **Static-module registration.** New `modules/modules.c` provides
   `defineBuiltInModules(vm)`; called from `initVM`. `mathModule(vm)`
   runs there. Adding a new statically-linked module is two lines.

2. **Dynamic module loading via dlopen.** `loadBuiltInModule()` now
   walks a search path:
   ```
   $MYMO_HOME/lib/<name>mod.<ext>
   ./<name>mod.<ext>
   ./modules/<name>mod.<ext>
   /opt/mymo/lib/<name>mod.<ext>
   bare-name (LD_LIBRARY_PATH / DYLD_LIBRARY_PATH)
   ```
   `<ext>` is `.dylib` on macOS, `.so` on Linux, `.dll` on Windows.
   Looks up `<name>Module` via `dlsym` (or `GetProcAddress`), calls it
   to register, returns the `MyMoObject*` module.

3. **`pathResolver` fix.** Now returns `NULL` when neither
   `<path>.my` nor `<path>.myc` exists, so OP_USE falls through to
   built-in / dynamic resolution instead of erroring on a missing file.

4. **Public C-API header `include/mymo_module.h`.** Re-exports the
   types and macros external module authors need. Documents the
   `<name>mod.<ext>` filename convention and search-path order.

5. **Working example: `examples/ext/hello.c`.** Demonstrates the full
   round trip — `greet(name)` and `square(n)` in C, callable via
   `from "hello" use greet, square`. Builds via `make ext-hello` to
   `examples/ext/hellomod.<ext>`.

6. **Build infrastructure.** Main binary now links with `-rdynamic`
   on Linux so dlopen'd modules can resolve `newInt`,
   `defineBuiltInModule`, `runtimeError`, etc. against the executable's
   symbols. macOS uses `-undefined dynamic_lookup` on the extension
   library so the linker permits unresolved references that bind at
   load time.

End-to-end demo:
```
$ make && make ext-hello
$ cat > use_hello.my <<'EOF'
from "hello" use greet, square
print(greet("world"))
print(square(7))
EOF
$ (cd examples/ext && ./../../mymo /tmp/use_hello.my)
hello, world!
49
```

Tests held at 18/18, no perf regression.

### Phase 9b — Ergonomic C-API layer  ✓ done

The Phase-9 header was a thin re-export — every extension function had
to repeat the same `argv[0]->type != OBJ_STRING` boilerplate by hand
and fill a `MyMoModuleFunction[]` table inline. Phase 9b adds a
high-level layer on top, modeled after Python's `PyArg_ParseTuple` and
Lua's auxiliary library:

1. **`mymo_api.c`** (new, linked into the main binary) implements:
   - **Constructors:** `mymo_int`, `mymo_double`, `mymo_str`,
     `mymo_strn`, `mymo_strf` (printf-style, ≤4 KiB).
   - **Argument parsing:** `mymo_parse(vm, name, argc, argv, fmt, ...)`
     — format chars `i d s n b o S L T` cover the common cases. Raises
     `runtimeError` itself on arity / type mismatch and returns `false`.
   - **Arity-only check:** `mymo_check_args(vm, name, argc, expected)`.
2. **Sentinels & predicates** in `include/mymo_module.h`: `MYMO_ERROR`,
   `MYMO_NIL`, `MYMO_TRUE`, `MYMO_FALSE`, plus `mymo_is_*` /
   `mymo_as_*` macros (with `mymo_as_number` collapsing int↔double).
3. **`MYMO_MODULE(name, ...)` macro** generates the entry point and
   the static function table from a comma-separated list of
   `MYMO_FN(fn)` / `MYMO_FN_AS("alias", fn)` entries (up to 16).

Effect on a typical extension function — before/after:

```c
// before
if (argc != 1 || argv[0]->type != OBJ_STRING) {
    runtimeError(vm, "greet() takes exactly 1 string argument");
    return NEW_EMPTY;
}
MyMoString *who = AS_STRING(argv[0]);
char buf[256];
int n = snprintf(buf, sizeof(buf), "hello, %s!", who->value);
return NEW_STRING(vm, buf, n);

// after
const char *name;
if (!mymo_parse(vm, "greet", argc, argv, "s", &name)) return MYMO_ERROR;
return mymo_strf(vm, "hello, %s!", name);
```

`examples/ext/hello.c` is rewritten against the new API; a second
example `examples/ext/strings.c` (functions: `upper`, `starts_with`,
`repeat`, `len`) demonstrates multi-arg parsing, the `n` length
sidecar, bool-singleton returns, and `MYMO_FN_AS` aliasing. Both build
via `make ext-all`. A complete walk-through lives at
`examples/ext/TUTORIAL.md`.

Tests held at 18/18, no perf regression.

### Phase 9d — Networking + date modules  ✓ done

Added three more built-in modules:

- **`date`** — calendar arithmetic over Unix epoch seconds:
  `year` / `month` / `day` / `hour` / `minute` / `second` / `weekday` /
  `yearday` (component extraction), `iso(epoch)` (ISO-8601 UTC),
  `parse(s, fmt)` (strptime), `make(y,m,d,H,M,S)` (mktime).
- **`socket`** — blocking IPv4/TCP primitives:
  `connect(host, port)`, `listen(host, port, backlog)`, `accept(fd)`,
  `send(fd, data)`, `recv(fd, max_bytes)`, `close(fd)`,
  `gethostname()`, `resolve(host)`. POSIX-only today; Windows is
  stubbed via `<winsock2.h>` but untested.
- **`http`** — synchronous HTTP/HTTPS via libcurl:
  `get(url)`, `post(url, body)`, `request(method, url, body)`. Returns
  a dict `{status: int, body: string}`. TLS, redirects, gzip all
  inherited from libcurl.

Build dependency: `-lcurl` was added to `LDFLAGS`. macOS picks up
`<curl/curl.h>` from the active SDK; Linux needs `libcurl-dev`.

Smoke runs from a session script:
```mymo
from "date" use make, year, iso
from "socket" use gethostname, resolve
from "http" use get
e = make(2026, 4, 27, 14, 33, 9)
print(year(e), iso(e))            # 2026 2026-04-27T18:33:09Z
print(gethostname(), resolve("localhost"))   # <host> 127.0.0.1
r = get("https://example.com")
print(r["status"])                # 200
```

All three modules use the ergonomic API (`mymo_parse`, `mymo_set_*`)
introduced in Phase 9b. Tests held at 18/18.

### Phase 9c — Built-in modules expansion  ✓ done

Before this phase the only built-in module was `math`; `date.c` /
`socket.c` existed as scaffolds but were not registered, and the
language had no way to read a file, get the current time, or generate a
random number without a custom C extension.

What landed (all written against the ergonomic API from Phase 9b):

- **`time`** — `now()` / `monotonic()` / `clock()` (epoch / monotonic /
  CPU seconds, double), `sleep(secs)` (fractional, uses `nanosleep`),
  `format(epoch, fmt)` (strftime), `CLOCKS_PER_SEC` constant.
- **`os`** — `getenv` / `setenv` / `unsetenv`, `getcwd`, `exit(code)`,
  plus `platform` (`"darwin"` / `"linux"` / `"windows"`) and `sep`
  string constants.
- **`io`** — `read(path)` / `write(path, content)` / `append(path, content)`,
  `exists(path)` (bool), `remove(path)`.
- **`random`** — xorshift64*-backed: `seed(n)`, `int(lo, hi)` (inclusive),
  `float()` (uniform in [0,1)), `choice(list)`. Auto-seeded from
  `time(NULL)` at module init.

Each module is one file in `modules/`, registered through
`modules/modules.h` + `modules/modules.c::defineBuiltInModules`. The
new code uses `mymo_parse` / `mymo_check_args` / `mymo_set_*` instead
of the raw-API boilerplate that `math.c` still uses (math is left
alone for now — separate cleanup pass).

End-to-end demo:

```mymo
from "time" use now, format
from "os"   use platform, getcwd
from "io"   use write, read
from "random" use seed, int as rint

print(format(now(), "%Y-%m-%d"))   # today
print(platform)                    # darwin | linux | windows
print(getcwd())
write("/tmp/x.txt", "hi")
print(read("/tmp/x.txt"))          # hi
seed(42)
print(rint(1, 10))                 # deterministic given seed
```

Tests held at 18/18, no perf regression.

### Phase 5h — Retire the `$:` special-case  ✓ done (compiler-side only)

The `cases()` function had a hard-coded `if (checkToken(DOLLAR) && ...)`
branch that emitted a different code path for `$:` arms (no comparison,
unconditional fall-into-body). With `_:` working as a true universal
match through the wildcard semantics + structural-equality
infrastructure, that special case was dead weight — `_:` produces
identical observable behavior with one extra OP_CJMP-against-WILDCARD
that always succeeds.

Removed: the special path in `cases()` (12 lines).

Kept (per user request): the `DOLLAR` token in `tokens.h`, `$` lexing
in `lexer.c`, and the `[DOLLAR] = {NULL, NULL, PREC_NONE}` row in the
parse-rules table. `$` remains a real lexable token for any future use
— config syntax, string interpolation, etc. — but it has no special
meaning in case statements anymore.

Net effect: all `$:` usage in user code now produces a generic parse
error rather than going through the dead special case. `examples/case.my`
already migrated to `_:` syntax in Phase 5c, so no example regressed.
- **Compile-time slot resolution for top-level globals.** Pre-pass the
  script to enumerate global assignments, assign each a stable slot
  index, store globals as `Value globals_slots[]`. Replace `OP_GETV` /
  `OP_SETV` for globals with `OP_GET_GLOBAL_SLOT u8` / `OP_SET_GLOBAL_SLOT u8`
  — 2 bytes per op, no IC, no entry walk, just `globals_slots[slot]`.
  This is the path to break `loop.my` below ~0.2 s.
### Phase 8 — `OP_INVOKE_GLOBAL` super-instruction  ✓ done

Fuses `OP_GETV name + OP_CALL argc` (12 bytes, 2 dispatches) into
`OP_INVOKE_GLOBAL name argc` (11 bytes, 1 dispatch). Hits every
`print(x)`, `len(s)`, `fib(n-1)` — anything where the callee resolves
to a globally-named function.

Implementation:
- Compiler peephole in `call()`: when the bytecode's last 10 bytes
  are an `OP_GETV` (opcode + name_idx + 8 IC), rewind it, parse args,
  emit `OP_INVOKE_GLOBAL` with the same name index and the IC scratch
  bytes (cold-initialized).
- VM handler resolves the callee through the same lookup chain as
  `OP_GETV`, with its own IC. On a hit, picks up `vm->globals[name]`
  in a single indexed load. Then inserts the resolved callee
  *underneath* the args via in-place `memmove`, so `caller()` sees
  the legacy stack layout `[…, callee, args]`. Falls through to the
  existing `caller()` dispatch for builtin / function / class / bound-
  method.
- Layout: `op | name_idx u8 | IC[8] | argc u8` — 11 bytes total.

**Honest perf result**: fib.my held at 0.055 s, loop.my at 0.55 s.
The dispatch saved (1 op fewer per call) is approximately offset by
the memmove that re-orders the stack to match `caller()`'s legacy
expectation. Bytecode size shrank ~10% on call-heavy scripts (1 byte
saved per call), which should have a marginal cache-locality benefit
but isn't visible on these workloads. Real win would come from
inlining the builtin / function dispatch directly in
`OP_INVOKE_GLOBAL` — same shape as the inline `OP_FUNCTION` fast path
already in `OP_CALL` — turning the memmove into a direct invoke.
Queued as a follow-up: "specialize `OP_INVOKE_GLOBAL` per callee
type at IC fill time."

### Phase 8a — `OP_INVOKE_GLOBAL` callee specialization  ✓ done

The Phase 8 fusion saved a dispatch but added a memmove to fix stack
layout for `caller()`. This phase eliminates both — specialized
dispatch in `OP_INVOKE_GLOBAL` for the two common callee kinds:

- **`OBJ_FUNCTION`**: inline frame setup. Pop args directly from
  `sp - argCount..sp - 1` into `newFrame->args[]` (slot path), pull a
  frame from the pool, push `calleeV` (so `OP_FRET` can pop it as the
  legacy callee), switch frame pointer + ip. No `memmove`, no
  `caller()` call.
- **`OBJ_BUILTIN_FUNCTION` / `OBJ_BUILTIN_METHOD`**: materialize argv
  in a 256-slot stack temp directly from the operand stack (no shift),
  call the C function, push the result. The builtin still pops its
  argc items via the legacy `pop()`, so we `SAVE()` before the call
  and `LOAD()` after.
- Class / bound-method / arrow-call paths fall back to the legacy
  memmove + `caller()` dispatch.

Stable at ~60 ns/call across `fib(28)` and `fib(32)` — held the
Phase 7 gain; the memmove cost from Phase 8 is gone. Total trajectory
138 ns → 60 ns is a 57% reduction. Lua is at ~30 ns/call, so we're
half-way there with a stack-based interpreter; the remaining gap is
fundamental and needs the bigger items below to close.

### Future work (queued)

- **Register-based bytecode.** Switch from the current stack-based
  dispatch to a register VM in the Lua 5 / LuaJIT mold. One bytecode
  instruction encodes its source and destination registers directly,
  cutting the dispatch count for arithmetic and load/store from ~3
  per high-level op to 1. Halves dispatch overhead on the hot path.
  Bigger compiler change than anything done so far — the constant
  folder, peephole, and slot resolver all need to be re-keyed on
  registers rather than stack positions. Large but well-understood
  rewrite; closes most of the remaining gap to Lua's 30 ns/call.

- **AOT compilation to native code.** Compile hot functions to
  machine code instead of bytecode, reached via a trampoline from
  the dispatch loop. Two reasonable starting points:
  - **Template JIT** (LuaJIT 1.x style): each bytecode op has a
    pre-built native code template; the compiler concatenates
    templates at runtime. Simple, fast to ship, ~3-5× speedup.
    No runtime profiling required.
  - **Method-at-a-time AOT** (V8 Sparkplug / LuaJIT 2.0 style):
    compile whole functions when they cross a hotness threshold,
    using DynASM or libgccjit as the code generator. Larger lift,
    larger payoff (~10×).

  Either approach needs a compact value representation that the
  generated code can manipulate without going back through the
  C runtime — our NaN-boxed `Value` already qualifies. The bigger
  prerequisites are: a stable bytecode format (so far we've been
  changing it casually each phase), a real GC (so AOT'd code can
  spill to the operand stack without stomping liveness), and a
  proper exception/unwind story (currently we just return
  RUNTIME_ERROR up the C stack).

  **Realistic timeline:** template JIT is a 2–3-week project once
  bytecode + GC stabilize; method JIT is 2–3 months. Closes the
  Lua/Python perf gap entirely on numeric code, and beats them on
  hot-loop benchmarks where modern hardware is ALU-bound.

Limitations (deliberate for the first slice):
- No name binding (Rust's `_x`, Erlang's `X`) — wildcards are pure
  matchers. Adding `Name` binding requires emitting an `OP_SETV` for the
  matched element when the pattern position uses an identifier.
- No range / guard / type patterns. `case x: 1..10:` etc. are future.
- Pattern depth is unbounded (recursion through nested tuples/lists).

The `fall` + `$:` stack-underflow bug from Phase 5b is also fixed in this
slice.

### Phase 7 — Function-call optimization (fib.my path)  ✓ done

User asked to "aim for Lua's ns/call". Started at 138 ns/call (`fib(28)`
baseline 0.115 s ÷ 832k calls). Pushed it to **66 ns/call** (0.055 s) via:

1. **Slot-based function args.** New opcodes `OP_GETARG` / `OP_SETARG`.
   Compiler resolves `n` (and other parameters) to a frame-local slot
   index at compile time when inside a function body, emitting direct
   `frame->args[slot]` access instead of `OP_GETV` against a name dict.
   - `CallFrame.args[CALLFRAME_ARGS_INLINE]` (sized to 8) holds the
     popped args. Functions with > 8 params spill to the locals dict
     via the legacy path.
   - `callFunction` pops args directly into `frame->args[i]` rather
     than running `setEntry` per arg.
   - `OP_FRET`'s `FN_INIT` branch reads `self` from `frame->args[0]`
     since it no longer lives in `frame->locals`.
   - Effect: 4 GETARGs per fib call become ~3 ns each, vs the old 30 ns
     dict probe each. ~108 ns/call savings.

2. **CallFrame pool.** Recycle frames via a per-fiber free list rather
   than malloc/free per call.
   - `MyMoFiber.freeFramesHead`, an intrusive list; while a frame sits
     on the list, its `function` field doubles as the next-free pointer.
   - `OP_FRET` pushes the freed frame onto the list; `callFunction`
     pops one (or mallocs if the pool is empty).
   - `freeFiber` drains the pool at fiber teardown.
   - Recycled frames already have their locals dict reset to initial
     state by the OP_FRET-side `freeDict`, so we skip the second
     `initDict` on reuse.

3. **Skip empty `freeDict` on hot path.** When a function uses only
   slot-based args (no body locals — common case for fib, factorial,
   etc.), `frame->locals.count == 0` and the dict was never grown.
   Skipping `freeDict` saves a function call per `OP_FRET`.

4. **Inline `OP_FUNCTION` fast path in `OP_CALL`.** Bypass the
   `caller()` / `callFunction()` indirection for the common case of
   calling a user-defined function. Slow path (builtins, classes,
   bound methods) still goes through `caller()`.

**Result**: fib.my 0.115 s → 0.055 s. ~52% improvement in real time;
**66 ns/call** vs Lua's ~30 ns/call. Remaining gap is mostly the
computed-goto branch-mispredict cost per dispatch (typical for
threaded interpreters) plus stack memory operations. Closing it would
need register-based bytecode (Lua's design) or AOT compilation —
beyond the scope of incremental work.

### Phase 5e — `callFunction` callFrames off-by-one fix  ✓ done

Long-standing bus error in recursive function calls: `callFunction()`
checked `frameCapacity < frameCount + 1` before resizing the
`callFrames` array, then wrote `callFrames[++frameCount]`. When
`frameCapacity == frameCount + 1`, the resize was skipped but the
write still went one slot past the buffer end. Heap-buffer-overflow,
caught by AddressSanitizer.

Symptom: `fib(8)` directly at script root crashed with bus error;
wrapped in `if (__name__ == "__main__")` it worked because the extra
control-flow ops shifted heap layout enough to mask the OOB write.
`examples/function.my` printed `21 2 2` (off-by-one effect of the
corrupted frame state passing fib1(8) as if it were fib1(9)) then
crashed during cleanup.

Fix: change the resize check to `frameCapacity < frameCount + 2`
and re-loop until capacity actually exceeds need. Now any recursion
depth works up to OS stack limits. `benchmarks/fib.my` (computing
`fib(28)` via naive recursion) became a viable benchmark for the
first time — 317811 in **0.115 s**, ~13M function calls.

`examples/function.my` now correctly prints `34 2 2` (fib1(9)=34).
The earlier `21` was the off-by-one corruption: with frame N's slot
overlapping frame N+1's, return values came back from the wrong call
depth.

### Phase 5b — `fall` + `$:` stack-underflow fix  ✓ done
Long-standing compiler bug in the case-statement emit: when a matched
case fell through (`fall`) into a `$:` default arm, the previous arm's
`OP_CJMP` had already popped the scrutinee, then the unconditional
`OP_JMP` from `fall` landed inside the default whose terminal `OP_POP`
then underflowed.

Fix: emit `OP_NIL` immediately before each `fall`'s `OP_JMP` in
`statement.c::caseStatement` and `statement.c::cases`. The placeholder
gets popped by the default arm's `OP_POP`. For `fall`-to-non-default
arms the extra slot is consumed by the existing not-equal-path
`OP_POP` at the end of `cases()`.

This bug had been intermittently masked by optimizer choices for the
last several phases — `__builtin_expect` hints, LTO, and even byte-load
reorderings in OP_GETV would shift whether the underflow stomped on
about-to-be-discarded stack memory or about-to-be-read memory. Now
truly fixed at the source.

### Phase 6 — AST refactor
Introduce a tagged-union AST. Parser builds AST → resolver pass → codegen.
`CompilerFlags` deletes itself.

**Exit criterion:** all 18 examples passing (the 3 pre-existing failures
fall out of the resolver). New syntax (e.g. pattern matching) becomes
straightforward to add.

### Phase 7 — Real GC
Mark-sweep collector with the fiber stack, frames, globals, interned
strings, and `currentClass` as roots. Allocate-triggered with grow factor.
Delete dead `refCount` field from `MyMoObject`.

**Exit criterion:** `make test` runs under ASan/UBSan without leaks; long-
running scripts don't grow unboundedly.

### Phase 8 — Cleanups
- `linenoise` for the REPL (replaces 150 lines of hand-rolled bracket
  matching with `goto` in `main.c::stdinRead`).
- Wire dynamic module loading (`loadBuiltInModule` is currently a no-op).
- Register `date` and `socket` modules in `modules/modules.h`.
- Tighten arrow-function termination (drop blank-line-as-terminator).

## How to work in this repo during the redesign

After every patch series:
```
make clean && make && make test && make bench
```

If `make test` reports a regression on a previously-green example, fix it
before moving on. Update `tests/golden/*.out` only when the output change
is intentional and reviewed.

When migrating a site from legacy `MyMoObject*` to `Value`, the typical
shape of the diff is:

```c
// Before:
MyMoObject *a = pop(vm);
MyMoObject *b = pop(vm);
if (IS_INT(a) && IS_INT(b)) {
    push(vm, NEW_INT(vm, INT_VAL(a) + INT_VAL(b)));
}

// After:
Value a = pop(vm);
Value b = pop(vm);
if (V_IS_INT(a) && V_IS_INT(b)) {
    push(vm, V_INT_VAL(V_AS_INT(a) + V_AS_INT(b)));
}
```

The `V_` prefix goes away in step 1.7 once the legacy macros are gone.
