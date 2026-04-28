#include "tokens.h"
#include <string.h>

Token newToken(const char *src, size_t type, size_t len, size_t col, size_t indent, size_t line)
{
    Token tok;
    tok.token = src;
    tok.type = type;
    tok.length = len;
    tok.col = col;
    tok.indent = indent;
    tok.line = line;
    return tok;
}
Keywords keywords[KEYWORDS] = {
    {AND, "and", 3},
    {AS, "as", 2},
    {BREAK, "break", 4},
    {CASE, "case", 4},
    {CLASS, "class", 5},
    {COND, "cond", 4},
    {CONTINUE, "continue", 8},
    {CATCH, "catch", 5},
    {DEL, "del", 3},
    {ELSE, "else", 4},
    {ELIF, "elif", 4},
    {FALL, "fall", 4},
    {FALSE, "False", 5},
    {FINALLY, "final", 5},
    {FOR, "for", 3},
    {FN, "fn", 2},
    {FROM, "from", 4},
    {IF, "if", 2},
    {IN, "in", 2},
    {IS, "is", 2},
    {NOT, "not", 3},
    {NIL, "Nil", 3},
    {OR, "or", 2},
    {PASS, "pass", 4},
    {RAISE, "raise", 5},
    {RET, "return", 6},
    {TRY, "try", 3},
    {TRUE, "True", 4},
    {USE, "use", 3},
    {WHILE, "while", 5},
    {XOR, "xor", 3},
    {YIELD, "yield", 5}};

// First char ↦ index range table for keywords[]. Built once on first
// call. Skips the linear scan for the ~96% of identifiers whose first
// letter has no matching keyword, and shrinks the inner loop for the
// rest. Also fixes a latent OOB read in the previous implementation,
// which memcmp'd `keywords[i].len` bytes of the input even when the
// input was shorter than the keyword.
static int kw_index_built = 0;
static int kw_first_lo[128]; // inclusive
static int kw_first_hi[128]; // exclusive

static void build_kw_index(void)
{
    for (int i = 0; i < 128; i++) { kw_first_lo[i] = -1; kw_first_hi[i] = -1; }
    for (int i = 0; i < KEYWORDS; i++)
    {
        unsigned char c = (unsigned char)keywords[i].keyword[0];
        if (c >= 128) continue;
        if (kw_first_lo[c] < 0) kw_first_lo[c] = i;
        kw_first_hi[c] = i + 1;
    }
    kw_index_built = 1;
}

int iskeyword(const char *s, int len)
{
    if (!kw_index_built) build_kw_index();
    if (len <= 0) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c >= 128) return 0;
    int lo = kw_first_lo[c];
    if (lo < 0) return 0;
    int hi = kw_first_hi[c];
    for (int i = lo; i < hi; i++)
    {
        if (keywords[i].len == len &&
            !memcmp(s, keywords[i].keyword, (size_t)len))
        {
            return keywords[i].key;
        }
    }
    return 0;
}

void printToken(Token *Token)
{
    printf("%.*s\n", Token->length, Token->token);
}