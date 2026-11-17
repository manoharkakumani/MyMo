// modules/io.c — built-in `io` module: file I/O and filesystem checks.
//
// Exports:
//   io.read(path)            -> string  (reads whole file)
//   io.write(path, content)  -> nil     (overwrite; creates file)
//   io.append(path, content) -> nil
//   io.exists(path)          -> bool
//   io.remove(path)          -> nil
//   io.mtime(path)           -> modification time (seconds), or Nil
//   io.isdir(path)           -> bool
//   io.listdir(path)         -> list of entry names (no . and ..)
//   io.mkdir(path)           -> nil (creates missing parents too)

#include "../include/mymo_module.h"
#include "../datatypes/list.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
  #include <dirent.h>
#endif

#ifdef _WIN32
  #include <io.h>
  #define access_compat(p, m) _access((p), (m))
  #define F_OK_COMPAT 0
#else
  #include <unistd.h>
  #define access_compat(p, m) access((p), (m))
  #define F_OK_COMPAT F_OK
#endif

static Value io_read(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.read", argc, argv, "s", &path)) return MYMO_ERROR;

    FILE *f = fopen(path, "rb");
    if (!f) {
        runtimeError(vm, "io.read(): cannot open '%s'", path);
        return MYMO_ERROR;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        runtimeError(vm, "io.read(): seek failed on '%s'", path);
        return MYMO_ERROR;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        runtimeError(vm, "io.read(): out of memory");
        return MYMO_ERROR;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    MyMoObject *s = mymo_strn(vm, buf, (int)n);
    free(buf);
    return objectToValue(s);
}

static Value io_write_mode(MVM *vm, const char *funcname,
                           uint argc, Value argv[],
                           const char *mode)
{
    const char *path, *content;
    int clen;
    if (!mymo_parse(vm, funcname, argc, argv, "ssn",
                    &path, &content, &clen))
        return MYMO_ERROR;

    FILE *f = fopen(path, mode);
    if (!f) {
        runtimeError(vm, "%s(): cannot open '%s'", funcname, path);
        return MYMO_ERROR;
    }
    size_t n = fwrite(content, 1, (size_t)clen, f);
    fclose(f);
    if (n != (size_t)clen) {
        runtimeError(vm, "%s(): short write to '%s'", funcname, path);
        return MYMO_ERROR;
    }
    return MYMO_NIL;
}

static Value io_write(MVM *vm, uint argc, Value argv[])
{
    return io_write_mode(vm, "io.write", argc, argv, "wb");
}

static Value io_append(MVM *vm, uint argc, Value argv[])
{
    return io_write_mode(vm, "io.append", argc, argv, "ab");
}

static Value io_exists(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.exists", argc, argv, "s", &path)) return MYMO_ERROR;
    return MYMO_BOOL(access_compat(path, F_OK_COMPAT) == 0);
}

static Value io_remove(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.remove", argc, argv, "s", &path)) return MYMO_ERROR;
    if (remove(path) != 0) {
        runtimeError(vm, "io.remove(): could not remove '%s'", path);
        return MYMO_ERROR;
    }
    return MYMO_NIL;
}

static Value io_mtime(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.mtime", argc, argv, "s", &path)) return MYMO_ERROR;
    struct stat st;
    if (stat(path, &st) != 0) return MYMO_NIL;
#if defined(__APPLE__)
    double t = (double)st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
#elif defined(_WIN32)
    double t = (double)st.st_mtime;
#else
    double t = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
#endif
    return V_DOUBLE_VAL(t);
}

static Value io_isdir(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.isdir", argc, argv, "s", &path)) return MYMO_ERROR;
    struct stat st;
    return MYMO_BOOL(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static Value io_listdir(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.listdir", argc, argv, "s", &path)) return MYMO_ERROR;
    MyMoList *out = newList(vm);
#ifdef _WIN32
    runtimeError(vm, "OSError: io.listdir(): not supported on Windows yet");
    return MYMO_ERROR;
#else
    DIR *d = opendir(path);
    if (!d) {
        runtimeError(vm, "OSError: io.listdir(): can't open '%s': %s", path, strerror(errno));
        return MYMO_ERROR;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        writeValueArray(vm, &out->values, objectToValue(mymo_str(vm, e->d_name)));
    }
    closedir(d);
    return objectToValue(AS_OBJECT(out));
#endif
}

static Value io_mkdir(MVM *vm, uint argc, Value argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.mkdir", argc, argv, "s", &path)) return MYMO_ERROR;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; ; p++) {
        char c = *p;
        if (c == '/' || c == '\0') {
            *p = '\0';
#ifdef _WIN32
            int rc = _mkdir(buf);
#else
            int rc = mkdir(buf, 0755);
#endif
            if (rc != 0 && errno != EEXIST) {
                runtimeError(vm, "OSError: io.mkdir(): can't create '%s': %s", buf, strerror(errno));
                return MYMO_ERROR;
            }
            *p = c;
            if (c == '\0') break;
        }
    }
    return MYMO_NIL;
}

MyMoObject *ioModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"read",   io_read},
        {"write",  io_write},
        {"append", io_append},
        {"exists", io_exists},
        {"remove", io_remove},
        {"mtime",  io_mtime},
        {"isdir",  io_isdir},
        {"listdir", io_listdir},
        {"mkdir",  io_mkdir},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "io", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
