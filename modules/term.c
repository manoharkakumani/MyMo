// modules/term.c — built-in `term` module: raw keyboard input and screen
// size for full-screen terminal apps (the `ui` library's terminal target).
//
// Exports:
//   term.isatty()          -> True when stdin and stdout are a terminal
//   term.size()            -> (columns, rows)
//   term.raw(on)           -> enter (True) / leave (False) raw mode: no echo,
//                             no line buffering; restored at exit anyway
//   term.key(timeout_ms?)  -> the next key, or Nil after timeout_ms
//                             (default: wait forever). Printable keys are
//                             themselves ("a", "é"); others are names:
//                             "enter" "tab" "shift-tab" "backspace" "esc"
//                             "up" "down" "left" "right" "home" "end"
//                             "delete" "pageup" "pagedown" "ctrl-c" "ctrl-<x>"
//   term.write(s)          -> write s to stdout unbuffered-ish (flushes)

#include "../include/mymo_module.h"
#include "../datatypes/tuple.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <conio.h>
  #include <io.h>
  #include <windows.h>
  #define isatty _isatty
  #define STDIN_FILENO 0
  #define STDOUT_FILENO 1
#else
  #include <sys/ioctl.h>
  #include <sys/select.h>
  #include <termios.h>
  #include <unistd.h>
#endif

#ifndef _WIN32
static struct termios g_saved;
static bool g_raw = false;

static void restore_terminal(void)
{
    if (g_raw)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = false;
    }
}

// Wait up to ms for stdin to be readable (ms < 0: forever).
static bool wait_input(long ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, ms < 0 ? NULL : &tv) > 0;
}

static int read_byte(long ms)
{
    if (!wait_input(ms))
        return -1;
    unsigned char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}
#endif

static Value term_isatty(MVM *vm, uint argc, Value argv[])
{
    if (!mymo_check_args(vm, "term.isatty", argc, 0)) return MYMO_ERROR;
    return MYMO_BOOL(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
}

static Value term_size(MVM *vm, uint argc, Value argv[])
{
    if (!mymo_check_args(vm, "term.size", argc, 0)) return MYMO_ERROR;
    int cols = 80, rows = 24;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
    {
        cols = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
    MyMoTuple *t = newTuple(vm);
    writeValueArray(vm, &t->values, V_INT_VAL(cols));
    writeValueArray(vm, &t->values, V_INT_VAL(rows));
    return objectToValue(AS_OBJECT(t));
}

static Value term_raw(MVM *vm, uint argc, Value argv[])
{
    int on;
    if (!mymo_parse(vm, "term.raw", argc, argv, "b", &on)) return MYMO_ERROR;
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(out, &mode))
        SetConsoleMode(out, mode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    if (GetConsoleMode(in, &mode))
        SetConsoleMode(in, on ? (mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT)) : (mode | ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
#else
    if (on && !g_raw)
    {
        if (tcgetattr(STDIN_FILENO, &g_saved) != 0)
            return MYMO_BOOL(false);
        static bool registered = false;
        if (!registered)
        {
            atexit(restore_terminal);
            registered = true;
        }
        struct termios raw = g_saved;
        raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(unsigned)(OPOST);
        raw.c_cflag |= CS8;
        raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        g_raw = true;
    }
    else if (!on)
        restore_terminal();
#endif
    return MYMO_BOOL(true);
}

static Value key_name(MVM *vm, const char *name) { return objectToValue(mymo_str(vm, name)); }

static Value term_key(MVM *vm, uint argc, Value argv[])
{
    long ms = -1;
    if (argc > 1)
    {
        runtimeError(vm, "TypeError: term.key() takes at most 1 argument (%u given)", argc);
        return MYMO_ERROR;
    }
    if (argc == 1 && !mymo_parse(vm, "term.key", argc, argv, "i", &ms)) return MYMO_ERROR;
    fflush(stdout);
#ifdef _WIN32
    if (ms >= 0)
    {
        DWORD start = GetTickCount();
        while (!_kbhit())
        {
            if ((long)(GetTickCount() - start) >= ms)
                return MYMO_NIL;
            Sleep(10);
        }
    }
    int c = _getwch();
    if (c == 0 || c == 0xE0)
    {
        switch (_getwch())
        {
        case 72: return key_name(vm, "up");
        case 80: return key_name(vm, "down");
        case 75: return key_name(vm, "left");
        case 77: return key_name(vm, "right");
        case 71: return key_name(vm, "home");
        case 79: return key_name(vm, "end");
        case 83: return key_name(vm, "delete");
        case 73: return key_name(vm, "pageup");
        case 81: return key_name(vm, "pagedown");
        default: return key_name(vm, "unknown");
        }
    }
    if (c == '\r') return key_name(vm, "enter");
    if (c == '\t') return key_name(vm, "tab");
    if (c == 8) return key_name(vm, "backspace");
    if (c == 27) return key_name(vm, "esc");
    if (c == 3) return key_name(vm, "ctrl-c");
    char buf[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, (wchar_t *)&c, 1, buf, sizeof buf, NULL, NULL);
    return objectToValue(mymo_strn(vm, buf, n));
#else
    int c = read_byte(ms);
    if (c < 0)
        return MYMO_NIL;
    switch (c)
    {
    case '\r':
    case '\n': return key_name(vm, "enter");
    case '\t': return key_name(vm, "tab");
    case 127:
    case 8: return key_name(vm, "backspace");
    case 3: return key_name(vm, "ctrl-c");
    case 27:
    {
        // An escape sequence arrives all at once; a lone ESC doesn't.
        int c1 = read_byte(30);
        if (c1 < 0)
            return key_name(vm, "esc");
        if (c1 != '[' && c1 != 'O')
            return key_name(vm, "esc");
        int c2 = read_byte(30);
        switch (c2)
        {
        case 'A': return key_name(vm, "up");
        case 'B': return key_name(vm, "down");
        case 'C': return key_name(vm, "right");
        case 'D': return key_name(vm, "left");
        case 'H': return key_name(vm, "home");
        case 'F': return key_name(vm, "end");
        case 'Z': return key_name(vm, "shift-tab");
        default: break;
        }
        if (c2 >= '0' && c2 <= '9')
        {
            int c3 = read_byte(30);
            while (c3 >= 0 && c3 != '~' && !(c3 >= 'A' && c3 <= 'Z'))
                c3 = read_byte(30); // skip modifiers such as "1;5"
            switch (c2)
            {
            case '1': case '7': return key_name(vm, "home");
            case '4': case '8': return key_name(vm, "end");
            case '3': return key_name(vm, "delete");
            case '5': return key_name(vm, "pageup");
            case '6': return key_name(vm, "pagedown");
            default: break;
            }
        }
        return key_name(vm, "unknown");
    }
    default:
        break;
    }
    if (c < 32)
    {
        char name[8];
        snprintf(name, sizeof name, "ctrl-%c", c + 'a' - 1);
        return key_name(vm, name);
    }
    // A UTF-8 character: read its continuation bytes.
    char buf[4] = {(char)c};
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    for (int i = 1; i < n; i++)
    {
        int b = read_byte(30);
        if (b < 0)
        {
            n = i;
            break;
        }
        buf[i] = (char)b;
    }
    return objectToValue(mymo_strn(vm, buf, n));
#endif
}

static Value term_write(MVM *vm, uint argc, Value argv[])
{
    const char *s;
    int len;
    if (!mymo_parse(vm, "term.write", argc, argv, "sn", &s, &len)) return MYMO_ERROR;
    fwrite(s, 1, (size_t)len, stdout);
    fflush(stdout);
    return MYMO_NIL;
}

MyMoObject *termModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"isatty", term_isatty},
        {"size",   term_size},
        {"raw",    term_raw},
        {"key",    term_key},
        {"write",  term_write},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "term", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
