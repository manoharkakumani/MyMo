// modules/io.c — built-in `io` module: file I/O and filesystem checks.
//
// Exports:
//   io.read(path)            -> string  (reads whole file)
//   io.write(path, content)  -> nil     (overwrite; creates file)
//   io.append(path, content) -> nil
//   io.exists(path)          -> bool
//   io.remove(path)          -> nil

#include "../include/mymo_module.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <io.h>
  #define access_compat(p, m) _access((p), (m))
  #define F_OK_COMPAT 0
#else
  #include <unistd.h>
  #define access_compat(p, m) access((p), (m))
  #define F_OK_COMPAT F_OK
#endif

static MyMoObject *io_read(MVM *vm, uint argc, MyMoObject *argv[])
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
    return s;
}

static MyMoObject *io_write_mode(MVM *vm, const char *funcname,
                                 uint argc, MyMoObject *argv[],
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

static MyMoObject *io_write(MVM *vm, uint argc, MyMoObject *argv[])
{
    return io_write_mode(vm, "io.write", argc, argv, "wb");
}

static MyMoObject *io_append(MVM *vm, uint argc, MyMoObject *argv[])
{
    return io_write_mode(vm, "io.append", argc, argv, "ab");
}

static MyMoObject *io_exists(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.exists", argc, argv, "s", &path)) return MYMO_ERROR;
    return access_compat(path, F_OK_COMPAT) == 0 ? MYMO_TRUE : MYMO_FALSE;
}

static MyMoObject *io_remove(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *path;
    if (!mymo_parse(vm, "io.remove", argc, argv, "s", &path)) return MYMO_ERROR;
    if (remove(path) != 0) {
        runtimeError(vm, "io.remove(): could not remove '%s'", path);
        return MYMO_ERROR;
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
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "io", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
