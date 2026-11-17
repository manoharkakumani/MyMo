#ifndef __CACHE_H__
#define __CACHE_H__

#include "vm.h"
#include <stdint.h>

// .myc bytecode cache — format and invariants documented in cache.c.

// Where the cache for `sourcePath` lives: <dir>/__mycache__/<name>.myc
// (like Python's __pycache__). Caller frees the result.
char *cachePathFor(const char *sourcePath);

// Content hash of a source file (never 0; 0 means "unknown source").
uint64_t cacheHash(const char *src, size_t len);

// Persist `fn` to `cachePath` (atomically). False on any failure; the
// caller just runs uncached.
bool cacheWrite(MyMoFunction *fn, const char *cachePath, uint64_t sourceHash);

// Load a cached function. With sourceHash != 0 the cache must have been
// written for exactly that source; 0 skips the check (running a bare .myc).
// NULL when missing, stale, from another format version, or corrupt.
MyMoFunction *cacheRead(MVM *vm, const char *cachePath, uint64_t sourceHash);

#endif
