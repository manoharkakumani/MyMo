#include "statement.h"
#include "bytecode.h"
#include "error.h"
#include "expression.h"
#include "memory.h"
#include "datatypes/bool.h"

void synchronize(Compiler *compiler)
{
    compiler->parser->panicMode = false;
    while (compiler->parser->current.type != END)
    {
        if (compiler->parser->previous.type == NEWLINE)
            return;
        switch (compiler->parser->current.type)
        {
        case CLASS:
        case FN:
        case FOR:
        case IF:
        case CASE:
        case WHILE:
        case RET:
            return;
        default:
            // Do nothing.
            ;
        }
        advanceToken(compiler);
    }
}

void and_(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    int andJump = emitJump(compiler, OP_JIF);
    emitByte(compiler, OP_POP);
    parsePrecedence(compiler, PREC_AND);
    patchJump(compiler, andJump);
}

void or_(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    int orJump = emitJump(compiler, OP_JIF);
    int endJump = emitJump(compiler, OP_JMP);
    patchJump(compiler, orJump);
    emitByte(compiler, OP_POP);
    parsePrecedence(compiler, PREC_OR);
    patchJump(compiler, endJump);
}

void trenaryCond(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    int ifJump = emitJump(compiler, OP_JIF);
    emitByte(compiler, OP_POP);
    expression(compiler);
    int jump = emitJump(compiler, OP_JMP);
    patchJump(compiler, ifJump);
    emitByte(compiler, OP_POP);
    consumeToken(compiler, COLON, "expected ':' after expression.");
    expression(compiler);
    patchJump(compiler, jump);
}

// `a if c else b`. The single-pass compiler has already emitted `a` by
// the time it sees `if`; cut that code out, compile `c`, and re-emit it
// in the true branch so `a` is only evaluated when `c` holds (jumps in
// `a` are relative, so the block can move as a whole).
void trenaryCond2(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    Chunk *ch = currentChunk(compiler);
    int start = compiler->infixLeftStart;
    int len = ch->count - start;
    u8 *code = malloc((size_t)len + 1);
    u32 *lines = malloc(sizeof(u32) * ((size_t)len + 1));
    u32 *cols = malloc(sizeof(u32) * ((size_t)len + 1));
    memcpy(code, ch->code + start, (size_t)len);
    memcpy(lines, ch->lines + start, sizeof(u32) * (size_t)len);
    memcpy(cols, ch->cols + start, sizeof(u32) * (size_t)len);
    ch->count = start;

    expression(compiler); // the condition
    int elseJump = emitJump(compiler, OP_JIF);
    emitByte(compiler, OP_POP);
    for (int i = 0; i < len; i++)
        writeChunk(compiler->parser->vm, currentChunk(compiler), code[i], lines[i], cols[i]);
    free(code);
    free(lines);
    free(cols);
    int endJump = emitJump(compiler, OP_JMP);
    patchJump(compiler, elseJump);
    emitByte(compiler, OP_POP);
    consumeToken(compiler, ELSE, "expected 'else' after 'if' expression.");
    expression(compiler);
    patchJump(compiler, endJump);
}

// Is the statement at `current` a multiple assignment `a, b (, c)* = ...`?
// Scans ahead on a copy of the lexer (it's a plain struct), so nothing is
// consumed when the answer is no.
static bool isUnpackAssignment(Compiler *compiler)
{
    if (!checkToken(compiler, NAME))
        return false;
    Lexer probe = *compiler->parser->lexer;
    int names = 1;
    bool expectComma = true;
    for (;;)
    {
        Token t = getToken(&probe);
        if (expectComma)
        {
            if (t.type == COMMA) { expectComma = false; continue; }
            return t.type == EQUAL && names >= 2;
        }
        if (t.type != NAME)
            return false;
        names++;
        expectComma = true;
    }
}

// Parse `NAME (, NAME)*` into `names`; returns the count.
int parseNameList(Compiler *compiler, Token *names, int max)
{
    int n = 0;
    do
    {
        consumeToken(compiler, NAME, "expected a name.");
        if (n == max)
        {
            error(compiler, "too many names to unpack.");
            return n;
        }
        names[n++] = compiler->parser->previous;
    } while (matchToken(compiler, COMMA));
    return n;
}

// After OP_UNPACK n pushed the elements in order, store them into the
// names (last element is on top).
void storeUnpacked(Compiler *compiler, Token *names, int n)
{
    for (int i = n - 1; i >= 0; i--)
    {
        emitStoreName(compiler, &names[i]);
        emitByte(compiler, OP_POP);
    }
}

// `a, b = expr` (expr is usually a tuple: `a, b = b, a` swaps).
static void unpackAssignment(Compiler *compiler)
{
    Token names[64];
    int n = parseNameList(compiler, names, 64);
    consumeToken(compiler, EQUAL, "expected '=' after names.");
    expression(compiler);
    emitBytes(compiler, OP_UNPACK, (u8)n);
    storeUnpacked(compiler, names, n);
}

void expressionStatement(Compiler *compiler)
{
    expression(compiler);
    emitByte(compiler, OP_POP);
}

#define EXIT_BREAK 1
#define EXIT_CONTINUE 2
#define EXIT_RETURN 3

// How a return's value is produced at the exit site.
#define RET_NIL 0   // bare `return`
#define RET_PARSE 1 // `return <expr>`
#define RET_STASH 2 // re-issued after a final block: read <ret>

// Hidden local holding a return value while final blocks run.
static uint returnStash(Compiler *compiler)
{
    Token tok = {.token = "<ret>", .length = 5};
    return identifierConstant(compiler, &tok);
}

static void emitReturnValue(Compiler *compiler, int retMode)
{
    if (retMode == RET_PARSE)
        expression(compiler);
    else if (retMode == RET_STASH)
        emitGetV(compiler, returnStash(compiler));
    else
        emitByte(compiler, OP_NIL);
}

// Emit a return/break/continue from the current position. Walks the open
// loops and try blocks innermost-first, popping the operand-stack slots
// they own (a for's iterator, a final body's [pending, code]); the first
// try in the way gets the exit routed through its final block.
static void emitExit(Compiler *compiler, int kind, int retMode)
{
    Loop *target = kind == EXIT_RETURN ? NULL : compiler->loop;
    Loop *loop = compiler->loop;
    TryCtx *t = compiler->tryCtx;
    for (;;)
    {
        bool takeLoop = loop && (!t || loop->seq > t->seq);
        if (takeLoop)
        {
            if (loop == target)
                break;
            if (loop->isFor)
                emitByte(compiler, OP_POP); // the iterator
            loop = loop->enclosing;
        }
        else if (t)
        {
            if (t->inFinal)
            {
                emitByte(compiler, OP_POP); // exit code
                emitByte(compiler, OP_POP); // pending exception
                t = t->enclosing;
                continue;
            }
            if (t->exitCount >= TRY_MAX_EXITS)
            {
                error(compiler, "too many return/break/continue statements in one try.");
                return;
            }
            if (kind == EXIT_RETURN)
            {
                // Evaluate the value while the try's handlers are still
                // active, then park it for the re-issued return.
                emitReturnValue(compiler, retMode);
                emitSetV(compiler, returnStash(compiler));
                emitByte(compiler, OP_POP);
            }
            for (int i = compiler->tryDepth; i > t->outerDepth; i--)
                emitByte(compiler, OP_ENDTRY);
            emitByte(compiler, OP_NIL);
            emitConstantV(compiler, V_INT_VAL(t->exitCount + 1));
            t->exitKinds[t->exitCount] = (u8)kind;
            t->exitJumps[t->exitCount] = emitJump(compiler, OP_JMP);
            t->exitCount++;
            return;
        }
        else
            break;
    }

    if (kind == EXIT_RETURN)
    {
        if (retMode == RET_NIL)
            emitReturn(compiler);
        else
        {
            emitReturnValue(compiler, retMode);
            emitByte(compiler, OP_FRET);
        }
        return;
    }
    for (int i = compiler->tryDepth; i > target->tryDepth; i--)
        emitByte(compiler, OP_ENDTRY);
    if (kind == EXIT_CONTINUE)
    {
        emitLoop(compiler, target->loopStart);
        return;
    }
    if (target->isFor)
        emitByte(compiler, OP_POP); // the iterator
    if (target->breaksCapacity < target->breaksCount + 1)
    {
        int oldCapacity = target->breaksCapacity;
        target->breaksCapacity = ResizeCapacity(oldCapacity);
        target->breakJumps = ResizeArray(compiler->parser->vm, int, target->breakJumps, oldCapacity, target->breaksCapacity);
    }
    target->breakJumps[target->breaksCount++] = emitJump(compiler, OP_JMP);
}

void breakStatement(Compiler *compiler)
{
    if (compiler->loop == NULL)
    {
        error(compiler, "cannot use 'break' outside of a loop.");
        return;
    }
    emitExit(compiler, EXIT_BREAK, RET_NIL);
}

void continueStatement(Compiler *compiler)
{
    if (compiler->loop == NULL)
    {
        error(compiler, "cannot use 'continue' outside of a loop.");
        return;
    }
    emitExit(compiler, EXIT_CONTINUE, RET_NIL);
}

void returnStatement(Compiler *compiler)
{
    if (compiler->function->type == FN_SCRIPT || compiler->function->type == FN_COMPILED)
    {
        error(compiler, "cannot return from top-level code.");
        return;
    }
    if (compiler->function->type == FN_INIT)
    {
        error(compiler, "cannot return from an initializer.");
    }
    emitExit(compiler, EXIT_RETURN, checkToken(compiler, NEWLINE) ? RET_NIL : RET_PARSE);
}

// del name | del target.attr | del target[key]   (target may chain:
// del a.b[0].c)
void deleteStatement(Compiler *compiler)
{
    consumeToken(compiler, NAME, "expected a name after 'del'.");
    if (!checkToken(compiler, DOT) && !checkToken(compiler, LSQB))
    {
        emitConstOp(compiler, OP_DELV, identifierConstant(compiler, &compiler->parser->previous));
        return;
    }
    compiler->flags.dontSetVar++;
    variable(compiler, false);
    for (;;)
    {
        if (matchToken(compiler, DOT))
        {
            consumeToken(compiler, NAME, "expected a property name after '.'.");
            uint name = identifierConstant(compiler, &compiler->parser->previous);
            if (!checkToken(compiler, DOT) && !checkToken(compiler, LSQB))
            {
                emitConstOp(compiler, OP_DELP, name);
                break;
            }
            emitConstOp(compiler, OP_GETP, name);
        }
        else if (matchToken(compiler, LSQB))
        {
            expression(compiler);
            consumeToken(compiler, RSQB, "expected ']' after the key.");
            if (!checkToken(compiler, DOT) && !checkToken(compiler, LSQB))
            {
                emitByte(compiler, OP_DELSUBSCR);
                break;
            }
            emitByte(compiler, OP_SUBSCR);
        }
        else
        {
            errorAtCurrent(compiler, "expected '.' or '[' in del target.");
            break;
        }
    }
    compiler->flags.dontSetVar--;
}

// global a, b / nonlocal a, b: later assignments in this function go to
// the module global / the enclosing function's variable.
static void scopeStatement(Compiler *compiler, u8 kind)
{
    do
    {
        consumeToken(compiler, NAME, kind == SCOPE_GLOBAL ? "expected a name after 'global'."
                                                          : "expected a name after 'nonlocal'.");
        if (compiler->flags.compileType != COMPILE_FUNCTION && kind == SCOPE_NONLOCAL)
        {
            error(compiler, "nonlocal is only allowed inside a function.");
            return;
        }
        if (compiler->scopeCount == 64)
        {
            error(compiler, "too many global/nonlocal names in one function.");
            return;
        }
        Token *t = &compiler->parser->previous;
        compiler->scopeNames[compiler->scopeCount] = newString(compiler->parser->vm, t->token, t->length);
        compiler->scopeKinds[compiler->scopeCount++] = kind;
    } while (matchToken(compiler, COMMA));
    emitByte(compiler, OP_NOP);
}

// assert cond[, message] — raises "AssertionError[: message]" when cond
// is falsy. The message is only evaluated on failure.
void assertStatement(Compiler *compiler)
{
    parsePrecedence(compiler, PREC_ASSIGNMENT); // not expression(): `c, msg` isn't a tuple
    int failJump = emitJump(compiler, OP_JIF);
    emitByte(compiler, OP_POP);
    int endJump = emitJump(compiler, OP_JMP);
    patchJump(compiler, failJump);
    emitByte(compiler, OP_POP);
    MVM *vm = compiler->parser->vm;
    if (matchToken(compiler, COMMA))
    {
        emitConstant(compiler, NEW_STRING(vm, "AssertionError: ", 16));
        expression(compiler);
        emitByte(compiler, OP_TOSTRING);
        emitBytes(compiler, OP_ADD, 0);
    }
    else
        emitConstant(compiler, NEW_STRING(vm, "AssertionError", 14));
    emitByte(compiler, OP_RAISE);
    patchJump(compiler, endJump);
}

void block(Compiler *compiler, size_t indent)
{
    skipNewLines(compiler);
    if (getIndent(compiler) != (indent + 4))
        errorAtCurrent(compiler, "expected an indented block");
    while (getIndent(compiler) == indent + 4)
    {
        statement(compiler);
        if (checkToken(compiler, END))
            return;
        if (compiler->parser->panicMode)
            synchronize(compiler);
    }
}

void ifStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    if (matchToken(compiler, IF) || matchToken(compiler, ELIF))
    {
        expression(compiler);
        int ifJump = emitJump(compiler, OP_JIF);
        consumeToken(compiler, COLON, "expected ':' after expression.");
        emitByte(compiler, OP_POP);
        if (matchToken(compiler, NEWLINE))
        {
            block(compiler, indent);
        }
        else
        {
            simpleStatement(compiler);
        }
        int Jump = emitJump(compiler, OP_JMP);
        patchJump(compiler, ifJump);
        emitByte(compiler, OP_POP);
        if (checkToken(compiler, ELIF) && (indent == getIndent(compiler)))
            ifStatement(compiler);
        else if (checkToken(compiler, ELSE) && (indent == getIndent(compiler)))
            elseStatement(compiler);
        patchJump(compiler, Jump);
    }
}

void elseStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    advanceToken(compiler);
    consumeToken(compiler, COLON, "expected ':' after expression.");
    if (matchToken(compiler, NEWLINE))
    {
        block(compiler, indent);
    }
    else
    {
        simpleStatement(compiler);
    }
}

// try:
//     <body>
// catch <name>:
//     <handler>
//
// Compiles to:
//     OP_TRY <catch_offset>
//     <body bytecode>
//     OP_ENDTRY
//     OP_JMP <end>
//   catch_label:
//     OP_SETV name    ; the unwinder pushed the exception value
//     OP_POP          ; SETV leaves the value on stack; discard
//     <handler bytecode>
//   end:
// Is `name` spelled like an exception class (catch KeyError:) rather
// than a variable to bind (catch e:)?
static bool looksLikeExceptionClass(Token *name)
{
    static const char *suffixes[] = {"Error", "Exception", "Iteration", "Exit", NULL};
    for (const char **suffix = suffixes; *suffix; suffix++)
    {
        int n = (int)strlen(*suffix);
        if (name->length >= n && memcmp(name->token + name->length - n, *suffix, (size_t)n) == 0)
            return true;
    }
    return false;
}

// `catch e:` binds everything to e; `catch KeyError:` is a typed clause.
static bool isCatchBinding(Compiler *compiler)
{
    if (!checkToken(compiler, NAME) || looksLikeExceptionClass(&compiler->parser->current))
        return false;
    Lexer probe = *compiler->parser->lexer;
    return getToken(&probe).type == COLON;
}

static void blockOrSimple(Compiler *compiler, size_t indent)
{
    if (matchToken(compiler, NEWLINE))
        block(compiler, indent);
    else
        simpleStatement(compiler);
}

// try:
//     body
// catch KeyError as e:        (or catch (KeyError, IndexError) as e:,
//     ...                      catch KeyError:, catch e:, catch:)
// catch e:
//     ...
// final:
//     cleanup (runs after the body/catch, and when an exception escapes)
//
//   OP_TRY H1; body; OP_ENDTRY; OP_NIL; JMP F
//   H1: [exc] -> <exc> hidden local
//       (with final: OP_TRY H2)
//       per clause: [GETV <exc>; types; OP_EXCMATCH; JIF next; POP]
//                   bind; body; (OP_ENDTRY); OP_NIL; JMP F
//                   next: POP
//       no clause matched: (OP_ENDTRY); GETV <exc>; JMP F   (OP_RAISE without final:)
//   H2: [exc2] (falls into F)
//   F:  [pending exception or Nil] final-body; OP_RERAISE
void tryStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    advanceToken(compiler); // consume `try`
    consumeToken(compiler, COLON, "expected ':' after try");
    TryCtx ctx = {.outerDepth = compiler->tryDepth, .seq = ++compiler->scopeSeq,
                  .inFinal = false, .exitCount = 0, .enclosing = compiler->tryCtx};
    compiler->tryCtx = &ctx;
    int tryJump = emitJump(compiler, OP_TRY);
    compiler->tryDepth++;
    blockOrSimple(compiler, indent);
    compiler->tryDepth--;
    emitByte(compiler, OP_ENDTRY);

    // Every path into F pushes [pending, code]: code 0 means "fall out
    // (re-raising pending unless Nil)", k > 0 is ctx.exitKinds[k-1].
    int jumpsToFinal[64];
    int finalJumps = 0;
    emitByte(compiler, OP_NIL);
    emitConstantV(compiler, V_INT_VAL(0));
    jumpsToFinal[finalJumps++] = emitJump(compiler, OP_JMP);
    patchJump(compiler, tryJump);

    // Handler: stash the exception in a hidden local.
    static int tryCounter = 0;
    char excBuf[32];
    Token excTok = {.token = excBuf};
    excTok.length = snprintf(excBuf, sizeof(excBuf), "<exc_%d>", tryCounter++);
    u32 excName = identifierConstant(compiler, &excTok);
    emitSetV(compiler, excName);
    emitByte(compiler, OP_POP);

    // Catch clauses run under their own handler so that a `final:` block
    // still runs if a clause raises (without one, F just re-raises).
    int innerTry = emitJump(compiler, OP_TRY);
    bool catchAll = false;
    while (!catchAll && checkToken(compiler, CATCH) && getIndent(compiler) == indent)
    {
        advanceToken(compiler); // `catch`
        bool typed = false;
        Token binding;
        bool hasBinding = false;
        if (isCatchBinding(compiler))
        {
            // `catch e:` — bind everything.
            advanceToken(compiler);
            binding = compiler->parser->previous;
            hasBinding = true;
        }
        else if (!checkToken(compiler, COLON))
        {
            typed = true;
            emitGetV(compiler, excName);
            parsePrecedence(compiler, PREC_OR);
            if (matchToken(compiler, AS))
            {
                consumeToken(compiler, NAME, "expected a name after 'as'.");
                binding = compiler->parser->previous;
                hasBinding = true;
            }
        }
        int nextClause = -1;
        if (typed)
        {
            emitByte(compiler, OP_EXCMATCH);
            nextClause = emitJump(compiler, OP_JIF);
            emitByte(compiler, OP_POP);
        }
        else
            catchAll = true;
        if (hasBinding)
        {
            emitGetV(compiler, excName);
            emitStoreName(compiler, &binding);
            emitByte(compiler, OP_POP);
        }
        consumeToken(compiler, COLON, "expected ':' after catch clause.");
        compiler->tryDepth++; // the clause runs under the inner handler
        blockOrSimple(compiler, indent);
        compiler->tryDepth--;
        emitByte(compiler, OP_ENDTRY);
        emitByte(compiler, OP_NIL);
        emitConstantV(compiler, V_INT_VAL(0));
        if (finalJumps < 64)
            jumpsToFinal[finalJumps++] = emitJump(compiler, OP_JMP);
        else
            error(compiler, "too many catch clauses.");
        if (nextClause >= 0)
        {
            patchJump(compiler, nextClause);
            emitByte(compiler, OP_POP);
        }
    }
    if (!catchAll)
    {
        // Nothing matched: carry the exception to the final block, which
        // re-raises it after running.
        emitByte(compiler, OP_ENDTRY);
        emitGetV(compiler, excName);
        emitConstantV(compiler, V_INT_VAL(0));
        jumpsToFinal[finalJumps++] = emitJump(compiler, OP_JMP);
    }
    // A clause raised: its exception is pending for the final block.
    patchJump(compiler, innerTry);
    emitConstantV(compiler, V_INT_VAL(0));

    for (int i = 0; i < finalJumps; i++)
        patchJump(compiler, jumpsToFinal[i]);
    for (int i = 0; i < ctx.exitCount; i++)
        patchJump(compiler, ctx.exitJumps[i]);
    ctx.inFinal = true;
    if (checkToken(compiler, FINALLY) && getIndent(compiler) == indent)
    {
        advanceToken(compiler);
        consumeToken(compiler, COLON, "expected ':' after final");
        blockOrSimple(compiler, indent);
    }
    compiler->tryCtx = ctx.enclosing;

    // [pending, code]: re-issue the exit that code names, from outside
    // the try (so an enclosing try's final block runs next).
    for (int i = 0; i < ctx.exitCount; i++)
    {
        emitByte(compiler, OP_DUP);
        emitConstantV(compiler, V_INT_VAL(i + 1));
        emitBytes(compiler, OP_EQUAL, 0);
        int next = emitJump(compiler, OP_JIF);
        emitByte(compiler, OP_POP); // true
        emitByte(compiler, OP_POP); // code
        emitByte(compiler, OP_POP); // pending (Nil)
        u8 kind = ctx.exitKinds[i];
        emitExit(compiler, kind, kind == EXIT_RETURN ? RET_STASH : RET_NIL);
        patchJump(compiler, next);
        emitByte(compiler, OP_POP); // false
    }
    emitByte(compiler, OP_POP); // code
    emitByte(compiler, OP_RERAISE);
}

// raise <expr>:
//     push the value, emit OP_RAISE which runtimeError-routes it
//     to the nearest active OP_TRY handler.
void raiseStatement(Compiler *compiler)
{
    advanceToken(compiler); // consume `raise`
    if (checkToken(compiler, NEWLINE) || checkToken(compiler, END))
    {
        // Bare `raise` re-raises the current exception. Emit a
        // string sentinel for now since we don't yet have access
        // to the in-flight value via the language.
        emitConstant(compiler, NEW_STRING(compiler->parser->vm, "RaiseError", 10));
    }
    else
    {
        expression(compiler);
    }
    emitByte(compiler, OP_RAISE);
}

void startLoop(Compiler *compiler, Loop *loop)
{
    loop->loopStart = currentChunk(compiler)->count;
    loop->enclosing = compiler->loop;
    loop->loopJump = 0;
    loop->breakJumps = NULL;
    loop->breaksCount = 0;
    loop->breaksCapacity = 0;
    loop->tryDepth = compiler->tryDepth;
    loop->isFor = false;
    loop->nameCount = 0;
    loop->seq = ++compiler->scopeSeq;
    compiler->loop = loop;
}

void loopStatement(Compiler *compiler)
{
    Loop loop;
    size_t indent = getIndent(compiler);
    if (matchToken(compiler, FOR))
    {
        // `for x in xs` or `for a, b in pairs` (each element unpacked).
        Token names[64];
        int n = parseNameList(compiler, names, 64);
        consumeToken(compiler, IN, "expected 'in' after iterator name");
        expression(compiler);
        emitByte(compiler, OP_GETI);
        startLoop(compiler, &loop);
        loop.isFor = true;
        for (int i = 0; i < n && i < 8; i++)
            loop.names[loop.nameCount++] = names[i];
        compiler->loop->loopJump = emitJump(compiler, OP_ITER);
        if (n == 1)
            emitStoreName(compiler, &names[0]);
        else
        {
            emitBytes(compiler, OP_UNPACK, (u8)n);
            storeUnpacked(compiler, names, n);
            emitByte(compiler, OP_NIL); // the loop body starts with OP_POP
        }
    }
    else
    {
        consumeToken(compiler, WHILE, "expected 'while or for' to start the loop");
        startLoop(compiler, &loop);
        expression(compiler);
        compiler->loop->loopJump = emitJump(compiler, OP_JIF);
    }
    emitByte(compiler, OP_POP);
    consumeToken(compiler, COLON, "expected ':' after expression.");
    if (matchToken(compiler, NEWLINE))
    {
        block(compiler, indent);
    }
    else
    {
        simpleStatement(compiler);
    }
    emitLoop(compiler, compiler->loop->loopStart);
    int Jump = emitJump(compiler, OP_JMP);
    compiler->loop = loop.enclosing;
    patchJump(compiler, loop.loopJump);
    emitByte(compiler, OP_POP);
    int breaksCount = loop.breaksCount;
    if (checkToken(compiler, ELSE) && (indent == getIndent(compiler)))
    {
        elseStatement(compiler);
    }
    patchJump(compiler, Jump);
    while (breaksCount)
    {
        breaksCount--;
        patchJump(compiler, loop.breakJumps[breaksCount]);
    }
    if (loop.breaksCapacity)
    {
        FreeArray(compiler->parser->vm, int, loop.breakJumps, loop.breaksCapacity);
    }
}

// Synthetic hidden-local name used to stash the case scrutinee so that
// binding patterns can extract from it without touching the operand
// stack. The leading `<` is outside the identifier alphabet, so it can
// never collide with a user-declared name.
static uint caseScrutineeName(Compiler *compiler)
{
    static const char NAME[] = "<scrutinee>";
    Token tok;
    tok.token = NAME;
    tok.length = (int)sizeof(NAME) - 1;
    return identifierConstant(compiler, &tok);
}

// Emit the per-arm binding-extraction sequence after the OP_CJMP equal
// branch. For each binding, walks the recorded path from the scrutinee
// root, emitting `OP_CONST idx + OP_SUBSCR` per step. Empty path (length
// 0) is the whole-scrutinee binding.
static void emitBindingExtractions(Compiler *compiler)
{
    int n = compiler->flags.bindingsCount;
    if (n == 0) return;
    uint scrutIdx = caseScrutineeName(compiler);
    for (int i = 0; i < n; i++)
    {
        uint nameIdx = compiler->flags.bindingNameIdx[i];
        int pathLen = compiler->flags.bindingPathLen[i];
        emitGetV(compiler, scrutIdx);  // push scrutinee root
        for (int d = 0; d < pathLen; d++)
        {
            emitConstantV(compiler, V_INT_VAL((int32_t)compiler->flags.bindingPath[i][d]));
            emitBytes(compiler, OP_SUBSCR, 0);
        }
        emitSetV(compiler, nameIdx);
        emitByte(compiler, OP_POP);
    }
    compiler->flags.bindingsCount = 0;
}

void compileCase(Compiler *compiler, u8 code)
{
    // Mark the parser as inside a case-arm pattern so bare `_` identifiers
    // emit OP_WILDCARD instead of looking up a variable named `_`. Cleared
    // at the end of this function so non-pattern code is unaffected.
    if (code == OP_CJMP) compiler->flags.casePattern++;

    if (code == OP_CJMP)
    {
        compiler->flags.multiCase++;
        // PREC_PITAR rather than PREC_ASSIGNMENT so the ternary-`if` infix
        // rule doesn't grab the guard's `if` token. Pattern syntax is
        // structural (literals, tuples, lists, `_`, `_name`); none of
        // those need PREC_ASSIGNMENT-level parsing.
        parsePrecedence(compiler, PREC_PITAR);
        compiler->flags.multiCase--;
    }
    else
    {
        expression(compiler);
    }
    int multiCases = 0;
    if (matchToken(compiler, COMMA) && code == OP_CJMP)
    {
        compiler->flags.multiCase++;
        do
        {
            multiCases++;
            parsePrecedence(compiler, PREC_PITAR);
        } while (matchToken(compiler, COMMA) && code == OP_CJMP);
        emitBytes(compiler, OP_MCASE, multiCases);
        compiler->flags.multiCase--;
    }
    if (code == OP_CJMP) compiler->flags.casePattern--;
    // Caller now consumes the COLON so it can parse an optional `if guard`
    // clause between the pattern and the colon.
}

void cases(Compiler *compiler, size_t indent, u8 code, int FallJump)
{
    if (getIndent(compiler) != indent + 4)
    {
        if (code == OP_CJMP)
        {
            emitByte(compiler, OP_POP);
        }
        return;
    }
    // `$:` default-arm syntax was retired in Phase 5h. Use `_:` instead;
    // the bare-`_` wildcard pattern matches anything and serves the same
    // role with the rest of the pattern-match infrastructure (bindings,
    // guards, fall-through).
    size_t c_indent = getIndent(compiler);
    compileCase(compiler, code);
    int CJump = emitJump(compiler, code);
    if (code == OP_JIF)
    {
        emitByte(compiler, OP_POP);
    }
    emitBindingExtractions(compiler);
    // Optional guard clause: `pat if expr:`. The predicate runs after
    // bindings are extracted (so it can refer to them) and before the
    // body. On a falsey predicate, jump to a "guard fail" landing that
    // re-pushes the scrutinee from the hidden <scrutinee> local and
    // jumps to where CJump miss-lands, so the next arm sees a fresh
    // scrutinee on stack just like the legacy CJMP-miss path.
    int GuardJump = -1;
    if (code == OP_CJMP && matchToken(compiler, IF))
    {
        expression(compiler);
        GuardJump = emitJump(compiler, OP_JIF);
        emitByte(compiler, OP_POP);  // pop truthy bool on continuation
    }
    consumeToken(compiler, COLON, "expected ':' after pattern.");
    if (FallJump != -1)
    {
        patchJump(compiler, FallJump);
    }
    if (matchToken(compiler, NEWLINE))
    {
        block(compiler, c_indent);
    }
    else
    {
        simpleStatement(compiler);
    }
    int FJump = -1;
    if (matchToken(compiler, FALL) && code == OP_CJMP)
    {
        // See caseStatement() for the placeholder rationale.
        emitByte(compiler, OP_NIL);
        FJump = emitJump(compiler, OP_JMP);
        consumeToken(compiler, NEWLINE, "expected 'Newline' after fall.");
    }
    int Jump = emitJump(compiler, OP_JMP);
    // Guard-fail landing: pop the falsy bool, re-push scrutinee, fall
    // through into the same code path the CJMP-miss takes (next arm).
    if (GuardJump != -1)
    {
        patchJump(compiler, GuardJump);
        emitByte(compiler, OP_POP);
        emitGetV(compiler, caseScrutineeName(compiler));
        // Fall through to the CJMP-miss landing below.
    }
    patchJump(compiler, CJump);
    if (code == OP_JIF)
    {
        emitByte(compiler, OP_POP);
    }
    cases(compiler, indent, code, FJump);
    patchJump(compiler, Jump);
}

void caseStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    u8 code = OP_JIF;
    if (matchToken(compiler, CASE))
    {
        expression(compiler);
        consumeToken(compiler, COLON, "expected ':' after expression.");
        code = OP_CJMP;
        // Stash the scrutinee in a hidden local so binding patterns
        // (`_name`) can extract from it after a successful match. OP_SETV
        // peeks (doesn't pop), so the operand stack is unchanged for the
        // existing case arm bytecode.
        emitSetV(compiler, caseScrutineeName(compiler));
    }
    else
    {
        consumeToken(compiler, COND, "expected 'cond'.");
    }
    if (matchToken(compiler, NEWLINE))
    {
        skipNewLines(compiler);
        if (getIndent(compiler) != (indent + 4))
            errorAtCurrent(compiler, "expected block");

        size_t c_indent = getIndent(compiler);
        compileCase(compiler, code);
        int CJump = emitJump(compiler, code);
        if (code == OP_JIF)
        {
            emitByte(compiler, OP_POP);
        }
        emitBindingExtractions(compiler);
        // Optional guard clause for the first arm. See cases() for the
        // landing-pattern rationale.
        int GuardJump = -1;
        if (code == OP_CJMP && matchToken(compiler, IF))
        {
            expression(compiler);
            GuardJump = emitJump(compiler, OP_JIF);
            emitByte(compiler, OP_POP);
        }
        consumeToken(compiler, COLON, "expected ':' after pattern.");
        if (matchToken(compiler, NEWLINE))
        {
            block(compiler, c_indent);
        }
        else
        {
            simpleStatement(compiler);
        }
        int FJump = -1;
        if (matchToken(compiler, FALL) && code == OP_CJMP)
        {
            // The matched OP_CJMP already popped the scrutinee. If `fall`
            // jumps into a default arm (`$:`), that arm's terminal OP_POP
            // would underflow. Push a NIL placeholder so the default's
            // pop has something to consume. For fall-to-non-default
            // branches the placeholder is harmless — those branches'
            // OP_CJMP is skipped via the FallJump landing point, and the
            // body runs identically with one extra slot in flight that
            // the trailing not-equal-path OP_POP at end-of-cases consumes.
            emitByte(compiler, OP_NIL);
            FJump = emitJump(compiler, OP_JMP);
            consumeToken(compiler, NEWLINE, "expected 'Newline' after fall.");
        }
        int Jump = emitJump(compiler, OP_JMP);
        if (GuardJump != -1)
        {
            patchJump(compiler, GuardJump);
            emitByte(compiler, OP_POP);
            emitGetV(compiler, caseScrutineeName(compiler));
        }
        patchJump(compiler, CJump);
        if (code == OP_JIF)
        {
            emitByte(compiler, OP_POP);
        }
        cases(compiler, indent, code, FJump);
        patchJump(compiler, Jump);
    }
    else
    {
        errorAtCurrent(compiler, "expected an indented block");
    }
}

void simpleStatement(Compiler *compiler)
{
    if (matchToken(compiler, RET))
    {
        returnStatement(compiler);
    }
    else if (matchToken(compiler, DEL))
    {
        deleteStatement(compiler);
    }
    else if (matchToken(compiler, ASSERT))
    {
        assertStatement(compiler);
    }
    else if (matchToken(compiler, GLOBAL))
    {
        scopeStatement(compiler, SCOPE_GLOBAL);
    }
    else if (matchToken(compiler, NONLOCAL))
    {
        scopeStatement(compiler, SCOPE_NONLOCAL);
    }
    else if (matchToken(compiler, PASS))
    {
        emitByte(compiler, OP_NOP);
    }
    else if (matchToken(compiler, BREAK))
    {
        breakStatement(compiler);
    }
    else if (matchToken(compiler, CONTINUE))
    {
        continueStatement(compiler);
    }
    else if (checkToken(compiler, RAISE))
    {
        raiseStatement(compiler);
    }
    else if (isUnpackAssignment(compiler))
    {
        unpackAssignment(compiler);
    }
    else
    {
        expressionStatement(compiler);
    }
    if (compiler->parser->current.type != END)
    {
        consumeToken(compiler, NEWLINE, "expect 'newline' after expression.");
    }
}

MyMoFunction *endFunction(Compiler *compiler)
{
    emitReturn(compiler);
    MyMoFunction *function = compiler->function;
    free(compiler);
    return function;
}

bool getParameter(MyMoString **parameters, MyMoString *name, int count)
{
    for (int c = 0; c < count; c++)
        if (!strcmp(parameters[c]->value, name->value))
            return true;
    return false;
}

void fnParameters(Compiler *compiler)
{
    if (!matchToken(compiler, NAME))
        errorAtCurrent(compiler, "expect parameters.");
    MyMoString *name = newString(compiler->parser->vm, compiler->parser->previous.token, compiler->parser->previous.length);
    if (!getParameter(compiler->function->argv, name, compiler->function->argc))
        compiler->function->argv[compiler->function->argc] = name;
    else
        errorAt(compiler, compiler->parser->previous, "duplicated parameters.");
    compiler->function->argc++;
    // if (matchToken(compiler, EQUAL)){
    //   expression(compiler);
    // }
}

bool operatorMethod(Compiler *compiler, u32 *argc)
{
#define __OP_OVERLOAD__(_argc) \
    do                         \
    {                          \
        *argc = _argc;         \
        return true;           \
    } while (0)
    if (matchToken(compiler, UPLUS))
        __OP_OVERLOAD__(1);
    if (matchToken(compiler, UMINUS))
        __OP_OVERLOAD__(1);
    if (matchToken(compiler, PLUS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, MINUS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EPLUS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EMINUS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, STAR))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, ESTAR))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, SLASH))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, DSTAR))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, ESLASH))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, PERCENT))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EPERCENT))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EDSTAR))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, AMPER))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EAMPER))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, VBAR))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EVBAR))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, CAP))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, ECAP))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, DLESS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EDLESS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, DGREATER))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EDGREATER))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, DEQUAL))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, ELESS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, LESS))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EGREATER))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, GREATER))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, EXCMARK))
        __OP_OVERLOAD__(1);
    if (matchToken(compiler, NEQUAL))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, IN))
        __OP_OVERLOAD__(2);
    if (matchToken(compiler, NOT))
        __OP_OVERLOAD__(1);
    if (matchToken(compiler, IS))
        __OP_OVERLOAD__(2);
#undef __OP_OVERLOAD__
    return false;
}

// Upper bound on stacked `@decorator` lines above one fn.
#define MAX_DECORATORS 16

static void functionStatementDecorated(Compiler *compiler, const u8 *decoratorArgc, int decoratorCount);

void functionStatement(Compiler *compiler)
{
    functionStatementDecorated(compiler, NULL, 0);
}

// Decorators:
//
//     @get("/todos/:id")
//     @log
//     fn show(req): ...
//
// (also on class methods, installed via OP_METV)
// compiles to `show = get("/todos/:id", log(show))`: the decorated fn is
// appended as the LAST argument of each decorator call (a bare `@log` is
// `log(show)`), applied bottom-up. Appending instead of Python's
// `get(path)(show)` keeps route helpers plain two-arg functions that
// also work undecorated (`get("/x", handler)`). Each decorator's callee and args are evaluated top-down
// before the fn is created, leaving the stack as
// [callee1, args1.., callee2, args2..]; the fn is then pushed and each
// OP_CALL (innermost first) consumes one callee + its args + the value
// below it.
void decoratedStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    u8 argcs[MAX_DECORATORS];
    int count = 0;
    while (matchToken(compiler, AT))
    {
        if (count == MAX_DECORATORS)
        {
            errorAtCurrent(compiler, "too many decorators on one function.");
            return;
        }
        // Callee: NAME ('.' NAME)* — a variable or property chain.
        consumeToken(compiler, NAME, "expected decorator name after '@'.");
        variable(compiler, false);
        while (matchToken(compiler, DOT))
            dot(compiler, false);
        u8 argc = 0;
        if (matchToken(compiler, LPAR))
        {
            compiler->flags.argv++;
            u8 kwCount;
            argc = argumentList(compiler, NULL, &kwCount); // no keywords in decorator arguments
            compiler->flags.argv--;
        }
        if (argc == 255)
        {
            error(compiler, "Can't have more than 255 arguments.");
            return;
        }
        argcs[count++] = argc + 1;
        consumeToken(compiler, NEWLINE, "expected newline after decorator.");
        if (getIndent(compiler) != indent)
        {
            errorAtCurrent(compiler, "decorator and fn must share the same indentation.");
            return;
        }
    }
    if (!checkToken(compiler, FN))
    {
        errorAtCurrent(compiler, "expected 'fn' after decorator.");
        return;
    }
    functionStatementDecorated(compiler, argcs, count);
}

static void functionStatementDecorated(Compiler *compiler, const u8 *decoratorArgc, int decoratorCount)
{
    size_t indent = getIndent(compiler);
    int defaults = 0; // trailing parameters with `= expr` defaults
    advanceToken(compiler);
    FunctionType type;
    if (compiler->flags.cl_fn)
    {
        if (compiler->parser->current.length == 8 && memcmp(compiler->parser->current.token, "__init__", 8) == 0)
        {
            type = FN_INIT;
        }
        else
        {
            type = FN_METHOD;
        }
    }
    else
    {
        type = FN_FUNCTION;
    }
    u32 argc = 0;
    bool oper_meth = operatorMethod(compiler, &argc);
    if (oper_meth)
    {
        type = FN_OPERATOR;
    }
    else if (!matchToken(compiler, NAME))
    {
        errorAtCurrent(compiler, "expected function name");
    }
    u32 name = identifierConstant(compiler, &compiler->parser->previous);
    Compiler *fncompiler = initCompiler(compiler->parser->vm, compiler->parser, type);
    compiler->flags.cl_fn = false;
    if (checkToken(compiler, LPAR) || oper_meth)
    {
        consumeToken(compiler, LPAR, "expected '(' after operator method name");
        if (!checkToken(fncompiler, RPAR) || oper_meth)
        {
            if (oper_meth)
            {
                fnParameters(fncompiler);
                argc--;
                if (argc)
                {
                    if (!matchToken(fncompiler, COMMA))
                        errorAtCurrent(fncompiler, "expected  argument as operator method requires upto two arguments.");
                    fnParameters(fncompiler);
                }
            }
            else
            {

                MyMoFunction *fnObj = fncompiler->function;
                // Parameters after `*rest` or a bare `*` are keyword-only.
                // The rest parameter is held back and stored after them,
                // so the frame is [positional..., kwonly..., rest, kw].
                bool seenStar = false;
                MyMoString *restName = NULL;
                do
                {
                    skipNewLines(fncompiler); // parameters may span lines
                    if (checkToken(fncompiler, RPAR))
                        break; // trailing comma
                    if (fnObj->argc > 255)
                    {
                        errorAtCurrent(fncompiler, "cannot have more than 255 parameters.");
                    }
                    if (fnObj->isargs & VARARGS_KW)
                        errorAtCurrent(fncompiler, "no parameter can follow a **keywords parameter.");
                    // *rest collects extra positional arguments into a
                    // tuple, **kw extra keyword arguments into a dict.
                    if (matchToken(fncompiler, STAR))
                    {
                        if (seenStar)
                            errorAtCurrent(fncompiler, "only one *rest (or bare *) parameter.");
                        seenStar = true;
                        if (checkToken(fncompiler, NAME))
                        {
                            fnParameters(fncompiler);
                            restName = fnObj->argv[--fnObj->argc];
                            fnObj->argv[fnObj->argc] = NULL;
                            fnObj->isargs |= VARARGS_REST;
                        }
                        continue;
                    }
                    if (matchToken(fncompiler, DSTAR))
                    {
                        if (restName)
                        {
                            fnObj->argv[fnObj->argc++] = restName;
                            restName = NULL;
                        }
                        fnParameters(fncompiler);
                        fnObj->isargs |= VARARGS_KW;
                        continue;
                    }
                    fnParameters(fncompiler);
                    if (restName && restName->length == compiler->parser->previous.length &&
                        memcmp(restName->value, compiler->parser->previous.token, (size_t)restName->length) == 0)
                        errorAt(fncompiler, fncompiler->parser->previous, "duplicated parameters.");
                    if (seenStar)
                        fnObj->kwonly++;
                    // `name = expr`: the default is compiled into the
                    // ENCLOSING function, so it is evaluated when this `fn`
                    // statement runs (like Python) and can see outer names.
                    if (matchToken(fncompiler, EQUAL))
                    {
                        compiler->flags.argv++; // a comma ends the expression
                        expression(compiler);
                        compiler->flags.argv--;
                        if (defaults == 255)
                            errorAtCurrent(compiler, "too many default values.");
                        defaults++;
                    }
                    else if (defaults > 0 && seenStar)
                    {
                        // Required keyword-only after a defaulted
                        // parameter: `_` holds its place in the defaults.
                        if (defaults == 255)
                            errorAtCurrent(compiler, "too many default values.");
                        emitByte(compiler, OP_WILDCARD);
                        defaults++;
                    }
                    else if (defaults > 0)
                    {
                        errorAt(fncompiler, fncompiler->parser->previous,
                                "a parameter without a default can't follow one with a default.");
                    }
                } while (matchToken(fncompiler, COMMA));
                skipNewLines(fncompiler);
                if (restName)
                    fnObj->argv[fnObj->argc++] = restName;
                if (seenStar && !(fnObj->isargs & VARARGS_REST) && fnObj->kwonly == 0)
                    errorAt(fncompiler, fncompiler->parser->previous, "a bare * must be followed by keyword-only parameters.");
                if (fnObj->kwonly)
                    fnObj->isargs |= VARARGS_KWONLY;
            }
        }
        consumeToken(fncompiler, RPAR, "expected ')' after parameters.");
    }
    consumeToken(fncompiler, COLON, "expected ':' before function body.");
    if (matchToken(fncompiler, NEWLINE))
    {
        block(fncompiler, indent);
    }
    else
    {
        simpleStatement(fncompiler);
    }
    MyMoFunction *function = endFunction(fncompiler);
    if (defaults > 0)
    {
        // The default values are on the stack: attach them to the function
        // before OP_FN/OP_MET publishes it (closure copies inherit them).
        emitConstOp(compiler, OP_CONST, makeConstant(compiler, AS_OBJECT(function)));
        emitBytes(compiler, OP_DEFAULTS, (u8)defaults);
        emitByte(compiler, OP_POP);
    }
    switch (type)
    {
    case FN_INIT:
    case FN_METHOD:
    case FN_OPERATOR:
        if (decoratorCount > 0)
        {
            // [class, decorator callees/args...] + method -> decorated
            // value, installed under `name` by OP_METV.
            emitConstOp(compiler, OP_CONST, makeConstant(compiler, AS_OBJECT(function)));
            for (int i = decoratorCount - 1; i >= 0; i--)
                emitBytes(compiler, OP_CALL, decoratorArgc[i]);
            emitConstOp(compiler, OP_CONST, makeConstant(compiler, AS_OBJECT(function)));
            emitConstOp(compiler, OP_METV, name);
        }
        else
            emitConstOp2(compiler, OP_MET, makeConstant(compiler, AS_OBJECT(function)), name);
        compiler->flags.cl_fn = true;
        break;
    case FN_FUNCTION:
        emitConstOp(compiler, OP_FN, makeConstant(compiler, AS_OBJECT(function)));
        for (int i = decoratorCount - 1; i >= 0; i--)
            emitBytes(compiler, OP_CALL, decoratorArgc[i]);
        emitSetV(compiler, name);
        emitByte(compiler, OP_POP);
        break;
    default:
        break;
    }
}

void classStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    advanceToken(compiler);
    if (!matchToken(compiler, NAME))
    {
        errorAtCurrent(compiler, "expected class name");
    }
    uint name = identifierConstant(compiler, &compiler->parser->previous);
    u8 superClasses = 0;
    emitConstOp(compiler, OP_CLASS, name);
    if (matchToken(compiler, LPAR))
    {
        if (!checkToken(compiler, RPAR))
        {
            compiler->flags.tuple = true;
            do
            {
                if (superClasses == 255)
                {
                    error(compiler, "cannot have more than 255 super classes.");
                }
                expression(compiler);
                superClasses++;
            } while (matchToken(compiler, COMMA));
            compiler->flags.tuple = false;
        }
        if (superClasses)
        {
            emitConstOp(compiler, OP_SUPERARGS, makeConstantV(compiler, V_INT_VAL((int32_t)superClasses)));
        }
        consumeToken(compiler, RPAR, "expected ')' after arguments.");
    }
    consumeToken(compiler, COLON, "expected ':' before class body.");
    if (matchToken(compiler, NEWLINE))
    {
        compiler->flags.cl_fn = true;
        block(compiler, indent);
    }
    else
    {
        simpleStatement(compiler);
    }
    emitByte(compiler, OP_ENDCLASS);
    emitSetV(compiler, name);
    emitByte(compiler, OP_POP);
    compiler->flags.cl_fn = false;
}

void compoundStatement(Compiler *compiler)
{
    switch (compiler->parser->current.type)
    {
    case CLASS:
        classStatement(compiler);
        break;
    case FN:
        functionStatement(compiler);
        break;
    default:
        errorAtCurrent(compiler, "Invalid compound statement");
        break;
    }
}
void flowStatement(Compiler *compiler)
{
    switch (compiler->parser->current.type)
    {
    case IF:
        ifStatement(compiler);
        break;
    case ELIF:
        errorAtCurrent(compiler, "Unexpected elif with out if statement");
        matchToken(compiler, ELIF);
        break;
    case ELSE:
        errorAtCurrent(compiler, "Unexpected else with out if or while statement");
        matchToken(compiler, ELSE);
        break;
    case CASE:
    case COND:
        caseStatement(compiler);
        break;
    case FOR:
    case WHILE:
        loopStatement(compiler);
        break;
    default:
        errorAtCurrent(compiler, "Invalid flow statement");
    }
}

void fromStatement(Compiler *compiler)
{
    advanceToken(compiler);
    if (!matchToken(compiler, STRING))
    {
        errorAtCurrent(compiler, "expected module name Ex:- 'from \"module\" use abc'");
    }
    u32 path = identifierConstant(compiler, &compiler->parser->previous);
    consumeToken(compiler, USE, "expected 'use' after module name");
    if (matchToken(compiler, STAR))
    {
        emitByte(compiler, OP_FALSE);
        emitConstOp(compiler, OP_USE, path);
        consumeToken(compiler, NEWLINE, "expected newline after * iin use statement");
        emitByte(compiler, OP_COPY);
        return;
    }
multiFrom:
    emitByte(compiler, OP_FALSE);
    emitConstOp(compiler, OP_USE, path);
    if (!matchToken(compiler, NAME))
    {
        errorAtCurrent(compiler, "expected property name Ex:- 'from \"module\" use abc'");
    }
    u32 name = identifierConstant(compiler, &compiler->parser->previous);
    emitConstOp(compiler, OP_GETP, name);
    if (checkToken(compiler, AS))
    {
        advanceToken(compiler);
        if (!matchToken(compiler, NAME))
        {
            errorAtCurrent(compiler, "expected module name  variable Ex:- 'from \"module\" use abc as a'");
        }
        name = identifierConstant(compiler, &compiler->parser->previous);
    }
    emitSetV(compiler, name);
    emitByte(compiler, OP_POP);
    if (matchToken(compiler, NEWLINE) || matchToken(compiler, END))
    {
        return;
    }
    else if (matchToken(compiler, COMMA))
    {
        goto multiFrom;
    }
    else
    {
        errorAtCurrent(compiler, "expected newline or ',' after use statement");
    }
}

void useStatement(Compiler *compiler)
{
multiUse:
    advanceToken(compiler);
    int noAs = 1;
    if (!matchToken(compiler, STRING))
    {
        errorAtCurrent(compiler, "expected module name Ex:- 'use \"module\"'");
    }
    u32 path = identifierConstant(compiler, &compiler->parser->previous);
    emitByte(compiler, OP_TRUE);
    emitConstOp(compiler, OP_USE, path);
    if (checkToken(compiler, AS))
    {
        advanceToken(compiler);
        if (!matchToken(compiler, NAME))
        {
            errorAtCurrent(compiler, "expected module name  variable Ex:- 'use \"module\" as mod'");
        }
        noAs = 0;
        emitByte(compiler, OP_POP);
        emitSetV(compiler, identifierConstant(compiler, &compiler->parser->previous));
        emitByte(compiler, OP_POP);
    }
    if (matchToken(compiler, NEWLINE) || matchToken(compiler, END))
    {
        if (noAs)
        {
            emitByte(compiler, OP_SETM);
        }
        return;
    }
    else if (matchToken(compiler, COMMA))
    {
        goto multiUse;
    }
    else
    {
        errorAtCurrent(compiler, "expected newline or ',' after use statement");
    }
}

void moduleStatement(Compiler *compiler)
{
    switch (compiler->parser->current.type)
    {
    case USE:
        useStatement(compiler);
        break;
    case FROM:
        fromStatement(compiler);
        break;
    default:
        errorAtCurrent(compiler, "Invalid package statement");
        break;
    }
}
// ------------------------------------------------ command calls, blocks
//
//   h1 "Hello"                   ->  h1("Hello")
//   input name, placeholder="x"  ->  input(name, placeholder="x")
//   button "Save" primary        ->  button("Save", primary=True)   (flags)
//   button "Go" icon="x"         ->  button("Go", icon="x")  (comma optional)
//   card "Title":                ->  card("Title", body=<block>)
//       text "{n} items"                 (strings with {..} interpolate)
//   button "+": count += 1       ->  button("+", body=<block>)
//   card(title="x"):             ->  card(title="x", body=<block>)
//   divider                      ->  divider() when it's callable
//
// A block is a closure (MyMoFunction.block) passed as the keyword
// argument `body`. It reads and assigns the enclosing scope's variables
// live (OP_SETB), except the loop variables of `for` loops around it,
// which it pins when it is created (MyMoFunction.bound).

static bool nameThenEqual(Compiler *compiler)
{
    if (!checkToken(compiler, NAME))
        return false;
    Lexer probe = *compiler->parser->lexer;
    return getToken(&probe).type == EQUAL;
}

// Does the statement at `current` (a NAME) start a command call? Scans a
// copy of the lexer. *bare: a lone `name` / `a.b` on its own line.
static bool isCommandCall(Compiler *compiler, bool *bare)
{
    *bare = false;
    if (!checkToken(compiler, NAME))
        return false;
    Lexer probe = *compiler->parser->lexer;
    Token prev = compiler->parser->current;
    Token t = getToken(&probe);
    while (t.type == DOT)
    {
        prev = getToken(&probe);
        if (prev.type != NAME)
            return false;
        t = getToken(&probe);
    }
    switch (t.type)
    {
    case STRING:
    case FSTRING:
    case INT:
    case DOUBLE:
    case NAME:
    case TRUE:
    case FALSE:
    case NIL:
    case COLON:
        return true;
    case LSQB:
    case LBRACE:
        // `bullets [1, 2]`, but `xs[1]` stays a subscript.
        return t.token > prev.token + prev.length;
    case LPAR:
    {
        // `card(title="x"):` is a call with a block.
        int depth = 1;
        for (;;)
        {
            Token u = getToken(&probe);
            if (u.type == END || u.type == ERROR)
                return false;
            if (u.type == LPAR || u.type == LSQB || u.type == LBRACE)
                depth++;
            else if ((u.type == RPAR || u.type == RSQB || u.type == RBRACE) && --depth == 0)
                break;
        }
        return getToken(&probe).type == COLON;
    }
    case NEWLINE:
    case END:
        *bare = !compiler->parser->repl; // the REPL prints a bare name instead
        return *bare;
    default:
        return false;
    }
}

// One argument of a command call. A plain string with {...} in it is
// an f-string here, so `text "Hi {name}"` needs no `f`.
static void commandArgument(Compiler *compiler)
{
    Token *cur = &compiler->parser->current;
    if (cur->type == STRING && memchr(cur->token, '{', (size_t)cur->length))
        cur->type = FSTRING;
    expression(compiler);
}

// The block after a command's `:`, compiled as a closure and left on the
// stack: an indented block on the following lines, or one statement on
// the same line.
static void compileBlock(Compiler *compiler, size_t indent)
{
    MVM *vm = compiler->parser->vm;
    Compiler *bc = initCompiler(vm, compiler->parser, FN_FUNCTION);
    bc->function->block = true;
    bc->function->name = newString(vm, "<block>", 7);
    if (matchToken(bc, NEWLINE))
        block(bc, indent);
    else
        statement(bc);
    MyMoFunction *fn = endFunction(bc);
    emitConstOp(compiler, OP_FN, makeConstant(compiler, AS_OBJECT(fn)));
    // Pin the loop variables of every enclosing `for`, so a block made in
    // a loop sees its own iteration's values when it runs later.
    for (Loop *l = compiler->loop; l; l = l->enclosing)
        for (int i = 0; i < l->nameCount; i++)
        {
            Token name = l->names[i];
            uint idx = identifierConstant(compiler, &name);
            compiler->parser->previous = name;
            variable(compiler, false); // loads it (arg slot, local or global)
            emitConstOp(compiler, OP_BLOCKVAR, idx);
        }
}

static void commandCall(Compiler *compiler, bool bare)
{
    size_t indent = getIndent(compiler);
    advanceToken(compiler); // the name
    variable(compiler, false);
    while (matchToken(compiler, DOT))
    {
        consumeToken(compiler, NAME, "expected a name after '.'.");
        emitConstOp(compiler, OP_GETP, identifierConstant(compiler, &compiler->parser->previous));
    }
    if (bare)
    {
        emitByte(compiler, OP_CALLIF);
        emitByte(compiler, OP_POP);
        if (!checkToken(compiler, END))
            consumeToken(compiler, NEWLINE, "expect 'newline' after expression.");
        return;
    }
    u16 kwNames[255];
    u8 kwc = 0;  // keyword arguments (their values follow the positional ones)
    int argc = 0; // all arguments, keywords included (OP_CALLKW's count)
    compiler->flags.argv++; // a comma ends each argument
    if (matchToken(compiler, LPAR))
        argc = argumentList(compiler, kwNames, &kwc); // consumes the ')'

    else
    {
        while (!checkToken(compiler, COLON) && !checkToken(compiler, NEWLINE) && !checkToken(compiler, END))
        {
            if (nameThenEqual(compiler))
            {
                advanceToken(compiler);
                Token name = compiler->parser->previous;
                advanceToken(compiler); // =
                commandArgument(compiler);
                argc++;
                if (kwc == 254)
                    error(compiler, "too many keyword arguments.");
                else
                    kwNames[kwc++] = (u16)identifierConstant(compiler, &name);
            }
            else
            {
                if (kwc > 0)
                    errorAtCurrent(compiler, "a positional argument can't follow keyword arguments.");
                commandArgument(compiler);
                argc++;
            }
            // After an argument, without a comma: `flag` means flag=True,
            // `key=value` is a keyword argument.
            while (checkToken(compiler, NAME) && kwc < 254)
            {
                bool keyword = nameThenEqual(compiler);
                advanceToken(compiler);
                Token name = compiler->parser->previous;
                if (keyword)
                {
                    advanceToken(compiler); // =
                    commandArgument(compiler);
                }
                else
                    emitByte(compiler, OP_TRUE);
                kwNames[kwc++] = (u16)identifierConstant(compiler, &name);
                argc++;
            }
            if (!matchToken(compiler, COMMA))
                break;
        }
    }
    compiler->flags.argv--;
    bool hasBlock = matchToken(compiler, COLON);
    if (hasBlock)
    {
        compileBlock(compiler, indent);
        Token body = {.token = "body", .length = 4};
        kwNames[kwc++] = (u16)identifierConstant(compiler, &body);
        argc++;
    }
    if (argc > 255)
        error(compiler, "too many arguments.");
    if (kwc > 0)
    {
        emitByte(compiler, OP_CALLKW);
        emitByte(compiler, (u8)argc);
        emitByte(compiler, kwc);
        for (int i = 0; i < kwc; i++)
        {
            emitByte(compiler, (u8)(kwNames[i] >> 8));
            emitByte(compiler, (u8)(kwNames[i] & 0xff));
        }
    }
    else
        emitBytes(compiler, OP_CALL, (u8)argc);
    emitByte(compiler, OP_POP);
    // The block consumed its own line(s).
    if (!hasBlock && !checkToken(compiler, END))
        consumeToken(compiler, NEWLINE, "expect 'newline' after the command's arguments.");
}

void statement(Compiler *compiler)
{
    if (checkToken(compiler, IF) || checkToken(compiler, ELIF) || checkToken(compiler, ELSE) || checkToken(compiler, CASE) || checkToken(compiler, COND) || checkToken(compiler, WHILE) || checkToken(compiler, FOR))
        flowStatement(compiler);
    else if (checkToken(compiler, CLASS) || checkToken(compiler, FN))
        compoundStatement(compiler);
    else if (checkToken(compiler, AT))
        decoratedStatement(compiler);
    else if (checkToken(compiler, TRY))
        tryStatement(compiler);
    else if (checkToken(compiler, USE) || checkToken(compiler, FROM))
    {
        moduleStatement(compiler);
    }
    else
    {
        bool bare;
        if (isCommandCall(compiler, &bare))
            commandCall(compiler, bare);
        else
            simpleStatement(compiler);
    }
}

void statements(Compiler *compiler)
{
    if (getIndent(compiler))
    {
        errorAtCurrent(compiler, "Invalid indentation");
    }
    while (!getIndent(compiler))
    {
        statement(compiler);
        if (compiler->parser->panicMode)
            synchronize(compiler);
        if (matchToken(compiler, END))
            break;
        if (getIndent(compiler))
            errorAtCurrent(compiler, "Invalid indentation");
    }
}