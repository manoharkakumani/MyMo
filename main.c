#include "common.h"
#include "compiler.h"
#include "vm.h"
#include "utils.h"
#include "datatypes/dict.h"

#ifdef HAVE_READLINE
  // libedit on macOS, GNU readline on Linux — same API surface.
  #ifdef __APPLE__
    #include <editline/readline.h>
  #else
    #include <readline/readline.h>
    #include <readline/history.h>
  #endif
#endif

// ─── REPL input ──────────────────────────────────────────────────────
//
// Scan a single buffer line by line, accumulating into `dst`. Track:
//   - bracket depth (`(`, `[`, `{`) — keep reading until balanced
//   - a `:` or `=>` line — expects an indented block; keep reading
//     until a blank line OR a dedent back to the original column
//   - open string literals — keep reading until the quote closes
//
// Returns the full source on submit (empty buffer → NULL on EOF /
// just-press-enter at top level).

typedef struct {
    int paren;   // ( )
    int square;  // [ ]
    int brace;   // { }
    int inBlock; // we're inside an indented block (after `:` or `=>`)
    int blankAfterBlock; // count of trailing blank lines
} ReplState;

// Update the bracket/block state by scanning one line of input.
// Returns 1 if the line should trigger continuation, 0 if we're done.
static void scanLine(const char *line, ReplState *s, int *opensBlock)
{
    *opensBlock = 0;
    int quote = 0;
    int len = (int)strlen(line);
    int nonblank = 0;
    for (int i = 0; i < len; i++)
    {
        char c = line[i];
        if (quote)
        {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') { quote = c; nonblank = 1; continue; }
        if (c == '#')
        {
            // Single `#` and `##...##` block comments — for REPL
            // multiline tracking we treat single-line `#` as
            // "ignore rest of line". Block `##` straddling lines
            // isn't worth handling here; users rarely type one.
            if (i + 1 < len && line[i+1] == '#')
            {
                // Skip until closing ##
                i += 2;
                while (i + 1 < len && !(line[i] == '#' && line[i+1] == '#')) i++;
                if (i + 1 < len) i++;
                continue;
            }
            break;
        }
        if (c == '(') { s->paren++; nonblank = 1; continue; }
        if (c == ')') { if (s->paren > 0) s->paren--; nonblank = 1; continue; }
        if (c == '[') { s->square++; nonblank = 1; continue; }
        if (c == ']') { if (s->square > 0) s->square--; nonblank = 1; continue; }
        if (c == '{') { s->brace++; nonblank = 1; continue; }
        if (c == '}') { if (s->brace > 0) s->brace--; nonblank = 1; continue; }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') nonblank = 1;
    }
    // After stripping trailing whitespace, check the last non-ws
    // char for `:` or `=>` (which begin an indented block).
    int end = len - 1;
    while (end >= 0 && (line[end] == ' ' || line[end] == '\t'
                        || line[end] == '\r' || line[end] == '\n'))
        end--;
    if (end >= 0)
    {
        if (line[end] == ':') *opensBlock = 1;
        else if (end >= 1 && line[end-1] == '=' && line[end] == '>') *opensBlock = 1;
    }
    if (!nonblank)
    {
        if (s->inBlock) s->blankAfterBlock++;
    }
    else
    {
        s->blankAfterBlock = 0;
    }
}

// Read one line either via libedit/readline (history + arrow keys)
// or, if HAVE_READLINE is off, plain fgets. The returned buffer is
// caller-owned and includes the trailing '\n' for consistency with
// fgets. NULL means EOF.
static char *readReplLine(const char *prompt)
{
#ifdef HAVE_READLINE
    char *line = readline(prompt);
    if (line == NULL) return NULL;
    size_t n = strlen(line);
    char *withNl = (char *)malloc(n + 2);
    memcpy(withNl, line, n);
    withNl[n]     = '\n';
    withNl[n + 1] = '\0';
    free(line);
    return withNl;
#else
    printf("%s", prompt);
    fflush(stdout);
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) == NULL) return NULL;
    return strdup(buf);
#endif
}

// Read one logical statement (possibly spanning many physical lines).
// Caller owns the returned buffer; returns NULL on EOF with empty
// buffer.
static char *readReplStatement(const char *promptFirst, const char *promptCont)
{
    char *buf = NULL;
    size_t bufLen = 0;
    size_t bufCap = 0;
    ReplState s = {0};
    int lineNo = 0;

    for (;;)
    {
        char *line = readReplLine(lineNo == 0 ? promptFirst : promptCont);
        if (line == NULL)
        {
            if (bufLen == 0)
            {
                free(buf);
                return NULL; // EOF on empty input
            }
            break; // EOF mid-statement — submit what we have
        }
        size_t lineLen = strlen(line);
        if (bufLen + lineLen + 2 > bufCap)
        {
            bufCap = (bufLen + lineLen + 2) * 2;
            buf = (char *)realloc(buf, bufCap);
        }
        memcpy(buf + bufLen, line, lineLen);
        bufLen += lineLen;
        buf[bufLen] = '\0';
        lineNo++;

        int opensBlock = 0;
        scanLine(line, &s, &opensBlock);
        free(line);
        if (opensBlock) s.inBlock = 1;

        if (lineNo == 1 && s.paren == 0 && s.square == 0
            && s.brace == 0 && !s.inBlock)
            break;
        if (s.paren == 0 && s.square == 0 && s.brace == 0
            && (!s.inBlock || s.blankAfterBlock >= 1))
            break;
    }
    return buf;
}

#ifdef HAVE_READLINE
// Globals that the completion callback needs. Set in repl() before
// readline is asked for input.
static MVM *g_repl_vm = NULL;

static char *replCompletionGenerator(const char *text, int state)
{
    static int idx;       // walk position into globals
    static int builtins;  // whether we've moved on to builtins
    static int len;
    if (g_repl_vm == NULL) return NULL;
    if (state == 0) { idx = 0; builtins = 0; len = (int)strlen(text); }
    MyMoDict *src = builtins ? &g_repl_vm->builtins : &g_repl_vm->globals;
    while (1)
    {
        while (idx <= src->capacity)
        {
            Entry *e = &src->entries[idx++];
            if (e->key == NULL) continue;
            if (e->key->type != OBJ_STRING) continue;
            MyMoString *k = (MyMoString *)e->key;
            if (k->length >= len && memcmp(k->value, text, (size_t)len) == 0)
            {
                // readline expects a malloc'd copy.
                char *out = (char *)malloc((size_t)k->length + 1);
                memcpy(out, k->value, (size_t)k->length);
                out[k->length] = '\0';
                return out;
            }
        }
        if (builtins) return NULL;
        builtins = 1;
        idx = 0;
    }
}

static char **replCompletion(const char *text, int start, int end)
{
    (void)start; (void)end;
    rl_attempted_completion_over = 1; // suppress filename fallback
    return rl_completion_matches(text, replCompletionGenerator);
}

// Resolve ~/.mymo_history (or fall back to ./.mymo_history if HOME
// isn't set). Caller owns the returned string.
static char *replHistoryPath(void)
{
    const char *home = getenv("HOME");
    const char *base = home && *home ? home : ".";
    size_t n = strlen(base);
    char *path = (char *)malloc(n + 16);
    snprintf(path, n + 16, "%s/.mymo_history", base);
    return path;
}
#endif

// True iff the source looks like a bare top-level expression (no
// statement keywords, no assignment, no colon/block, parens
// balanced). When that's the case the REPL wraps it as
// `print(<src>)` so the user sees the value — Python-style.
static int looksLikeBareExpression(const char *src)
{
    int parens = 0, square = 0, brace = 0;
    int quote = 0;
    int sawNonSpace = 0;
    int len = (int)strlen(src);
    static const char *KW[] = {
        "if ", "else", "elif", "while", "for ", "fn ", "class", "case",
        "cond", "return", "break", "continue", "raise", "try", "del",
        "use ", "from ", "yield", "pass", "fall", "catch", "final",
        NULL
    };
    // Trim leading whitespace and reject statement keywords up-front.
    int i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\t')) i++;
    for (int k = 0; KW[k]; k++)
    {
        size_t kl = strlen(KW[k]);
        if ((size_t)(len - i) >= kl && memcmp(src + i, KW[k], kl) == 0)
            return 0;
    }
    // Skip auto-print for expressions whose evaluation is already
    // side-effecting in a way the user sees (a `print(...)` call).
    // Otherwise `print("hi")` would show `hi` and then auto-print
    // print's return value too.
    static const char *SIDE_EFFECT[] = {
        "print(", "respond(", "respond_json(", NULL,
    };
    for (int k = 0; SIDE_EFFECT[k]; k++)
    {
        size_t kl = strlen(SIDE_EFFECT[k]);
        if ((size_t)(len - i) >= kl && memcmp(src + i, SIDE_EFFECT[k], kl) == 0)
            return 0;
    }
    for (; i < len; i++)
    {
        char c = src[i];
        if (quote)
        {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') { quote = c; sawNonSpace = 1; continue; }
        if (c == '#') break; // rest is comment
        if (c == '(') parens++;
        else if (c == ')') parens--;
        else if (c == '[') square++;
        else if (c == ']') square--;
        else if (c == '{') brace++;
        else if (c == '}') brace--;
        else if (c == ':' && parens == 0 && square == 0 && brace == 0)
            return 0; // top-level `:` starts a block; dict-literal `:` is fine
        else if (c == '=' && (i + 1 >= len || src[i+1] != '='))
        {
            // Assignment-like — except `=>` arrow which is fine.
            if (i + 1 < len && src[i+1] == '>') { i++; continue; }
            // Top-level `=` is assignment.
            if (parens == 0 && square == 0 && brace == 0) return 0;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') sawNonSpace = 1;
    }
    return sawNonSpace;
}

static int isWhitespaceOnly(const char *s)
{
    while (*s)
    {
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return 0;
        s++;
    }
    return 1;
}

// Try to match a leading `:command` and act on it. Returns 1 if
// handled (and the REPL should continue), 0 if `src` isn't a
// command and should be sent to the compiler, or -1 to exit.
static int handleReplCommand(const char *src)
{
    while (*src == ' ' || *src == '\t') src++;
    if (*src != ':') return 0;
    src++;
    // Strip trailing newline / spaces for comparison.
    char cmd[64];
    size_t n = 0;
    while (*src && *src != ' ' && *src != '\n' && *src != '\r' && n + 1 < sizeof(cmd))
        cmd[n++] = *src++;
    cmd[n] = '\0';
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0)
        return -1;
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0)
    {
        printf("\n  MyMo REPL commands:\n");
        printf("    :exit / :quit / :q      leave the REPL\n");
        printf("    :help / :h / :?         this help\n");
        printf("    :clear / :cls           clear the screen\n");
        printf("\n  Input rules:\n");
        printf("    - simple statements (`x = 1`, `print(x)`) submit on Enter\n");
        printf("    - unbalanced (/[/{ keeps reading until matched\n");
        printf("    - lines ending in `:` or `=>` start an indented block;\n");
        printf("      submit with a blank line\n");
        printf("    - Ctrl-D at the prompt exits\n\n");
        return 1;
    }
    if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0)
    {
        printf("\033[2J\033[H");
        fflush(stdout);
        return 1;
    }
    fprintf(stderr, "  unknown REPL command :%s — try :help\n", cmd);
    return 1;
}

int repl(MVM *vm)
{
    vm->currentModule = newModule(vm,
        newString(vm, "__repl__", 8),
        newString(vm, "@repl", 5));

#ifdef HAVE_READLINE
    using_history();
    g_repl_vm = vm;
    rl_attempted_completion_function = replCompletion;
    // Allow `_` and `:` in completion tokens so things like `_state`
    // and `:exit` complete naturally. (Default breaks on `_`.)
    rl_basic_word_break_characters = (char *)" \t\n\"\\'`@$><=;|&{(";
    char *histPath = replHistoryPath();
    read_history(histPath);
    printf("MyMo REPL — type :help for commands, :exit to leave.\n");
    printf("Line editing + history enabled (saved to %s).\n", histPath);
#else
    printf("MyMo REPL — type :help for commands, :exit to leave.\n");
#endif

    const char *promptFirst = ">>> ";
    const char *promptCont  = "... ";

    for (;;)
    {
        char *src = readReplStatement(promptFirst, promptCont);
        if (src == NULL)
        {
            printf("\n");
#ifdef HAVE_READLINE
            write_history(histPath);
            free(histPath);
#endif
            return 0;
        }
        if (isWhitespaceOnly(src))
        {
            free(src);
            continue;
        }
        int cmd = handleReplCommand(src);
        if (cmd == -1)
        {
            free(src);
            printf("bye.\n");
#ifdef HAVE_READLINE
            write_history(histPath);
            free(histPath);
#endif
            return 0;
        }
        if (cmd == 1)
        {
            free(src);
            continue;
        }
#ifdef HAVE_READLINE
        // Add to history. Strip trailing newline for nicer display
        // when scrolling back.
        size_t n = strlen(src);
        char *hist = strndup(src, n);
        while (n > 0 && (hist[n-1] == '\n' || hist[n-1] == '\r')) hist[--n] = '\0';
        if (n > 0) add_history(hist);
        free(hist);
#endif
        // Python-style: a bare expression auto-prints. Wrap it in
        // `print(...)` so the compiler emits the side-effecting form.
        char *toCompile = src;
        char *wrapped = NULL;
        if (looksLikeBareExpression(src))
        {
            size_t n2 = strlen(src);
            while (n2 > 0 && (src[n2-1] == '\n' || src[n2-1] == '\r')) n2--;
            wrapped = (char *)malloc(n2 + 16);
            int wlen = snprintf(wrapped, n2 + 16, "print(%.*s)\n", (int)n2, src);
            (void)wlen;
            toCompile = wrapped;
        }
        MyMoFunction *function = compile(vm, toCompile, "@repl", COMPILE_REPL);
        if (function != NULL)
        {
            interpreter(vm, function);
        }
        free(wrapped);
        free(src);
    }
    return 0;
}

extern void nodes_set_argv0(const char *path);

int main(int argc, const char *argv[])
{
    char selfpath[10000];
    #ifdef _WIN32
        if (_fullpath(selfpath, argv[0], MAX_PATH))
            nodes_set_argv0(selfpath);
    #else
        if (realpath(argv[0], selfpath))
            nodes_set_argv0(selfpath);
        else
            nodes_set_argv0(argv[0]);
    #endif
    MVM *vm = initVM();
    if (argc == 1)
    {
        return repl(vm);
    }
    else if (argc == 2)
    {
        char actualpath[10000];
        char *path;
        #ifdef _WIN32
            path =  _fullpath(actualpath, argv[1], MAX_PATH); 
        #else
            path = realpath(argv[1], actualpath);
        #endif
        if (path == NULL)
        {
            fprintf(stderr, "Could not find script \"%s\".\n", argv[1]);
            exit(74);
        }
        char *extension = strrchr(path, '.');
        if (extension == NULL)
        {
            fprintf(stderr, "Invalid MyMo file.\n");
            exit(74);
        }
        else if (strcmp(extension, ".my") == 0)
        {
            char *path = pathResolver(vm, (char *)argv[1]);
            MyMoFunction *function = runFile(vm, path);
            size_t len = strlen(path);
            if (path[len - 1] == 'c')
            {
                path[len - 1] = '\0';
                len--;
            }
            vm->currentModule = newModule(vm, newString(vm, "__main__", 8), newString(vm, path, len));
            free(path);
            #ifdef DEBUG_PRINT_CODE
                if(function)
                    debugChunk(function);
            #endif
            I_Result result = interpreter(vm, function);
            if (result == COMPILE_ERROR)
            {
                freeVM(vm);
                exit(65);
            }
            if (result == RUNTIME_ERROR)
            {
                freeVM(vm);
                exit(70);
            }
        }
        else
        {
            fprintf(stderr, "Invalid MyMo file\n");
            free(vm);
            exit(74);
        }
    }
    else
    {
        fprintf(stderr, "Usage: MyMo [path]\n");
        freeVM(vm);
        exit(64);
    }
    freeVM(vm);
    return 0;
}