#include "expression.h"
#include "statement.h"
#include "bytecode.h"
#include "datatypes/datatypes.h"
#include "error.h"

void integer_(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    long n = strtol(compiler->parser->previous.token, NULL, 10);
    // Fits in 32 bits → emit inline NaN-boxed int. Otherwise fall back to a
    // heap MyMoInt for the wider value (rare; will become a small-bignum
    // slow path in a later step).
    if (n >= INT32_MIN && n <= INT32_MAX)
    {
        emitConstantV(compiler, V_INT_VAL((int32_t)n));
    }
    else
    {
        emitConstant(compiler, NEW_INT(compiler->parser->vm, n));
    }
}

void double_(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    // Doubles are stored directly in the NaN-boxed Value (any non-QNaN
    // bit pattern). Skip the heap MyMoDouble entirely for literals.
    double d = strtod(compiler->parser->previous.token, NULL);
    emitConstantV(compiler, V_DOUBLE_VAL(d));
}

void string_(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    emitConstant(compiler, (NEW_STRING(compiler->parser->vm, compiler->parser->previous.token, compiler->parser->previous.length)));
}

// f-string interpolation. Walks the FSTRING token's content,
// splitting on `{...}` segments. Each literal piece becomes a string
// constant; each `{expr}` re-runs the lexer+parser over the expr text
// and emits OP_TOSTRING. Pieces are concatenated with OP_ADD. The
// sub-parser shares the outer compiler's chunk, so all emit goes to
// the same place — we only have to swap the parser-level lexer+
// lookahead tokens for the duration of each interpolation.
void fstring_(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    const char *content = compiler->parser->previous.token;
    int total = (int)compiler->parser->previous.length;
    MVM *vm = compiler->parser->vm;

    int piece_count = 0;
    int i = 0;
    while (i < total)
    {
        // Scan to the next `{` (or end).
        int seg_start = i;
        while (i < total && content[i] != '{') i++;
        int seg_len = i - seg_start;
        // Emit the literal piece. Always emit even if empty so the
        // first OP_ADD has a left operand; the compiler optimizer
        // can later drop empty pieces.
        if (seg_len > 0 || piece_count == 0)
        {
            emitConstant(compiler, NEW_STRING(vm, content + seg_start, seg_len));
            piece_count++;
            if (piece_count > 1) emitBytes(compiler, OP_ADD, 0);
        }
        if (i >= total) break;
        // At `{`. Find matching `}` with brace depth tracking so
        // `{ {"k":1}["k"] }` (a dict-subscript expression) works.
        // Skip over string literals (single, double, backtick) so
        // braces inside a nested f-string or a literal don't throw
        // off the depth counter. Strings here can't escape `}` —
        // that's what closes us — but they CAN contain `{` for
        // nested f-strings like `f"{f"x={x}"}"`.
        i++; // past `{`
        int expr_start = i;
        int depth = 1;
        while (i < total && depth > 0)
        {
            char c = content[i];
            if (c == '"' || c == '\'' || c == '`')
            {
                char quote = c;
                i++;
                while (i < total && content[i] != quote)
                {
                    if (content[i] == '\\' && i + 1 < total) i++;
                    i++;
                }
                if (i < total) i++; // past closing quote
                continue;
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
            if (depth > 0) i++;
        }
        if (depth != 0)
        {
            errorAtCurrent(compiler, "Unclosed `{` in f-string interpolation.");
            return;
        }
        int expr_len = i - expr_start;
        i++; // past `}`

        // Build a null-terminated buffer for the embedded expression.
        // We append a `\n` so the sub-lexer's NEWLINE check is
        // satisfied at end-of-source.
        char *buf = New(char, expr_len + 2);
        memcpy(buf, content + expr_start, expr_len);
        buf[expr_len] = '\n';
        buf[expr_len + 1] = '\0';

        // Swap parser state, parse the expression, restore.
        Lexer *outerLexer = compiler->parser->lexer;
        Token outerCurrent = compiler->parser->current;
        Token outerPrevious = compiler->parser->previous;
        Lexer *innerLexer = initLexer(buf);
        compiler->parser->lexer = innerLexer;
        compiler->parser->current = getToken(innerLexer);
        expression(compiler);
        freeLexer(innerLexer);
        Free(vm, char, buf);
        compiler->parser->lexer = outerLexer;
        compiler->parser->current = outerCurrent;
        compiler->parser->previous = outerPrevious;

        emitByte(compiler, OP_TOSTRING);
        piece_count++;
        if (piece_count > 1) emitBytes(compiler, OP_ADD, 0);
    }
    if (piece_count == 0)
    {
        // Empty f-string `f""`.
        emitConstant(compiler, NEW_STRING(vm, "", 0));
    }
}
void literal(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    switch (compiler->parser->previous.type)
    {
    case FALSE:
        emitByte(compiler, OP_FALSE);
        break;
    case NIL:
        emitByte(compiler, OP_NIL);
        break;
    case TRUE:
        emitByte(compiler, OP_TRUE);
        break;
    default:
        return; // Unreachable.
    }
}

void unary(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    TokenType operatorType = compiler->parser->previous.type;
    parsePrecedence(compiler, PREC_UNARY);
    switch (operatorType)
    {
    case EXCMARK:
    case NOT:
        emitByte(compiler, OP_NOT);
        break;
    case MINUS:
        emitByte(compiler, OP_NEG);
        break;
    case PLUS:
        emitByte(compiler, OP_POS);
        break;
    default:
        return;
    }
}

void binary(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    TokenType operatorType = compiler->parser->previous.type;
    ParseRule *rule = getRule(operatorType);
    parsePrecedence(compiler, (Precedence)(rule->precedence + 1));
    u32 inplace = 0;
    switch (operatorType)
    {
    case NEQUAL:
        emitBytes(compiler, OP_EQUAL, inplace + 1);
        emitByte(compiler, OP_NOT);
        break;
    case DEQUAL:
        emitBytes(compiler, OP_EQUAL, inplace);
        break;
    case IS:
        // Handled in isOp() below — never emitted here because the
        // parse rule for IS uses a custom infix that peeks `not`
        // BEFORE parsing the RHS (so `is not Nil` doesn't get parsed
        // as `is (not Nil)`).
        break;
    case GREATER:
        emitBytes(compiler, OP_GREATER, inplace);
        break;
    case EGREATER:
        emitBytes(compiler, OP_LESS, inplace + 1);
        emitByte(compiler, OP_NOT);
        break;
    case LESS:
        emitBytes(compiler, OP_LESS, inplace);
        break;
    case ELESS:
        emitBytes(compiler, OP_GREATER, inplace + 1);
        emitByte(compiler, OP_NOT);
        break;
    case PLUS:
        emitBytes(compiler, OP_ADD, inplace);
        break;
    case MINUS:
        emitBytes(compiler, OP_SUB, inplace);
        break;
    case STAR:
        emitBytes(compiler, OP_MUL, inplace);
        break;
    case SLASH:
        emitBytes(compiler, OP_DIV, inplace);
        break;
    case DSTAR:
        emitBytes(compiler, OP_POW, inplace);
        break;
    case DSLASH:
        emitBytes(compiler, OP_IDIV, inplace);
        break;
    case PERCENT:
        emitBytes(compiler, OP_MOD, inplace);
        break;
    case DLESS:
        emitBytes(compiler, OP_LSFT, inplace);
        break;
    case DGREATER:
        emitBytes(compiler, OP_RSFT, inplace);
        break;
    case AMPER:
        emitBytes(compiler, OP_BAND, inplace);
        break;
    case VBAR:
        emitBytes(compiler, OP_BOR, inplace);
        break;
    case CAP:
        emitBytes(compiler, OP_BXOR, inplace);
        break;
    default:
        return; // Unreachable.
    }
}

// IC-aware emitters for the get/set ops inside assign(): when the op is
// OP_GETV/OP_SETV, also reserve the inline-cache scratch bytes. Property
// ops (OP_GETP/OP_SETP/OP_AGETP) keep the legacy 2-byte encoding.
static void emitGetSet(Compiler *c, u8 op, u8 name)
{
    if (op == OP_GETV) emitGetV(c, name);
    else if (op == OP_SETV) emitSetV(c, name);
    else emitBytes(c, op, name);
}

bool assign(Compiler *compiler, bool canAssign, u8 set, u8 get, u8 name)
{
    if (!canAssign)
        return false;
    size_t type = compiler->parser->current.type;
    if (compiler->flags.dontSetVar && type >= EQUAL && type <= EDGREATER)
    {
        errorAtCurrent(compiler, "Assignment is not possible.");
        return false;
    }
    if (matchToken(compiler, EQUAL))
    {
        expression(compiler);
        emitGetSet(compiler, set, name);
        return true;
    }
    u32 inplace = 1;
    if (matchToken(compiler, EPLUS))
    {
        // Peephole: `name += int_literal` -> OP_INCR_VAR.
        int startPos = currentChunk(compiler)->count;
        emitGetSet(compiler, get, name);
        int afterGetv = currentChunk(compiler)->count;
        expression(compiler);
        int afterExpr = currentChunk(compiler)->count;
        Chunk *ch = currentChunk(compiler);
        if (set == OP_SETV
            && afterExpr - afterGetv == 2
            && ch->code[afterGetv] == OP_CONST)
        {
            u8 constIdx = ch->code[afterGetv + 1];
            Value v = ch->constants.values[constIdx];
            if (V_IS_INT(v))
            {
                ch->count = startPos;
                emitIncrVar(compiler, name, V_AS_INT(v));
                return true;
            }
        }
        emitBytes(compiler, OP_ADD, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EMINUS))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_SUB, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, ESTAR))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_MUL, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, ESLASH))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_DIV, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EDSTAR))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_POW, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EDSLASH))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_IDIV, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EPERCENT))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_MOD, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EVBAR))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_BOR, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EAMPER))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_BAND, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, ECAP))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_BXOR, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EDGREATER))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_RSFT, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    else if (matchToken(compiler, EDLESS))
    {
        emitGetSet(compiler, get, name);
        expression(compiler);
        emitBytes(compiler, OP_LSFT, inplace);
        emitGetSet(compiler, set, name);
        return true;
    }
    return false;
}

// Infix `is` (with optional `not`). Peeks the next token for `not`
// before parsing the RHS so that `is not Nil` reads as a single
// identity check, not `is (not Nil)`. The VM's OP_IS handler takes a
// one-byte negation flag and produces the final bool directly.
void isOp(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    int negate = matchToken(compiler, NOT) ? 1 : 0;
    parsePrecedence(compiler, (Precedence)(PREC_EQUALITY + 1));
    emitBytes(compiler, OP_IS, (u32)negate);
}

void dot(Compiler *compiler, bool canAssign)
{
    consumeToken(compiler, NAME, "Expect property name after '.'.");
    u32 name = identifierConstant(compiler, &compiler->parser->previous);
    if (!assign(compiler, canAssign, OP_SETP, OP_AGETP, name))
    {
        emitBytes(compiler, OP_GETP, name);
    }
}

// JS-style optional chaining: `r?.prop`. If the receiver is Nil, the
// whole access short-circuits to Nil; otherwise it behaves like `.`.
// Assignment is intentionally not supported (`r?.x = 1` would be a
// silent no-op when r is Nil, which is usually a bug).
void optDot(Compiler *compiler, bool canAssign)
{
    consumeToken(compiler, NAME, "Expect property name after '?.'.");
    u32 name = identifierConstant(compiler, &compiler->parser->previous);
    emitBytes(compiler, OP_OGETP, name);
}

// Resolve an identifier to a function-arg slot if we're inside a function
// body and the name matches one of the function's declared parameters.
// Returns the slot index (0..argc-1) or -1 if not an arg.
static int resolveArgSlot(Compiler *compiler, Token *t)
{
    MyMoFunction *fn = compiler->function;
    if (fn->type == FN_SCRIPT || fn->type == FN_MODULE) return -1;
    if (fn->argc > CALLFRAME_ARGS_INLINE) return -1;  // spill case stays on dict path
    for (int i = 0; i < fn->argc; i++)
    {
        MyMoString *p = fn->argv[i];
        if (p && p->length == t->length
            && memcmp(p->value, t->token, t->length) == 0)
            return i;
    }
    return -1;
}

void variable(Compiler *compiler, bool canAssign)
{
    // Inside a case-arm pattern:
    //   `_`     -> wildcard (matches anything, no binding)
    //   `_name` -> wildcard + binding (matches anything, binds to `name`
    //              when the arm fires). Constraints:
    //              - Only at the outer-tuple level (depth ≤ 1)
    //              - Up to 16 bindings per arm
    //              - `__init__`, `__name__`, etc. (>=2 leading underscores)
    //                still resolve as variables — Python-style dunders.
    // Function-arg slot fast path: when `n` (or any other parameter
    // name) is referenced inside the function body, emit OP_GETARG/
    // OP_SETARG instead of going through the dict. Cuts ~30 ns per
    // variable read in tight recursive loops like fib.
    {
        int argSlot = resolveArgSlot(compiler, &compiler->parser->previous);
        if (argSlot >= 0)
        {
            // Handle the four common assignment shapes inline. Other
            // compound forms fall through to the read path; covering them
            // is mechanical but not needed for the perf hot path.
            if (canAssign && matchToken(compiler, EQUAL))
            {
                expression(compiler);
                emitBytes(compiler, OP_SETARG, (u8)argSlot);
                return;
            }
            if (canAssign && matchToken(compiler, EPLUS))
            {
                emitBytes(compiler, OP_GETARG, (u8)argSlot);
                expression(compiler);
                emitBytes(compiler, OP_ADD, 1);
                emitBytes(compiler, OP_SETARG, (u8)argSlot);
                return;
            }
            if (canAssign && matchToken(compiler, EMINUS))
            {
                emitBytes(compiler, OP_GETARG, (u8)argSlot);
                expression(compiler);
                emitBytes(compiler, OP_SUB, 1);
                emitBytes(compiler, OP_SETARG, (u8)argSlot);
                return;
            }
            emitBytes(compiler, OP_GETARG, (u8)argSlot);
            return;
        }
    }

    if (compiler->flags.casePattern
        && compiler->parser->previous.length >= 1
        && compiler->parser->previous.token[0] == '_')
    {
        Token *t = &compiler->parser->previous;
        if (t->length == 1)
        {
            emitByte(compiler, OP_WILDCARD);
            return;
        }
        if (t->length >= 2 && t->token[1] != '_'
            && compiler->flags.casePatternDepth <= 4
            && compiler->flags.bindingsCount < 16)
        {
            // Strip the leading `_` to get the binding name.
            Token nameTok = *t;
            nameTok.token = t->token + 1;
            nameTok.length = t->length - 1;
            u8 nameIdx = identifierConstant(compiler, &nameTok);
            int bIdx = compiler->flags.bindingsCount++;
            compiler->flags.bindingNameIdx[bIdx] = nameIdx;
            // Build path from the per-depth position stack. Outermost
            // index first; inside `((_a, _b), _c)` the binding `a` has
            // path [0, 0], `b` has [0, 1], `c` has [1].
            int depth = (int)compiler->flags.casePatternDepth;
            compiler->flags.bindingPathLen[bIdx] = depth;
            for (int d = 0; d < depth && d < 4; d++)
            {
                // The binding lives at the CURRENT position-1 of this depth
                // (we've already incremented for the previous element). For
                // the outermost depth and the innermost, we read the live
                // counter; intermediate depths likewise.
                compiler->flags.bindingPath[bIdx][d] = (int)compiler->flags.casePatternStackPos[d];
            }
            emitByte(compiler, OP_WILDCARD);
            return;
        }
        // fall through: treat `__name`, `_name` at depth>1, or overflow as
        // a regular variable reference (current semantics).
    }
    u8 name = identifierConstant(compiler, &compiler->parser->previous);
    u8 set, get;
    set = OP_SETV;
    get = OP_GETV;
    if (assign(compiler, canAssign, set, get, name))
    {
        // do nothing
    }
    else
    {
        if (compiler->parser->previous.type == NAME && checkToken(compiler, ARROW))
        {
            Compiler *arrowCompiler = initCompiler(compiler->parser->vm, compiler->parser, FN_ARROWFN);
            arrowCompiler->function->name = newString(compiler->parser->vm, "arrowfn", 7);
            arrowCompiler->function->argc = 1;
            MyMoString *arg = newString(compiler->parser->vm, compiler->parser->previous.token, compiler->parser->previous.length);
            advanceToken(compiler);
            arrowCompiler->function->argv[0] = arg;
            size_t indent = getIndent(compiler);
            if (matchToken(arrowCompiler, NEWLINE))
            {
                block(arrowCompiler, indent);
                retreatNewLine(arrowCompiler);
            }
            else
            {
                expression(arrowCompiler);
                emitByte(arrowCompiler, OP_FRET);
            }
            MyMoFunction *arrowFunction = endFunction(arrowCompiler);
            emitBytes(compiler, OP_FN, makeConstant(compiler, AS_OBJECT(arrowFunction)));
            // printf("arrow function");
        }
        else
            emitGetSet(compiler, get, name);
    }
}

void expression(Compiler *compiler)
{
    parsePrecedence(compiler, PREC_ASSIGNMENT);
    if (checkToken(compiler, COMMA) && !compiler->flags.argv && !compiler->flags.dict && !compiler->flags.list && !compiler->flags.tuple && !compiler->flags.multiCase)
    {
        advanceToken(compiler);
        u32 count = 1;
        do
        {
            if (checkToken(compiler, NEWLINE))
            {
                break;
            }
            compiler->flags.dontSetVar++;
            parsePrecedence(compiler, PREC_ASSIGNMENT);
            compiler->flags.dontSetVar--;
            count++;
        } while (matchToken(compiler, COMMA));
        if (count > 1)
        {
            emitBytes(compiler, OP_TUPLE, count);
        }
    }
}

void grouping(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    if (checkArrow(compiler))
    {
        Compiler *arrowCompiler = initCompiler(compiler->parser->vm, compiler->parser, FN_ARROWFN);
        arrowCompiler->function->name = newString(compiler->parser->vm, "arrowfn", 7);
        if (!checkToken(compiler, RPAR))
        {
            do
            {
                advanceToken(arrowCompiler);
                if (arrowCompiler->function->argc == 255)
                {
                    errorAtCurrent(arrowCompiler, "Can't have more than 255 parameters.");
                }
                if (arrowCompiler->parser->previous.type == NAME)
                {
                    arrowCompiler->function->argc++;
                    MyMoString *arg = newString(arrowCompiler->parser->vm, arrowCompiler->parser->previous.token, arrowCompiler->parser->previous.length);
                    arrowCompiler->function->argv[arrowCompiler->function->argc - 1] = arg;
                }
                else
                {
                    error(arrowCompiler, "Expect argument name.");
                }
            } while (matchToken(arrowCompiler, COMMA));
        }
        consumeToken(arrowCompiler, RPAR, "Expect ')' after parameters.");
        size_t indent = getIndent(compiler);
        consumeToken(arrowCompiler, ARROW, "Expect ':' after parameters.");
        if (matchToken(arrowCompiler, NEWLINE))
        {
            block(arrowCompiler, indent);
            retreatNewLine(arrowCompiler);
        }
        else
        {
            expression(arrowCompiler);
            emitByte(arrowCompiler, OP_FRET);
        }
        MyMoFunction *arrowFunction = endFunction(arrowCompiler);
        emitBytes(compiler, OP_FN, makeConstant(compiler, AS_OBJECT(arrowFunction)));
    }
    else
    {
        u32 count = 0;
        compiler->flags.tuple++;
        // Track element position per nesting level so `_name` bindings
        // can record a full subscript path. Max 4 levels of nesting;
        // deeper falls back to wildcard-without-binding.
        bool trackPos = (compiler->flags.casePattern > 0);
        if (trackPos)
        {
            compiler->flags.casePatternDepth++;
            if (compiler->flags.casePatternDepth <= 4)
                compiler->flags.casePatternStackPos[compiler->flags.casePatternDepth - 1] = 0;
        }
        if (!checkToken(compiler, RPAR))
        {
            do
            {
                skipNewLines(compiler);
                if (checkToken(compiler, RPAR))
                {
                    break;
                }
                compiler->flags.dontSetVar++;
                expression(compiler);
                compiler->flags.dontSetVar--;
                skipNewLines(compiler);
                count++;
                if (trackPos && compiler->flags.casePatternDepth >= 1
                    && compiler->flags.casePatternDepth <= 4)
                    compiler->flags.casePatternStackPos[compiler->flags.casePatternDepth - 1]++;
            } while (matchToken(compiler, COMMA));
        }
        if (count == 1)
        {
            consumeToken(compiler, RPAR, "Expect ')' after expression.");
        }
        else
        {
            consumeToken(compiler, RPAR, "Expected closing ')'");
            emitBytes(compiler, OP_TUPLE, count);
        }
        if (trackPos) compiler->flags.casePatternDepth--;
        compiler->flags.tuple--;
    }
}

u8 argumentList(Compiler *compiler)
{
    uint8_t argCount = 0;
    if (!checkToken(compiler, RPAR))
    {
        do
        {
            if (argCount == 255)
                error(compiler, "Can't have more than 255 arguments.");
            expression(compiler);
            argCount++;
        } while (matchToken(compiler, COMMA));
    }
    consumeToken(compiler, RPAR, "Expect ')' after arguments.");
    return argCount;
}

void call(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    Chunk *ch = currentChunk(compiler);
    // Peephole: when the very last emitted bytes are an OP_GETV (10 bytes:
    // opcode + name_idx + 8 IC scratch), rewind it and emit a fused
    // OP_INVOKE_GLOBAL after the args. Saves a dispatch (1 op vs 2) and
    // ~3 bytes of bytecode for every `print(x)` / `len(s)` style call.
    int markBeforeArgs = ch->count;
    bool fuseable = (markBeforeArgs >= 10
                     && ch->code[markBeforeArgs - 10] == OP_GETV);
    u8 fusedNameIdx = 0;
    if (fuseable)
    {
        fusedNameIdx = ch->code[markBeforeArgs - 10 + 1];
        ch->count = markBeforeArgs - 10;  // rewind the OP_GETV
    }

    compiler->flags.argv++;
    u8 argCount = argumentList(compiler);
    if (compiler->flags.pithru)
    {
        argCount++;
    }
    if (fuseable)
    {
        // Layout: opcode | name_idx | IC[8] | argc — 11 bytes total.
        emitByte(compiler, OP_INVOKE_GLOBAL);
        emitByte(compiler, fusedNameIdx);
        emitByte(compiler, IC_TAG_COLD);
        for (int i = 1; i < IC_BYTES; i++) emitByte(compiler, 0);
        emitByte(compiler, argCount);
    }
    else
    {
        emitBytes(compiler, OP_CALL, argCount);
    }
    compiler->flags.argv--;
}

void subScript(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    compiler->flags.dontSetVar++;
    if (matchToken(compiler, COLON))
    {
        emitByte(compiler, OP_NIL);
        if (matchToken(compiler, RSQB))
        {
            emitByte(compiler, OP_NIL);
            emitByte(compiler, OP_NIL);
            emitByte(compiler, OP_SLICE);
            goto endSS;
        }
    step2:
        if (matchToken(compiler, COLON))
        {
            emitByte(compiler, OP_NIL);
        step3:
            if (matchToken(compiler, RSQB))
            {
                emitByte(compiler, OP_NIL);
                emitByte(compiler, OP_SLICE);
                goto endSS;
            }
            else
            {
                expression(compiler);
                consumeToken(compiler, RSQB, "Expect ']' after expression.");
                emitByte(compiler, OP_SLICE);
                goto endSS;
            }
        }
        else
        {
            expression(compiler);
            if (matchToken(compiler, COLON))
                goto step3;
        }
        if (matchToken(compiler, RSQB))
        {
            emitByte(compiler, OP_NIL);
            emitByte(compiler, OP_SLICE);
            goto endSS;
        }
        consumeToken(compiler, RSQB, "Expect ']' after expression.");
    }
    else
    {
        expression(compiler);
        if (matchToken(compiler, RSQB))
        {
            if (matchToken(compiler, EQUAL))
            {
                expression(compiler);
                emitByte(compiler, OP_SETSUBSCR);
            }
            else
            {
                emitByte(compiler, OP_SUBSCR);
            }
            goto endSS;
        }
        if (matchToken(compiler, COLON))
        {
            if (matchToken(compiler, RSQB))
            {
                emitByte(compiler, OP_NIL);
                emitByte(compiler, OP_NIL);
                emitByte(compiler, OP_SLICE);
                goto endSS;
            }
            if (matchToken(compiler, RSQB))
            {
                emitByte(compiler, OP_NIL);
                emitByte(compiler, OP_NIL);
                emitByte(compiler, OP_SLICE);
                goto endSS;
            }
            goto step2;
        }
        consumeToken(compiler, RSQB, "Expect ']' after expression.");
    }
endSS:
    compiler->flags.dontSetVar--;
    return;
}

// Forward declaration — defined below.
static void listComprehension(Compiler *compiler, const char *exprText, int exprLen);

void list(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    compiler->flags.dontSetVar++;
    u32 count = 0;
    compiler->flags.list++;
    // Same position tracking as grouping() — list patterns `[a, b, c]`
    // also bind by index in case-arm context.
    bool trackPos = (compiler->flags.casePattern > 0);
    if (trackPos)
    {
        compiler->flags.casePatternDepth++;
        if (compiler->flags.casePatternDepth <= 4)
            compiler->flags.casePatternStackPos[compiler->flags.casePatternDepth - 1] = 0;
    }
    if (!checkToken(compiler, RSQB))
    {
        skipNewLines(compiler);
        // Capture the source text of the first expression so we can
        // re-emit it inside the comprehension's loop body. Token
        // pointers reference lexer->src directly; the slice is
        // (start, end_of_last_token).
        const char *exprStart = compiler->parser->current.token;
        int savedChunkCount = compiler->function->chunk->count;
        expression(compiler);
        // Use the START of the NEXT token as the end-of-expr cursor.
        // Going through parser->previous.token + length would
        // truncate when the expression ends in a string token —
        // STRING tokens point at the content (between quotes), so
        // token+length lands AT the closing quote, not after it,
        // and we'd lose the closing `"`.
        const char *exprEnd = compiler->parser->current.token;
        skipNewLines(compiler);
        if (checkToken(compiler, FOR))
        {
            // Discard the bytecode we just emitted for the first
            // expression — it'll be re-emitted inside the loop body
            // via a sub-parser. The constants it added remain in
            // the pool but they're harmless (just unreachable).
            compiler->function->chunk->count = savedChunkCount;
            int exprLen = (int)(exprEnd - exprStart);
            listComprehension(compiler, exprStart, exprLen);
            if (trackPos) compiler->flags.casePatternDepth--;
            compiler->flags.list--;
            compiler->flags.dontSetVar--;
            return;
        }
        count++;
        if (trackPos && compiler->flags.casePatternDepth >= 1
            && compiler->flags.casePatternDepth <= 4)
            compiler->flags.casePatternStackPos[compiler->flags.casePatternDepth - 1]++;
        while (matchToken(compiler, COMMA))
        {
            skipNewLines(compiler);
            if (checkToken(compiler, RSQB))
            {
                break;
            }
            expression(compiler);
            skipNewLines(compiler);
            count++;
            if (trackPos && compiler->flags.casePatternDepth >= 1
                && compiler->flags.casePatternDepth <= 4)
                compiler->flags.casePatternStackPos[compiler->flags.casePatternDepth - 1]++;
        }
    }
    consumeToken(compiler, RSQB, "Expected closing ']'");
    emitBytes(compiler, OP_LIST, count);
    if (trackPos) compiler->flags.casePatternDepth--;
    compiler->flags.list--;
    compiler->flags.dontSetVar--;
}

// List comprehension: `[EXPR for VAR in ITER (if COND)?]`.
// Already at the `for` token. Caller has captured EXPR's source text
// for re-emission and rewound chunk->count. Builds bytecode for:
//
//   __lc_result = []
//   for VAR in ITER:
//       if COND:
//           __lc_result.append(EXPR)
//   __lc_result  // leaves on stack as the expression's value
//
// VAR is a real local (uses whatever slot logic OP_SETV resolves);
// __lc_result is a hidden local with a name outside the identifier
// alphabet so it can never collide with user code.
static void listComprehension(Compiler *compiler, const char *exprText, int exprLen)
{
    MVM *vm = compiler->parser->vm;
    // Hidden-local name for the accumulator. `<` is not an identifier
    // start char in MyMo, so this can never collide with user code.
    // Nested comprehensions need distinct names, so we suffix with a
    // monotonically increasing counter.
    static int lc_counter = 0;
    char lcBuf[32];
    int lcLen = snprintf(lcBuf, sizeof(lcBuf), "<lc_result_%d>", lc_counter++);
    Token lcTok;
    lcTok.token = lcBuf;
    lcTok.length = lcLen;
    u32 lcName = identifierConstant(compiler, &lcTok);

    // result = []
    emitBytes(compiler, OP_LIST, 0);
    emitSetV(compiler, lcName);
    emitByte(compiler, OP_POP);

    // Consume `for VAR in`.
    consumeToken(compiler, FOR, "expected 'for' in list comprehension");
    consumeToken(compiler, NAME, "expected an iterator name");
    u32 varName = identifierConstant(compiler, &compiler->parser->previous);
    consumeToken(compiler, IN, "expected 'in' after iterator name");

    // Iterable expression. Use a precedence above PREC_ASSIGNMENT
    // so a trailing `if cond` (the filter clause) is left for us
    // to consume — otherwise IF's ternary infix rule would try to
    // parse `iter if cond else <expected>` and demand an `else`.
    parsePrecedence(compiler, PREC_OR);
    emitByte(compiler, OP_GETI);
    Loop loop;
    startLoop(compiler, &loop);
    compiler->loop->loopJump = emitJump(compiler, OP_ITER);
    emitSetV(compiler, varName);
    emitByte(compiler, OP_POP);

    // Optional `if COND` filter.
    int condJump = -1;
    if (matchToken(compiler, IF))
    {
        expression(compiler);
        condJump = emitJump(compiler, OP_JIF);
        emitByte(compiler, OP_POP); // pop the condition's True
    }

    // result.append(EXPR) — OP_GETP now allocates a fresh bound
    // method per lookup, so nested comprehensions no longer alias
    // the same `self`.
    emitGetV(compiler, lcName);
    Token appendTok;
    appendTok.token = "append";
    appendTok.length = 6;
    u32 appendName = identifierConstant(compiler, &appendTok);
    emitBytes(compiler, OP_GETP, appendName);

    // Sub-parse the saved expression text — same swap-and-restore
    // pattern as f-string interpolations. The new lexer reads from
    // a heap buffer terminated by `\n` so it produces a clean
    // NEWLINE after the expression.
    char *buf = New(char, exprLen + 2);
    memcpy(buf, exprText, exprLen);
    buf[exprLen] = '\n';
    buf[exprLen + 1] = '\0';
    Lexer *outerLexer = compiler->parser->lexer;
    Token outerCurrent = compiler->parser->current;
    Token outerPrevious = compiler->parser->previous;
    Lexer *innerLexer = initLexer(buf);
    compiler->parser->lexer = innerLexer;
    compiler->parser->current = getToken(innerLexer);
    expression(compiler);
    freeLexer(innerLexer);
    Free(vm, char, buf);
    compiler->parser->lexer = outerLexer;
    compiler->parser->current = outerCurrent;
    compiler->parser->previous = outerPrevious;

    emitBytes(compiler, OP_CALL, 1);
    emitByte(compiler, OP_POP); // discard append's return value

    if (condJump >= 0)
    {
        int afterAppend = emitJump(compiler, OP_JMP);
        patchJump(compiler, condJump);
        emitByte(compiler, OP_POP); // pop the condition's False
        patchJump(compiler, afterAppend);
    }

    emitLoop(compiler, compiler->loop->loopStart);
    int Jump = emitJump(compiler, OP_JMP);
    compiler->loop = loop.enclosing;
    patchJump(compiler, loop.loopJump);
    emitByte(compiler, OP_POP); // pop the iterator
    patchJump(compiler, Jump);
    int breaksCount = loop.breaksCount;
    while (breaksCount)
    {
        breaksCount--;
        patchJump(compiler, loop.breakJumps[breaksCount]);
    }
    if (loop.breaksCapacity)
    {
        FreeArray(compiler->parser->vm, int, loop.breakJumps, loop.breaksCapacity);
    }

    // Push the accumulated result as the expression's value.
    emitGetV(compiler, lcName);
    consumeToken(compiler, RSQB, "Expected closing ']' after comprehension");
}

void dictionary(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    compiler->flags.dontSetVar++;
    u32 count = 0;
    if (!checkToken(compiler, RBRACE))
    {
        do
        {
            skipNewLines(compiler);
            if (checkToken(compiler, RBRACE))
            {
                break;
            }
            expression(compiler);
            skipNewLines(compiler);
            consumeToken(compiler, COLON, "Expected ':'");
            skipNewLines(compiler);
            compiler->flags.dict++;
            expression(compiler);
            compiler->flags.dict--;
            skipNewLines(compiler);
            count++;
        } while (matchToken(compiler, COMMA));
    }
    consumeToken(compiler, RBRACE, "Expected closing '}'");
    emitBytes(compiler, OP_DICT, count);
    compiler->flags.dontSetVar--;
}

void pipeThrough(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    compiler->flags.pithru++;
    if (matchToken(compiler, NAME))
    {
        u32 name = identifierConstant(compiler, &compiler->parser->previous);
        emitGetV(compiler, name);
        if (matchToken(compiler, DOT))
        {
            consumeToken(compiler, NAME, "Expected property name after '.'.");
            u32 name = identifierConstant(compiler, &compiler->parser->previous);
            emitBytes(compiler, OP_GETP, name);
        }
        emitByte(compiler, OP_PITHRU);
        if (matchToken(compiler, LPAR))
        {
            call(compiler, canAssign);
        }
        else
        {
            emitByte(compiler, OP_CALL);
            emitByte(compiler, 1);
        }
    }
    else
    {
        errorAtCurrent(compiler, "Cannot pipe through a literal");
    }
    compiler->flags.pithru--;
}

void yeild(Compiler *compiler, bool canAssign)
{
    UNUSED(canAssign);
    if (compiler->flags.compileType == COMPILE_FUNCTION)
    {
        emitGetV(compiler, identifierConstant(compiler, &compiler->parser->previous));
        compiler->function->type = (compiler->function->type == FN_METHOD ? FN_GEN_METHOD : FN_GENERATOR);
    }
    else
    {
        errorAt(compiler, compiler->parser->previous, "Cannot yeild outside of a function");
    }
}