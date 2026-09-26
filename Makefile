# MyMo build
#
# Targets:
#   make            - release build (-O3) -> ./mymo
#   make debug      - debug build with VM tracing -> ./mymo-debug
#   make test       - run every examples/*.my and test.my as a smoke test
#   make bench      - time the test.my workload
#   make clean      - remove binaries and object files
#
# Build flags can be overridden, e.g.:
#   make CC=gcc-14
#   make CFLAGS_EXTRA=-fsanitize=address

CC          ?= cc
CSTD        ?= -std=c11
WARN        := -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
COMMON      := $(CSTD) $(WARN) -fno-strict-aliasing
RELEASE     := -O3 -DNDEBUG
DEBUG       := -O0 -g3 -DDEBUG_PRINT_CODE -DDEBUG_STACK_TRACE
LDFLAGS     := -lm -lcurl -lsqlite3
UNAME_S     := $(shell uname -s)
# Line-editing / history / tab-completion for the REPL. macOS ships
# libedit (BSD-licensed, readline-compatible). Linux distros usually
# have GNU readline. Both expose the same `readline()` / `add_history()`
# API, so main.c is platform-agnostic. If you don't have either, omit
# -DHAVE_READLINE and the REPL falls back to fgets.
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -ledit
  COMMON  += -DHAVE_READLINE
else
  ifeq ($(shell pkg-config --exists readline && echo yes),yes)
    LDFLAGS += $(shell pkg-config --libs readline)
    COMMON  += -DHAVE_READLINE $(shell pkg-config --cflags readline)
  endif
endif
ifeq ($(UNAME_S),Linux)
  # -ldl for dlopen, -rdynamic to expose the mymo binary's symbols
  # (newInt, defineBuiltInModule, runtimeError, ...) to extension
  # modules loaded via dlopen. Without -rdynamic the .so resolves OK at
  # build time but the dynamic loader can't bind the references.
  LDFLAGS += -ldl -rdynamic
  # -std=c11 makes glibc/musl hide POSIX APIs (clock_gettime, setenv,
  # strdup, realpath, poll, ...) unless a feature macro is set.
  COMMON  += -D_DEFAULT_SOURCE
endif

# Enumerate sources, skipping test.c (scratch) and unregistered module sources
# (date.c, socket.c) which are not yet wired through modules/modules.h.
# mymo_api.c is the C-API helper layer used by extension modules; it lives
# in the main binary so external .dylibs/.so files resolve at dlopen time.
SRC_ROOT    := $(filter-out test.c, $(wildcard *.c))
SRC_DT      := $(wildcard datatypes/*.c)
SRC_MOD     := modules/math.c modules/time.c modules/os.c modules/io.c \
               modules/random.c modules/date.c modules/socket.c modules/http.c \
               modules/json.c modules/sqlite.c modules/server.c modules/nodes.c \
               modules/runloop.c \
               modules/modules.c
SRC         := $(SRC_ROOT) $(SRC_DT) $(SRC_MOD)

OBJ_REL     := $(SRC:.c=.o)
OBJ_DBG     := $(SRC:.c=.do)

BIN         := mymo
BIN_DEBUG   := mymo-debug

.DEFAULT_GOAL := all

.PHONY: all debug test bench clean lib embed-example ext-hello ext-strings ext-mymath ext-all stdlib

# Embedded MyMo-source stdlib modules. Every .my file in stdlib/
# becomes importable via `from "name" use ...` from any script.
# scripts/gen_stdlib.sh slurps stdlib/*.my into a single header
# (modules/stdlib_src.h) at build time — drop a new file in stdlib/
# and rebuild; no other edits required.
STDLIB_SRCS := $(wildcard stdlib/*.my)

modules/stdlib_src.h: $(STDLIB_SRCS) scripts/gen_stdlib.sh
	@scripts/gen_stdlib.sh $@ stdlib

stdlib: modules/stdlib_src.h

# Bytecode-cache build stamp (see cache.c): cache.o is rebuilt whenever any
# other object changes, picking up a fresh MYMO_BUILD_ID, so .myc files
# written by an older interpreter build are ignored instead of reused.
BUILD_ID     = $(shell date +%s)
cache.o:  $(filter-out cache.o,$(OBJ_REL))
cache.do: $(filter-out cache.do,$(OBJ_DBG))
cache.o cache.do: CFLAGS_EXTRA += -DMYMO_BUILD_ID='"$(BUILD_ID)"'

# utils.c is the only consumer of the embedded sources, so depend
# on the generated header there (both release + debug objects).
utils.o: modules/stdlib_src.h
utils.do: modules/stdlib_src.h

EXT_DYLIB := $(if $(filter Darwin,$(UNAME_S)),dylib,$(if $(filter Linux,$(UNAME_S)),so,dll))

all: $(BIN)

$(BIN): CFLAGS := $(COMMON) $(RELEASE) $(CFLAGS_EXTRA)
$(BIN): $(OBJ_REL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

debug: $(BIN_DEBUG)

$(BIN_DEBUG): CFLAGS := $(COMMON) $(DEBUG) $(CFLAGS_EXTRA)
$(BIN_DEBUG): $(OBJ_DBG)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# -MMD writes a <obj>.d per object listing the headers it includes, so
# editing a header rebuilds its dependents (without it, stale objects
# silently mix old and new struct/typedef layouts).
DEPFLAGS     = -MMD -MP -MF $@.d

%.o: %.c
	$(CC) $(COMMON) $(RELEASE) $(CFLAGS_EXTRA) $(DEPFLAGS) -c -o $@ $<

%.do: %.c
	$(CC) $(COMMON) $(DEBUG) $(CFLAGS_EXTRA) $(DEPFLAGS) -c -o $@ $<

-include $(addsuffix .d,$(OBJ_REL) $(OBJ_DBG))

test: $(BIN)
	@./scripts/run-examples.sh

bench: $(BIN)
	@./scripts/bench.sh

clean:
	rm -f $(BIN) $(BIN_DEBUG) $(OBJ_REL) $(OBJ_DBG) $(LIB) examples/embed/host \
	      $(addsuffix .d,$(OBJ_REL) $(OBJ_DBG)) \
	      examples/ext/hellomod.* examples/ext/stringsmod.* examples/ext/mymathmod.*

# Build the sample external module — demonstrates the C-API path.
# Output is examples/ext/hellomod.{dylib,so,dll}.
ext-hello: examples/ext/hellomod.$(EXT_DYLIB)

# External-module link flags: the .dylib/.so references symbols
# (`newInt`, `defineBuiltInModule`, `runtimeError` …) that live in the
# main mymo binary, not in libc. On macOS we tell the linker to defer
# resolution to runtime via `-undefined dynamic_lookup`; the dyld loader
# resolves against the executable that dlopen()s us. Linux ld permits
# undefined references in shared libraries by default.
ifeq ($(UNAME_S),Darwin)
  EXT_LDFLAGS := -undefined dynamic_lookup
else
  EXT_LDFLAGS :=
endif

examples/ext/hellomod.$(EXT_DYLIB): examples/ext/hello.c
	$(CC) $(COMMON) $(RELEASE) -shared -fPIC -I. -Iinclude -o $@ $< $(EXT_LDFLAGS)

# Larger sample — demonstrates multi-arg parsing, returning bool, alias
# names. See examples/ext/TUTORIAL.md.
ext-strings: examples/ext/stringsmod.$(EXT_DYLIB)

examples/ext/stringsmod.$(EXT_DYLIB): examples/ext/strings.c
	$(CC) $(COMMON) $(RELEASE) -shared -fPIC -I. -Iinclude -o $@ $< $(EXT_LDFLAGS)

# Sample with module-level constants (PI, E, TAU, VERSION).
ext-mymath: examples/ext/mymathmod.$(EXT_DYLIB)

examples/ext/mymathmod.$(EXT_DYLIB): examples/ext/mymath.c
	$(CC) $(COMMON) $(RELEASE) -shared -fPIC -I. -Iinclude -o $@ $< $(EXT_LDFLAGS)

ext-all: ext-hello ext-strings ext-mymath

# Embedding: libmymo.a is the interpreter without main.o, for C programs
# that host MyMo through include/mymo.h. The example host links it.
LIB         := libmymo.a
LIB_LDFLAGS := -lm -lcurl -lsqlite3 $(if $(filter Linux,$(UNAME_S)),-ldl,)

lib: $(LIB)

$(LIB): $(filter-out main.o,$(OBJ_REL))
	ar rcs $@ $^

embed-example: examples/embed/host

examples/embed/host: examples/embed/host.c $(LIB)
	$(CC) $(COMMON) $(RELEASE) -Iinclude -I. -o $@ $< $(LIB) $(LIB_LDFLAGS)
