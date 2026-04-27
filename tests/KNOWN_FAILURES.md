# Known issues

## Release build: nothing failing

Every example in `examples/` plus `test.my` passes against `master` in the
default release build. `make test` runs all 18 to completion.

## Historical: `fall` + `$:` stack underflow (resolved)

Earlier sessions hit a stack underflow at the end of `examples/case.my`
when a `fall` keyword jumped from a matched non-default arm into a `$:`
(default) arm. Root cause: `OP_CJMP` on the matched arm pops the case
scrutinee, then the unconditional `OP_JMP` from `fall` lands inside the
default arm whose terminal `OP_POP` then has nothing to pop.

Fixed in `statement.c::caseStatement` and `statement.c::cases` by emitting
`OP_NIL` immediately before each `fall`'s `OP_JMP`. The default arm's
`OP_POP` now consumes that placeholder. For `fall`-to-non-default arms the
extra slot is harmless — the trailing not-equal-path `OP_POP` at the end
of `cases()` still pairs up correctly.

## AddressSanitizer: heap-buffer-overflow in `iskeyword` (lexer.c)

Building with `-fsanitize=address` and running any non-trivial example trips
ASan with a 4–8 byte read past the end of the source buffer allocated in
`runFile()` (`utils.c`). The trace points at `iskeyword` (`lexer.c`) doing
`memcmp(token, candidate, k)` where `token` is right at the end of the
mmapped/malloc'd source and the keyword length `k` reads past EOF.

This is a pre-existing bug independent of the perf-redesign work — the
release build does not trip it because the over-read happens to land on
accessible memory. Address it as part of Phase 8 cleanup (alongside the REPL
input-handling overhaul), or earlier if a real crash emerges.

To reproduce:
```bash
make clean
make CFLAGS_EXTRA="-fsanitize=address -fno-omit-frame-pointer"
./mymo examples/variable.my       # ASan report points at iskeyword in lexer.c
```

## Historical: stale `.myc` cache pollution (resolved 2026-04-26)

When `./mymo path/to/foo.my` runs, the interpreter writes a `path/to/foo.myc`
cached bytecode file alongside the source. If those caches outlive the
interpreter version that created them — for example, after a pull or a
refactor that changes the runtime data layout — they silently corrupt
results: the new interpreter happily loads stale bytecode whose constant-pool
entries reference structures that no longer exist.

When the perf-redesign work began, the repo had stale `.myc` files that
caused `class.my`, `pipeT.my`, and `use.my` to appear broken. They aren't —
deleting the `.myc` files made all three pass. `scripts/run-examples.sh` now
removes `*.myc` before every run.

If you start seeing inexplicable runtime errors during a redesign step, the
first thing to try is `/bin/rm -f examples/*.myc benchmarks/*.myc test.myc`.
Long-term we should either version-stamp the cache header or stop writing it
by default.
