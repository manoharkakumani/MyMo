
#include "bytecode.h"
#include "error.h"
#include "datatypes/datatypes.h"
#include "vm.h"   // full MVM struct for currentModule check

uint makeConstant(Compiler *compiler, MyMoObject *value)
{
    int constant = addConstant(compiler->parser->vm, currentChunk(compiler), value);
    // Constant operands are one byte wide; a larger index would silently
    // wrap and read the wrong constant.
    if (constant > UINT16_MAX)
    {
        error(compiler, "Too many distinct constants in one function (max 65536); split it into smaller functions or modules.");
        return 0;
    }
    return (uint)constant;
}

uint makeConstantV(Compiler *compiler, Value v)
{
    int constant = addConstantV(compiler->parser->vm, currentChunk(compiler), v);
    // Constant operands are one byte wide; a larger index would silently
    // wrap and read the wrong constant.
    if (constant > UINT16_MAX)
    {
        error(compiler, "Too many distinct constants in one function (max 65536); split it into smaller functions or modules.");
        return 0;
    }
    return (uint)constant;
}

void emitConstOp2(Compiler *compiler, u8 op, uint first, uint second)
{
    if (first > UINT8_MAX || second > UINT8_MAX)
    {
        emitByte(compiler, OP_WIDE);
        emitByte(compiler, (u8)(first >> 8));
        emitByte(compiler, (u8)(second >> 8));
        compiler->lastWideTarget = currentChunk(compiler)->count;
    }
    emitByte(compiler, op);
    emitByte(compiler, (u8)(first & 0xff));
    if (op == OP_MET)
        emitByte(compiler, (u8)(second & 0xff));
}

void emitConstOp(Compiler *compiler, u8 op, uint index)
{
    emitConstOp2(compiler, op, index, 0);
}

uint identifierConstant(Compiler *compiler, Token *name)
{
    return makeConstant(compiler, NEW_STRING(compiler->parser->vm, name->token, name->length));
}

void emitConstant(Compiler *compiler, MyMoObject *value)
{
    emitConstOp(compiler, OP_CONST, makeConstant(compiler, value));
}

void emitConstantV(Compiler *compiler, Value v)
{
    emitConstOp(compiler, OP_CONST, makeConstantV(compiler, v));
}

static void emitNameOpWithIC(Compiler *compiler, u8 op, uint nameIdx)
{
    emitConstOp(compiler, op, nameIdx);
    // Reserve IC slots, all zero-initialized so dict_tag starts cold (0).
    // Cold tag is 0xff, but for a fresh emit any tag value works because the
    // version check immediately sees modifyCount > 0 mismatch on first hit.
    // We initialize to 0xff explicitly to be safe.
    emitByte(compiler, IC_TAG_COLD);
    for (int i = 1; i < IC_BYTES; i++) emitByte(compiler, 0);
}

// SCOPE_GLOBAL / SCOPE_NONLOCAL if the function declared the name, else 0.
static int nameScope(Compiler *compiler, uint nameIdx)
{
    if (compiler->scopeCount == 0)
        return 0;
    Value v = currentChunk(compiler)->constants.values[nameIdx];
    MyMoString *name = AS_STRING(V_AS_OBJ(v));
    for (int i = 0; i < compiler->scopeCount; i++)
        if (compiler->scopeNames[i] == name)
            return compiler->scopeKinds[i];
    return 0;
}

void emitGetV(Compiler *compiler, uint nameIdx)
{
    if (nameScope(compiler, nameIdx) == SCOPE_GLOBAL)
        emitConstOp(compiler, OP_GETG, nameIdx);
    else
        emitNameOpWithIC(compiler, OP_GETV, nameIdx); // nonlocal reads find the enclosing variable
}

void emitSetV(Compiler *compiler, uint nameIdx)
{
    int scope = nameScope(compiler, nameIdx);
    if (scope == SCOPE_GLOBAL)
        emitConstOp(compiler, OP_SETG, nameIdx);
    else if (scope == SCOPE_NONLOCAL)
        emitConstOp(compiler, OP_SETNL, nameIdx);
    else
        emitNameOpWithIC(compiler, OP_SETV, nameIdx);
}

void emitIncrVar(Compiler *compiler, uint nameIdx, int32_t delta)
{
    if (nameScope(compiler, nameIdx))
    {
        // No super-instruction for global/nonlocal names: name = name + delta.
        emitGetV(compiler, nameIdx);
        emitConstantV(compiler, V_INT_VAL(delta));
        emitBytes(compiler, OP_ADD, 1);
        emitSetV(compiler, nameIdx); // leaves the value, like OP_INCR_VAR
        return;
    }
    emitConstOp(compiler, OP_INCR_VAR, nameIdx);
    // 8 IC scratch bytes (same layout as OP_GETV/OP_SETV — first byte cold).
    emitByte(compiler, IC_TAG_COLD);
    for (int i = 1; i < IC_BYTES; i++) emitByte(compiler, 0);
    // 4 bytes little-endian delta.
    emitByte(compiler, (u8)(delta & 0xff));
    emitByte(compiler, (u8)((delta >> 8) & 0xff));
    emitByte(compiler, (u8)((delta >> 16) & 0xff));
    emitByte(compiler, (u8)((delta >> 24) & 0xff));
}
void emitByte(Compiler *compiler, u8 byte)
{
    writeChunk(compiler->parser->vm, currentChunk(compiler), byte, compiler->parser->previous.line, compiler->parser->previous.col);
}

void emitBytes(Compiler *compiler, u8 byte1, u8 byte2)
{
    emitByte(compiler, byte1);
    emitByte(compiler, byte2);
}

void patchJump(Compiler *compiler, int offset)
{
    int jump = currentChunk(compiler)->count - offset - 2;
    if (jump > UINT16_MAX)
    {
        error(compiler, "Too much code to jump over.");
    }
    currentChunk(compiler)->code[offset] = (jump >> 8) & 0xff;
    currentChunk(compiler)->code[offset + 1] = jump & 0xff;
}

int emitJump(Compiler *compiler, u8 instruction)
{
    emitByte(compiler, instruction);
    emitByte(compiler, 0xff);
    emitByte(compiler, 0xff);
    return currentChunk(compiler)->count - 2;
}

void emitLoop(Compiler *compiler, int loopStart)
{
    emitByte(compiler, OP_LOOP);
    int offset = currentChunk(compiler)->count - loopStart + 2;
    if (offset > UINT16_MAX)
        error(compiler, "Loop body too large.");
    emitByte(compiler, (offset >> 8) & 0xff);
    emitByte(compiler, offset & 0xff);
}

void emitReturn(Compiler *compiler)
{
    switch (compiler->function->type)
    {
    case FN_SCRIPT:
    case FN_MODULE:
        emitByte(compiler, OP_RET);
        break;
    default:
        emitByte(compiler, OP_NIL);
        emitByte(compiler, OP_FRET);
        break;
    }
}