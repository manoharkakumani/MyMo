// .myc bytecode cache.
//
// runFile() compiles `foo.my` once and writes `__mycache__/foo.myc` in the
// same directory (created on demand, like Python's __pycache__). Later runs reuse
// the cached bytecode when the header still matches, skipping lexing and
// compiling. The source is still read on every run (it's cheap, and
// runtime errors need it for the "offending line" display); only its hash
// is compared, so a stale cache is detected by content, not by mtime.
//
// File layout (native endianness — the cache is a per-machine artefact):
//
//   magic   "MYMC"
//   u32     MYMO_CACHE_VERSION   bump on any format or opcode change
//   u32     OP_COUNT         opcode count, as a cheap extra guard
//   u64     build id             hash of MYMO_BUILD_ID: bytecode from a
//                                different interpreter build is never reused
//   u64     source hash          FNV-1a of the .my text (0 = unknown)
//   function                     the script's top-level function
//
//   function := u8 type, i32 argc, u8 isargs, str|none name,
//               str argv[argc], chunk
//   chunk    := i32 count, u8 code[count], u32 lines[count],
//               u32 cols[count], i32 nconst, const[nconst]
//   const    := u8 tag, payload (see CacheTag)
//   str      := i32 length, bytes (no terminator)
//
// Every read is checked; any mismatch or truncation makes cacheRead return
// NULL and the caller recompiles. Writes go to a temp file that is
// renamed into place, so a crash or a concurrent run never leaves a
// half-written cache behind.

#include "cache.h"
#include "datatypes/datatypes.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
  #include <direct.h>
  #define make_dir(p) _mkdir(p)
#else
  #define make_dir(p) mkdir((p), 0755)
#endif

#define CACHE_DIR "__mycache__"

char *cachePathFor(const char *sourcePath)
{
    const char *slash = strrchr(sourcePath, '/');
#ifdef _WIN32
    const char *bslash = strrchr(sourcePath, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    size_t dirLen = slash ? (size_t)(slash - sourcePath + 1) : 0;
    const char *base = sourcePath + dirLen;
    size_t n = dirLen + strlen(CACHE_DIR) + 1 + strlen(base) + 2;
    char *out = malloc(n);
    snprintf(out, n, "%.*s" CACHE_DIR "/%sc", (int)dirLen, sourcePath, base);
    return out;
}

// Create the __mycache__ directory that will hold `cachePath`.
static bool ensureCacheDir(const char *cachePath)
{
    const char *slash = strrchr(cachePath, '/');
    if (!slash)
        return true;
    size_t len = (size_t)(slash - cachePath);
    char *dir = malloc(len + 1);
    memcpy(dir, cachePath, len);
    dir[len] = '\0';
    bool ok = make_dir(dir) == 0 || errno == EEXIST;
    free(dir);
    return ok;
}

#define MYMO_CACHE_VERSION 5 // 3: OP_WIDE; 4: OP_DEFAULTS; 5: OP_METV

// Stamped by the Makefile, which rebuilds cache.o whenever any other object
// changes; so any compiler/VM change invalidates existing caches.
#ifndef MYMO_BUILD_ID
#define MYMO_BUILD_ID __DATE__ " " __TIME__
#endif

static uint64_t buildId(void)
{
    const char *id = MYMO_BUILD_ID;
    return cacheHash(id, strlen(id));
}
static const char CACHE_MAGIC[4] = {'M', 'Y', 'M', 'C'};

typedef enum
{
    TAG_NIL,
    TAG_FALSE,
    TAG_TRUE,
    TAG_INT,       // inline 32-bit int
    TAG_DOUBLE,    // inline double (raw 8 bytes)
    TAG_BIG_INT,   // heap MyMoInt (64-bit long)
    TAG_STRING,
    TAG_FUNCTION,
} CacheTag;

uint64_t cacheHash(const char *src, size_t len)
{
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (size_t i = 0; i < len; i++)
    {
        h ^= (unsigned char)src[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1; // 0 is reserved for "no source"
}

// ---- writing --------------------------------------------------------------

static bool put(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
static bool putU8(FILE *f, uint8_t v) { return put(f, &v, sizeof v); }
static bool putI32(FILE *f, int32_t v) { return put(f, &v, sizeof v); }

static bool putStr(FILE *f, MyMoString *s)
{
    return putI32(f, s->length) && put(f, s->value, (size_t)s->length);
}

static bool putFunction(FILE *f, MyMoFunction *fn);

static bool putConst(FILE *f, Value v)
{
    if (V_IS_NIL(v))   return putU8(f, TAG_NIL);
    if (V_IS_FALSE(v)) return putU8(f, TAG_FALSE);
    if (V_IS_TRUE(v))  return putU8(f, TAG_TRUE);
    if (V_IS_INT(v))   return putU8(f, TAG_INT) && putI32(f, V_AS_INT(v));
    if (V_IS_DOUBLE(v))
    {
        double d = V_AS_DOUBLE(v);
        return putU8(f, TAG_DOUBLE) && put(f, &d, sizeof d);
    }
    MyMoObject *o = V_AS_OBJ(v);
    switch (o->type)
    {
    case OBJ_INT:
    {
        int64_t n = ((MyMoInt *)o)->value;
        return putU8(f, TAG_BIG_INT) && put(f, &n, sizeof n);
    }
    case OBJ_DOUBLE:
    {
        double d = ((MyMoDouble *)o)->value;
        return putU8(f, TAG_DOUBLE) && put(f, &d, sizeof d);
    }
    case OBJ_NIL:  return putU8(f, TAG_NIL);
    case OBJ_BOOL: return putU8(f, AS_BOOL(o)->value ? TAG_TRUE : TAG_FALSE);
    case OBJ_STRING:   return putU8(f, TAG_STRING) && putStr(f, AS_STRING(o));
    case OBJ_FUNCTION: return putU8(f, TAG_FUNCTION) && putFunction(f, AS_FUNCTION(o));
    default:
        return false; // not a compile-time constant we know how to persist
    }
}

static bool putChunk(FILE *f, Chunk *c)
{
    if (!putI32(f, c->count) || !put(f, c->code, (size_t)c->count)
        || !put(f, c->lines, sizeof(u32) * (size_t)c->count)
        || !put(f, c->cols, sizeof(u32) * (size_t)c->count)
        || !putI32(f, c->constants.count))
        return false;
    for (int i = 0; i < c->constants.count; i++)
        if (!putConst(f, c->constants.values[i]))
            return false;
    return true;
}

static bool putFunction(FILE *f, MyMoFunction *fn)
{
    if (!putU8(f, (uint8_t)fn->type) || !putI32(f, fn->argc) || !putU8(f, fn->isargs)
        || !putU8(f, fn->name != NULL) || (fn->name && !putStr(f, fn->name)))
        return false;
    for (int i = 0; i < fn->argc; i++)
        if (!putStr(f, fn->argv[i]))
            return false;
    return putChunk(f, fn->chunk);
}

bool cacheWrite(MyMoFunction *fn, const char *cachePath, uint64_t sourceHash)
{
    if (!ensureCacheDir(cachePath))
        return false; // read-only directory etc.: just run uncached
    size_t n = strlen(cachePath);
    char *tmp = malloc(n + 32);
    snprintf(tmp, n + 32, "%s.tmp%ld", cachePath, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f)
    {
        free(tmp);
        return false; // read-only directory etc.: just run uncached
    }
    uint32_t version = MYMO_CACHE_VERSION, ops = OP_COUNT;
    uint64_t build = buildId();
    bool ok = put(f, CACHE_MAGIC, sizeof CACHE_MAGIC) && put(f, &version, sizeof version)
              && put(f, &ops, sizeof ops) && put(f, &build, sizeof build)
              && put(f, &sourceHash, sizeof sourceHash)
              && putFunction(f, fn);
    ok = (fclose(f) == 0) && ok;
    if (ok)
        ok = rename(tmp, cachePath) == 0;
    if (!ok)
        remove(tmp);
    free(tmp);
    return ok;
}

// ---- reading --------------------------------------------------------------

static bool get(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static bool getI32(FILE *f, int32_t *v) { return get(f, v, sizeof *v); }

// Guard against corrupt lengths before allocating.
#define CACHE_MAX_LEN (1 << 28)

static MyMoString *getStr(MVM *vm, FILE *f)
{
    int32_t len;
    if (!getI32(f, &len) || len < 0 || len > CACHE_MAX_LEN)
        return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf || !get(f, buf, (size_t)len))
    {
        free(buf);
        return NULL;
    }
    MyMoString *s = AS_STRING(NEW_STRING(vm, buf, len));
    free(buf);
    return s;
}

static MyMoFunction *getFunction(MVM *vm, FILE *f);

static bool getConst(MVM *vm, FILE *f, Value *out)
{
    uint8_t tag;
    if (!get(f, &tag, 1))
        return false;
    switch (tag)
    {
    case TAG_NIL:   *out = V_NIL_VAL;   return true;
    case TAG_FALSE: *out = V_FALSE_VAL; return true;
    case TAG_TRUE:  *out = V_TRUE_VAL;  return true;
    case TAG_INT:
    {
        int32_t i;
        if (!getI32(f, &i)) return false;
        *out = V_INT_VAL(i);
        return true;
    }
    case TAG_DOUBLE:
    {
        double d;
        if (!get(f, &d, sizeof d)) return false;
        *out = V_DOUBLE_VAL(d);
        return true;
    }
    case TAG_BIG_INT:
    {
        int64_t n;
        if (!get(f, &n, sizeof n)) return false;
        *out = V_OBJ_VAL(NEW_INT(vm, (long)n));
        return true;
    }
    case TAG_STRING:
    {
        MyMoString *s = getStr(vm, f);
        if (!s) return false;
        *out = V_OBJ_VAL(AS_OBJECT(s));
        return true;
    }
    case TAG_FUNCTION:
    {
        MyMoFunction *fn = getFunction(vm, f);
        if (!fn) return false;
        *out = V_OBJ_VAL(AS_OBJECT(fn));
        return true;
    }
    default:
        return false;
    }
}

static bool getChunk(MVM *vm, FILE *f, Chunk *c)
{
    int32_t count, nconst;
    if (!getI32(f, &count) || count < 0 || count > CACHE_MAX_LEN)
        return false;
    c->capacity = count;
    c->count = count;
    c->code = ResizeArray(vm, u8, NULL, 0, count);
    c->lines = ResizeArray(vm, u32, NULL, 0, count);
    c->cols = ResizeArray(vm, u32, NULL, 0, count);
    if (!get(f, c->code, (size_t)count) || !get(f, c->lines, sizeof(u32) * (size_t)count)
        || !get(f, c->cols, sizeof(u32) * (size_t)count)
        || !getI32(f, &nconst) || nconst < 0 || nconst > CACHE_MAX_LEN)
        return false;
    for (int i = 0; i < nconst; i++)
    {
        Value v;
        if (!getConst(vm, f, &v))
            return false;
        writeValueArray(vm, &c->constants, v);
    }
    return true;
}

static MyMoFunction *getFunction(MVM *vm, FILE *f)
{
    uint8_t type, isargs, hasName;
    int32_t argc;
    if (!get(f, &type, 1) || !getI32(f, &argc) || argc < 0 || argc > 255
        || !get(f, &isargs, 1) || !get(f, &hasName, 1))
        return NULL;
    MyMoFunction *fn = newFunction(vm);
    fn->type = (FunctionType)type;
    fn->argc = argc;
    fn->isargs = isargs;
    if (hasName && !(fn->name = getStr(vm, f)))
        return NULL;
    for (int i = 0; i < argc; i++)
        if (!(fn->argv[i] = getStr(vm, f)))
            return NULL;
    return getChunk(vm, f, fn->chunk) ? fn : NULL;
}

MyMoFunction *cacheRead(MVM *vm, const char *cachePath, uint64_t sourceHash)
{
    FILE *f = fopen(cachePath, "rb");
    if (!f)
        return NULL;
    char magic[4];
    uint32_t version, ops;
    uint64_t build, hash;
    MyMoFunction *fn = NULL;
    if (get(f, magic, sizeof magic) && memcmp(magic, CACHE_MAGIC, sizeof magic) == 0
        && get(f, &version, sizeof version) && version == MYMO_CACHE_VERSION
        && get(f, &ops, sizeof ops) && ops == (uint32_t)(OP_COUNT)
        && get(f, &build, sizeof build) && build == buildId()
        && get(f, &hash, sizeof hash) && (sourceHash == 0 || hash == sourceHash))
        fn = getFunction(vm, f);
    fclose(f);
    return fn;
}
