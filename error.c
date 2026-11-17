#include "error.h"

// Extract the Nth line (1-indexed) of `source` into `out` (max `out_cap` bytes
// including the terminator). Returns the number of bytes written, or 0 if
// the line doesn't exist or the source is unavailable. Mirrors the
// extractSourceLine helper in vm.c — kept local to keep error.c self-
// contained (the two translation units have different include surfaces).
static int extractLine(const char *source, int lineNo, char *out, size_t out_cap)
{
    if (!source || lineNo < 1 || out_cap == 0) return 0;
    const char *p = source;
    int cur = 1;
    while (*p && cur < lineNo)
    {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != lineNo) return 0;
    size_t n = 0;
    while (*p && *p != '\n' && n + 1 < out_cap)
    {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return (int)n;
}

// Echo the offending source line + a caret under the column.
// Matches the runtime traceback format (vm.c::runtimeError) so the
// compile-time and runtime errors look the same to the user.
static void printSourceCaret(Compiler *compiler, Token token)
{
    if (!compiler->parser->lexer) return;
    const char *src = compiler->parser->lexer->src;
    if (!src) return;
    char lineBuf[512];
    int n = extractLine(src, (int)token.line, lineBuf, sizeof(lineBuf));
    if (n <= 0) return;
    int lead = 0;
    while (lead < n && (lineBuf[lead] == ' ' || lineBuf[lead] == '\t'))
        lead++;
    fprintf(stderr, "      %s\n", lineBuf + lead);
    int caretCol = (int)token.col - lead;
    if (caretCol < 1) caretCol = 1;
    fprintf(stderr, "      ");
    for (int k = 1; k < caretCol; k++) fputc(' ', stderr);
    // Underline the whole token, not just the start column — `at 'foo'`
    // is more readable when the caret spans the offending lexeme.
    int width = (int)token.length;
    if (width < 1) width = 1;
    for (int k = 0; k < width; k++) fputc('^', stderr);
    fputc('\n', stderr);
}

void errorAt(Compiler *compiler, Token token, const char *message)
{
    if (compiler->parser->panicMode)
        return;
    compiler->parser->panicMode = true;
    fprintf(stderr, "[%ld : %ld] Syntax Error", token.line, token.col);
    if (token.type == END)
    {
        fprintf(stderr, " at end");
    }
    else if (token.type == ERROR)
    {
        // Nothing.
    }
    else
    {
        fprintf(stderr, " at '%.*s'", (int)token.length, token.token);
    }
    fprintf(stderr, ": %s\n", message);
    printSourceCaret(compiler, token);
    compiler->parser->hadError = true;
}

void errorAtCurrent(Compiler *compiler, const char *message)
{
    errorAt(compiler, compiler->parser->current, message);
}

void error(Compiler *compiler, const char *message)
{
    errorAt(compiler, compiler->parser->previous, message);
}
