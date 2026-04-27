// modules/os.c — built-in `os` module.
//
// Exports:
//   os.getenv(name)         -> string or nil
//   os.setenv(name, value)  -> nil
//   os.unsetenv(name)       -> nil
//   os.getcwd()             -> string
//   os.exit(code)           -> never returns
//   os.platform             constant: "darwin" | "linux" | "windows" | "unknown"
//   os.sep                  constant: "/" or "\\"

#include "../include/mymo_module.h"
#include <stdlib.h>

#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
  #define getcwd_compat(buf, size) _getcwd((buf), (size))
#else
  #include <unistd.h>
  #define getcwd_compat(buf, size) getcwd((buf), (size))
#endif

static MyMoObject *os_getenv(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *name;
    if (!mymo_parse(vm, "os.getenv", argc, argv, "s", &name)) return MYMO_ERROR;
    const char *v = getenv(name);
    return v ? mymo_str(vm, v) : MYMO_NIL;
}

static MyMoObject *os_setenv(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *name, *value;
    if (!mymo_parse(vm, "os.setenv", argc, argv, "ss", &name, &value))
        return MYMO_ERROR;
#ifdef _WIN32
    if (_putenv_s(name, value) != 0) {
        runtimeError(vm, "os.setenv(): failed to set %s", name);
        return MYMO_ERROR;
    }
#else
    if (setenv(name, value, 1) != 0) {
        runtimeError(vm, "os.setenv(): failed to set %s", name);
        return MYMO_ERROR;
    }
#endif
    return MYMO_NIL;
}

static MyMoObject *os_unsetenv(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *name;
    if (!mymo_parse(vm, "os.unsetenv", argc, argv, "s", &name))
        return MYMO_ERROR;
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
    return MYMO_NIL;
}

static MyMoObject *os_getcwd(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "os.getcwd", argc, 0)) return MYMO_ERROR;
    char buf[4096];
    if (!getcwd_compat(buf, sizeof(buf))) {
        runtimeError(vm, "os.getcwd(): failed");
        return MYMO_ERROR;
    }
    return mymo_str(vm, buf);
}

static MyMoObject *os_exit(MVM *vm, uint argc, MyMoObject *argv[])
{
    long code = 0;
    if (argc > 1) {
        runtimeError(vm, "os.exit(): takes 0 or 1 arguments, got %u", argc);
        return MYMO_ERROR;
    }
    if (argc == 1) {
        if (!mymo_parse(vm, "os.exit", argc, argv, "i", &code)) return MYMO_ERROR;
    }
    exit((int)code);
    return MYMO_NIL;  // unreachable
}

MyMoObject *osModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"getenv",   os_getenv},
        {"setenv",   os_setenv},
        {"unsetenv", os_unsetenv},
        {"getcwd",   os_getcwd},
        {"exit",     os_exit},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "os", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    MyMoObject *mod = defineBuiltInModule(vm, &def);

#if defined(__APPLE__)
    mymo_set_str(vm, mod, "platform", "darwin");
    mymo_set_str(vm, mod, "sep",      "/");
#elif defined(__linux__)
    mymo_set_str(vm, mod, "platform", "linux");
    mymo_set_str(vm, mod, "sep",      "/");
#elif defined(_WIN32)
    mymo_set_str(vm, mod, "platform", "windows");
    mymo_set_str(vm, mod, "sep",      "\\");
#else
    mymo_set_str(vm, mod, "platform", "unknown");
    mymo_set_str(vm, mod, "sep",      "/");
#endif
    return mod;
}
