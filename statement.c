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

void breakStatement(Compiler *compiler)
{
    Loop *loop = compiler->loop;
    if (loop == NULL)
    {
        error(compiler, "cannot use 'break' outside of a loop.");
        return;
    }
    if (loop->breaksCapacity < loop->breaksCount + 1)
    {
        int oldCapacity = loop->breaksCapacity;
        loop->breaksCapacity = ResizeCapacity(oldCapacity);
        loop->breakJumps = ResizeArray(compiler->parser->vm, int, loop->breakJumps, oldCapacity, loop->breaksCapacity);
    }
    for (int i = compiler->tryDepth; i > loop->tryDepth; i--)
        emitByte(compiler, OP_ENDTRY);
    if (loop->isFor)
        emitByte(compiler, OP_POP); // the iterator
    compiler->loop->breakJumps[compiler->loop->breaksCount] = emitJump(compiler, OP_JMP);
    compiler->loop->breaksCount++;
    return;
}

void continueStatement(Compiler *compiler)
{
    if (compiler->loop == NULL)
    {
        error(compiler, "cannot use 'continue' outside of a loop.");
        return;
    }
    for (int i = compiler->tryDepth; i > compiler->loop->tryDepth; i--)
        emitByte(compiler, OP_ENDTRY);
    emitLoop(compiler, compiler->loop->loopStart);
    return;
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
    // Drop the iterators of every enclosing `for` in this function so
    // OP_FRET finds the callee slot where it expects it. The loop
    // variable was already stored, so the return expression can use it.
    for (Loop *loop = compiler->loop; loop; loop = loop->enclosing)
        if (loop->isFor)
            emitByte(compiler, OP_POP);
    if (checkToken(compiler, NEWLINE))
    {
        emitReturn(compiler);
    }
    else
    {
        expression(compiler);
        emitByte(compiler, OP_FRET);
    }
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
void tryStatement(Compiler *compiler)
{
    size_t indent = getIndent(compiler);
    advanceToken(compiler); // consume `try`
    consumeToken(compiler, COLON, "expected ':' after try");
    int tryJump = emitJump(compiler, OP_TRY);
    compiler->tryDepth++;
    if (matchToken(compiler, NEWLINE))
        block(compiler, indent);
    else
        simpleStatement(compiler);
    compiler->tryDepth--;
    emitByte(compiler, OP_ENDTRY);
    int afterCatch = emitJump(compiler, OP_JMP);
    patchJump(compiler, tryJump);
    // The catch arm. Optional `<name>` binding before the colon.
    if (!matchToken(compiler, CATCH))
    {
        // No `catch` follows — equivalent to `catch _:` (discard
        // the exception). Still need to consume the pushed value
        // so the operand stack stays balanced.
        emitByte(compiler, OP_POP);
        patchJump(compiler, afterCatch);
        return;
    }
    if (matchToken(compiler, NAME))
    {
        u32 nameIdx = identifierConstant(compiler, &compiler->parser->previous);
        emitSetV(compiler, nameIdx);
    }
    emitByte(compiler, OP_POP);
    consumeToken(compiler, COLON, "expected ':' after catch [name]");
    if (matchToken(compiler, NEWLINE))
        block(compiler, indent);
    else
        simpleStatement(compiler);
    patchJump(compiler, afterCatch);
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

                do
                {
                    if (fncompiler->function->argc > 255)
                    {
                        errorAtCurrent(fncompiler, "cannot have more than 255 parameters.");
                    }
                    fnParameters(fncompiler);
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
                    else if (defaults > 0)
                    {
                        errorAt(fncompiler, fncompiler->parser->previous,
                                "a parameter without a default can't follow one with a default.");
                    }
                } while (matchToken(fncompiler, COMMA));
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