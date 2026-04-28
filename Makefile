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
# NOCACHE: disable .myc bytecode-cache writes. The cache format predates the
# Phase-1 redesign and cannot represent inline NaN-boxed Values; re-enable
# after step 1.7 once the cache format is rev'd to tag inline values.
COMMON      := $(CSTD) $(WARN) -fno-strict-aliasing -DNOCACHE
RELEASE     := -O3 -DNDEBUG
DEBUG       := -O0 -g3 -DDEBUG_PRINT_CODE -DDEBUG_STACK_TRACE
LDFLAGS     := -lm -lcurl -lsqlite3
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  # -ldl for dlopen, -rdynamic to expose the mymo binary's symbols
  # (newInt, defineBuiltInModule, runtimeError, ...) to extension
  # modules loaded via dlopen. Without -rdynamic the .so resolves OK at
  # build time but the dynamic loader can't bind the references.
  LDFLAGS += -ldl -rdynamic
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
               modules/modules.c
SRC         := $(SRC_ROOT) $(SRC_DT) $(SRC_MOD)

OBJ_REL     := $(SRC:.c=.o)
OBJ_DBG     := $(SRC:.c=.do)

BIN         := mymo
BIN_DEBUG   := mymo-debug

.PHONY: all debug test bench clean ext-hello ext-strings ext-mymath ext-all

EXT_DYLIB := $(if $(filter Darwin,$(UNAME_S)),dylib,$(if $(filter Linux,$(UNAME_S)),so,dll))

all: $(BIN)

$(BIN): CFLAGS := $(COMMON) $(RELEASE) $(CFLAGS_EXTRA)
$(BIN): $(OBJ_REL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

debug: $(BIN_DEBUG)

$(BIN_DEBUG): CFLAGS := $(COMMON) $(DEBUG) $(CFLAGS_EXTRA)
$(BIN_DEBUG): $(OBJ_DBG)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(COMMON) $(RELEASE) $(CFLAGS_EXTRA) -c -o $@ $<

%.do: %.c
	$(CC) $(COMMON) $(DEBUG) $(CFLAGS_EXTRA) -c -o $@ $<

test: $(BIN)
	@./scripts/run-examples.sh

bench: $(BIN)
	@./scripts/bench.sh

clean:
	rm -f $(BIN) $(BIN_DEBUG) $(OBJ_REL) $(OBJ_DBG) \
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
