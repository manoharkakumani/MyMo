#include "lexer.h"

// Hot path — called once per source byte. Force-inline so getToken's
// tight loops (digits, identifiers, strings, whitespace runs) don't
// pay a function-call hop per character.
static inline void lexerAdvanceFast(Lexer *lexer)
{
    lexer->currentChar = lexer->src[lexer->srcLen++];
    lexer->col++;
    lexer->len++;
}

void lexerAdvance(Lexer *lexer) { lexerAdvanceFast(lexer); }

static inline Token lexerAdvanceTokenFast(Lexer *lexer, Token token)
{
    lexerAdvanceFast(lexer);
    return token;
}

Token lexerAdvanceToken(Lexer *lexer, Token token)
{
    return lexerAdvanceTokenFast(lexer, token);
}

// In the rest of this file, switch the heavily-used identifiers over
// to the inlined variants without rewriting every call site.
#define lexerAdvance(L)              lexerAdvanceFast(L)
#define lexerAdvanceToken(L, T)      lexerAdvanceTokenFast((L), (T))

Token getToken(Lexer *lexer)
{
    if (lexer->currentChar != '\0')
    {
        if (lexer->line == 0)
        {
            while (isspace(lexer->currentChar))
            {
                if (lexer->currentChar == '\n')
                {
                    break;
                }
                lexer->indent++;
                lexerAdvance(lexer);
            }
            lexer->line++;
        }
    }
    while (lexer->currentChar != '\0')
    {
        int num_type = 0;
        int start = lexer->col;
        lexer->len = 1;
        lexer->token = lexer->src + lexer->srcLen - 1;
        if (lexer->currentChar == '\n')
        {
            Token tok = newToken("Newline", NEWLINE, lexer->len + 7, start, lexer->indent, lexer->line);
            lexer->line++;
            lexer->col = 0;
            lexer->indent = 0;
            lexerAdvance(lexer);
        newline:
            while (lexer->currentChar == '\n')
            {
                lexer->line++;
                lexer->col = 0;
                lexer->indent = 0;
                lexerAdvance(lexer);
            }
            while (lexer->currentChar == '\t')
            {
                lexer->indent += 4;
                lexer->col += 3;
                lexerAdvance(lexer);
                if (lexer->currentChar == '\n')
                    goto newline;
            }
            while (isspace(lexer->currentChar))
            {
                lexer->indent++;
                lexerAdvance(lexer);
                if (lexer->currentChar == '\n')
                    goto newline;
            }
            return tok;
        }
        else if (lexer->currentChar == 13)
        {
            lexerAdvance(lexer);
        }
        else if (isspace(lexer->currentChar))
        {
            lexerAdvance(lexer);
        }
        else if (lexer->currentChar == '#')
        {
            // Snapshot whether the opener sits at line-start (only
            // leading whitespace before it). col here points at the
            // first '#' (no advance yet for it).
            int openFullLine = ((int)lexer->col - (int)lexer->indent) <= 1;
            lexerAdvance(lexer);
            if (lexer->currentChar == '#')
            {
                lexerAdvance(lexer);
            comment:
                while (lexer->currentChar != '#')
                {
                    if (lexer->currentChar == '\0')
                        break;
                    if (lexer->currentChar == '\n')
                    {
                        lexer->line++;
                        lexer->col = 0;
                        lexer->indent = 0;
                    }
                    lexerAdvance(lexer);
                }
                if (lexer->currentChar == '#')
                {
                    lexerAdvance(lexer);
                    if (lexer->currentChar == '#')
                        lexerAdvance(lexer);
                    else
                        goto comment;
                }
                // Block comment closed. If both the opening `##` was
                // at line-start AND the closing `##` is at end-of-line
                // (currentChar == '\n' with nothing else on its line
                // after the close), eat that trailing `\n` so the
                // parser doesn't see a stray Newline. Same reason as
                // the single-line case below. Inline `## ... ##` (e.g.
                // `y = ##foo## 5`) is left untouched.
                if (openFullLine && lexer->currentChar == '\n')
                {
                    while (lexer->currentChar == '\n')
                    {
                        lexer->line++;
                        lexer->col = 0;
                        lexer->indent = 0;
                        lexerAdvance(lexer);
                    }
                    while (lexer->currentChar == '\t')
                    {
                        lexer->indent += 4;
                        lexer->col += 3;
                        lexerAdvance(lexer);
                    }
                    while (lexer->currentChar == ' ')
                    {
                        lexer->indent++;
                        lexerAdvance(lexer);
                    }
                }
            }
            else
            {
                // Single-line `# ...`. If it sits at the start of its
                // line, swallow the trailing `\n` so the parser
                // doesn't see a stray Newline between two statements.
                // Inline `code # foo` leaves the `\n` for the outer
                // loop to emit a normal statement-terminator.
                while (lexer->currentChar != '\n')
                {
                    if (lexer->currentChar == '\0')
                        break;
                    lexerAdvance(lexer);
                }
                if (openFullLine && lexer->currentChar == '\n')
                {
                    while (lexer->currentChar == '\n')
                    {
                        lexer->line++;
                        lexer->col = 0;
                        lexer->indent = 0;
                        lexerAdvance(lexer);
                    }
                    while (lexer->currentChar == '\t')
                    {
                        lexer->indent += 4;
                        lexer->col += 3;
                        lexerAdvance(lexer);
                    }
                    while (lexer->currentChar == ' ')
                    {
                        lexer->indent++;
                        lexerAdvance(lexer);
                    }
                }
            }
        }
        else if (isdigit(lexer->currentChar))
        {
            if (lexer->currentChar == '0')
            {
                while (lexer->currentChar == '0')
                    lexerAdvance(lexer);
                if (lexer->currentChar == '.')
                {
                    goto fraction;
                }
                else if (lexer->currentChar == 'x' || lexer->currentChar == 'X')
                {
                    lexerAdvance(lexer);
                    if (!(isxdigit(lexer->currentChar)))
                    {
                        return newToken("Invalid Hex literal", ERROR, lexer->len, start, lexer->indent, lexer->line);
                    }
                    while (isxdigit(lexer->currentChar))
                    {
                        lexerAdvance(lexer);
                        if (lexer->currentChar == '.')
                        {
                            return newToken("Invalid Hex literal", ERROR, lexer->len, start, lexer->indent, lexer->line);
                        }
                    }
                }
                else if (lexer->currentChar == 'o' || lexer->currentChar == 'O')
                {
                    lexerAdvance(lexer);
                    if (!(isxdigit(lexer->currentChar)))
                    {
                        return newToken("Invalid Octal literal", ERROR, lexer->len, start, lexer->indent, lexer->line);
                    }
                    while (isxdigit(lexer->currentChar))
                    {
                        lexerAdvance(lexer);
                        if (lexer->currentChar == '.')
                        {
                            return newToken("Invalid Octal literal", ERROR, lexer->len, start, lexer->indent, lexer->line);
                        }
                    }
                }
                else
                    goto ints;
            }
            else
            {
            ints:
                while (isdigit(lexer->currentChar))
                {
                    lexerAdvance(lexer);
                    if (lexer->currentChar == 'e' || lexer->currentChar == 'E')
                        goto exponent;
                }
            fraction:
                if (lexer->currentChar == '.')
                {
                    do
                    {
                        num_type = 1;
                        lexerAdvance(lexer);
                        if (lexer->currentChar == 'e' || lexer->currentChar == 'E')
                            goto exponent;
                        if (lexer->currentChar == '.')
                        {
                            return newToken("Invalid Number", ERROR, lexer->len, start, lexer->indent, lexer->line);
                        }
                    } while (isdigit(lexer->currentChar));
                }
            exponent:
                if (lexer->currentChar == 'e' || lexer->currentChar == 'E')
                {
                    num_type = 1;
                    lexerAdvance(lexer);
                    if (lexer->currentChar == '+' || lexer->currentChar == '-')
                    {
                        lexerAdvance(lexer);
                    }
                    while (isdigit(lexer->currentChar))
                    {
                        if (lexer->currentChar == '.')
                        {
                            return newToken("Invalid Number", ERROR, lexer->len, start, lexer->indent, lexer->line);
                        }
                        lexerAdvance(lexer);
                    }
                }
            }
            if (isalpha(lexer->currentChar))
                return newToken("Invalid identifier", ERROR, lexer->len, start, lexer->indent, lexer->line);
            return newToken(lexer->token, num_type ? DOUBLE : INT, lexer->len - 1, start, lexer->indent, lexer->line);
        }
        else if (lexer->currentChar == 39 || lexer->currentChar == '"' || lexer->currentChar == '`')
        {
            int s = lexer->currentChar;
            lexer->token = lexer->src + lexer->srcLen;
            lexerAdvance(lexer);
            while (lexer->currentChar != s)
            {
                if (lexer->currentChar == '\0' || (lexer->currentChar == '\n' && s != '`'))
                {
                    return newToken("Unexpected EOL or EOF", ERROR, lexer->len, start, lexer->indent, lexer->line);
                }
                if (lexer->currentChar == '\n')
                {
                    lexer->line++;
                    lexer->col = 0;
                }
                if (lexer->currentChar == 13)
                {
                    lexerAdvance(lexer);
                }
                if (lexer->currentChar == '\t')
                {
                    lexer->col += 3;
                }
                lexerAdvance(lexer);
            }
            return lexerAdvanceToken(lexer, newToken(lexer->token, STRING, lexer->len - 2, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '_' || isalpha(lexer->currentChar))
        {
            while (lexer->currentChar == '_' || isalnum(lexer->currentChar))
            {
                lexerAdvance(lexer);
            }
            int k = iskeyword(lexer->token, lexer->len - 1);
            if (k)
            {
                return newToken(lexer->token, k, lexer->len - 1, start, lexer->indent, lexer->line);
            }
            else
            {
                return newToken(lexer->token, NAME, lexer->len - 1, start, lexer->indent, lexer->line);
            }
        }
        else if (lexer->currentChar == '+')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, EPLUS, lexer->len, start, lexer->indent, lexer->line));
                break;
            case '@':
                return lexerAdvanceToken(lexer, newToken(lexer->token, UPLUS, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, PLUS, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '-')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, EMINUS, lexer->len, start, lexer->indent, lexer->line));
                break;
            case '@':
                return lexerAdvanceToken(lexer, newToken(lexer->token, UMINUS, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, MINUS, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '*')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '*':
                lexerAdvance(lexer);
                switch (lexer->currentChar)
                {
                case '=':
                    return lexerAdvanceToken(lexer, newToken(lexer->token, EDSTAR, lexer->len, start, lexer->indent, lexer->line));
                    break;
                default:
                    return newToken(lexer->token, DSTAR, lexer->len - 1, start, lexer->indent, lexer->line);
                    break;
                }
                break;
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, ESTAR, lexer->len, start, lexer->indent, lexer->line));

                break;
            default:
                return newToken(lexer->token, STAR, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '/')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '/':
                lexerAdvance(lexer);
                switch (lexer->currentChar)
                {
                case '=':
                    return lexerAdvanceToken(lexer, newToken(lexer->token, EDSLASH, lexer->len, start, lexer->indent, lexer->line));
                    break;
                default:
                    return newToken(lexer->token, DSLASH, lexer->len - 1, start, lexer->indent, lexer->line);
                    break;
                }
                break;
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, ESLASH, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, SLASH, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '%')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, EPERCENT, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, PERCENT, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '<')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '<':
                lexerAdvance(lexer);
                switch (lexer->currentChar)
                {
                case '=':
                    return lexerAdvanceToken(lexer, newToken(lexer->token, EDLESS, lexer->len, start, lexer->indent, lexer->line));

                    break;
                default:
                    return newToken(lexer->token, DLESS, lexer->len - 1, start, lexer->indent, lexer->line);
                    break;
                }
                break;
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, ELESS, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, LESS, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '>')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '>':
                lexerAdvance(lexer);
                switch (lexer->currentChar)
                {
                case '=':
                    return lexerAdvanceToken(lexer, newToken(lexer->token, EDGREATER, lexer->len, start, lexer->indent, lexer->line));
                    break;
                default:
                    return newToken(lexer->token, DGREATER, lexer->len - 1, start, lexer->indent, lexer->line);
                    break;
                }
                break;
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, EGREATER, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, GREATER, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '|')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, EVBAR, lexer->len, start, lexer->indent, lexer->line));
                break;
            case '>':
                return lexerAdvanceToken(lexer, newToken(lexer->token, PITHRU, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, VBAR, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '&')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, EAMPER, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, AMPER, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '^')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, ECAP, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, CAP, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '=')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, DEQUAL, lexer->len, start, lexer->indent, lexer->line));
                break;
            case '>':
                return lexerAdvanceToken(lexer, newToken(lexer->token, ARROW, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, EQUAL, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '!')
        {
            lexerAdvance(lexer);
            switch (lexer->currentChar)
            {
            case '=':
                return lexerAdvanceToken(lexer, newToken(lexer->token, NEQUAL, lexer->len, start, lexer->indent, lexer->line));
                break;
            default:
                return newToken(lexer->token, EXCMARK, lexer->len - 1, start, lexer->indent, lexer->line);
                break;
            }
        }
        else if (lexer->currentChar == '$')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, DOLLAR, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '@')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, AT, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '~')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, TILD, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '?')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, QMARK, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '.')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, DOT, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '(')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, LPAR, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == ')')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, RPAR, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '[')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, LSQB, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == ']')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, RSQB, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '{')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, LBRACE, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '}')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, RBRACE, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == ':')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, COLON, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == ';')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, SEMI, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == ',')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, COMMA, lexer->len, start, lexer->indent, lexer->line));
        }
        else if (lexer->currentChar == '\\')
        {
            return lexerAdvanceToken(lexer, newToken(lexer->token, BSLASH, lexer->len, start, lexer->indent, lexer->line));
        }
        else
        {
            return newToken("Unidentified Character", ERROR, lexer->len, start, lexer->indent, lexer->line);
        }
    }
    return newToken("END", END, 0, 0, 0, lexer->line + 1);
}

Lexer *initLexer(const char *src)
{
    Lexer *lexer = New(Lexer, 1);
    lexer->line = 0;
    lexer->col = 0;
    lexer->indent = 0;
    lexer->src = src;
    lexer->srcLen = 0;
    lexer->len = 0;
    lexerAdvance(lexer);
    return lexer;
}

void freeLexer(Lexer *lexer)
{
    free(lexer);
}