// modules/os.c — built-in `os` module.
//
// Exports:
//   os.getenv(name)         -> string or nil
//   os.setenv(name, value)  -> nil
//   os.unsetenv(name)       -> nil
//   os.getcwd()             -> string
//   os.exit(code)           -> never returns
//   os.system(cmd)          -> exit status of `cmd` run by the shell
//   os.popen(cmd)           -> what `cmd` printed to stdout (string)
//   os.args                 [script, arg1, ...] from the command line
//   os.executable           path of the running mymo binary
//   os.exec(args)           replace this process with `executable args...`
//   os.platform             constant: "darwin" | "linux" | "windows" | "unknown"
//   os.sep                  constant: "/" or "\\"

#include "../include/mymo_module.h"
#include "../datatypes/list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
  #define getcwd_compat(buf, size) _getcwd((buf), (size))
#else
  #include <unistd.h>
  #define getcwd_compat(buf, size) getcwd((buf), (size))
#endif

#ifdef _WIN32
  #define popen _popen
  #define pclose _pclose
#else
  #include <sys/wait.h>
#endif

static int g_argc = 0;
static const char **g_argv = NULL;
static char g_exe[4096];

// Called by main(): the interpreter's own path (for os.exec).
void os_set_executable(const char *path)
{
    snprintf(g_exe, sizeof g_exe, "%s", path);
}

static Value os_exec(MVM *vm, uint argc, Value argv[])
{
    MyMoList *list;
    if (!mymo_parse(vm, "os.exec", argc, argv, "L", &list)) return MYMO_ERROR;
#ifdef _WIN32
    runtimeError(vm, "OSError: os.exec(): not supported on Windows yet");
    return MYMO_ERROR;
#else
    int n = list->values.count;
    char **av = calloc((size_t)n + 2, sizeof(char *));
    av[0] = g_exe;
    for (int i = 0; i < n; i++) {
        Value v = list->values.values[i];
        if (!V_IS_OBJ_TYPE(v, OBJ_STRING)) {
            free(av);
            runtimeError(vm, "TypeError: os.exec(): arguments must be strings");
            return MYMO_ERROR;
        }
        av[i + 1] = AS_STRING(V_AS_OBJ(v))->value;
    }
    fflush(stdout);
    fflush(stderr);
    execvp(g_exe, av); // searches PATH when mymo was started by name
    free(av);
    runtimeError(vm, "OSError: os.exec(): %s", strerror(errno));
    return MYMO_ERROR;
#endif
}

// Called by main() before the VM starts: the script path and its args.
void os_set_args(int argc, const char **argv)
{
    g_argc = argc;
    g_argv = argv;
}

static Value os_system(MVM *vm, uint argc, Value argv[])
{
    const char *cmd;
    if (!mymo_parse(vm, "os.system", argc, argv, "s", &cmd)) return MYMO_ERROR;
    fflush(stdout);
    int status = system(cmd);
#ifndef _WIN32
    if (status != -1 && WIFEXITED(status))
        status = WEXITSTATUS(status);
#endif
    return V_INT_VAL(status);
}

static Value os_popen(MVM *vm, uint argc, Value argv[])
{
    const char *cmd;
    if (!mymo_parse(vm, "os.popen", argc, argv, "s", &cmd)) return MYMO_ERROR;
    fflush(stdout);
    FILE *p = popen(cmd, "r");
    if (!p)
    {
        runtimeError(vm, "OSError: os.popen(): can't run command");
        return MYMO_ERROR;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, p)) > 0)
    {
        len += n;
        if (len == cap)
            buf = realloc(buf, cap *= 2);
    }
    pclose(p);
    MyMoObject *out = mymo_strn(vm, buf, (int)len);
    free(buf);
    return objectToValue(out);
}

static Value os_getenv(MVM *vm, uint argc, Value argv[])
{
    const char *name;
    if (!mymo_parse(vm, "os.getenv", argc, argv, "s", &name)) return MYMO_ERROR;
    const char *v = getenv(name);
    return v ? objectToValue(mymo_str(vm, v)) : MYMO_NIL;
}

static Value os_setenv(MVM *vm, uint argc, Value argv[])
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

static Value os_unsetenv(MVM *vm, uint argc, Value argv[])
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

static Value os_getcwd(MVM *vm, uint argc, Value argv[])
{
    if (!mymo_check_args(vm, "os.getcwd", argc, 0)) return MYMO_ERROR;
    char buf[4096];
    if (!getcwd_compat(buf, sizeof(buf))) {
        runtimeError(vm, "os.getcwd(): failed");
        return MYMO_ERROR;
    }
    return objectToValue(mymo_str(vm, buf));
}

static Value os_exit(MVM *vm, uint argc, Value argv[])
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
        {"system",   os_system},
        {"popen",    os_popen},
        {"exec",     os_exec},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "os", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    MyMoObject *mod = defineBuiltInModule(vm, &def);
    MyMoList *args = newList(vm);
    for (int i = 0; i < g_argc; i++)
        writeValueArray(vm, &args->values, objectToValue(mymo_str(vm, g_argv[i])));
    mymo_set(vm, mod, "args", AS_OBJECT(args));
    mymo_set_str(vm, mod, "executable", g_exe);

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
