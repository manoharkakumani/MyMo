
#include "bytecode.h"
#include "error.h"
#include "datatypes/datatypes.h"
#include "vm.h"   // full MVM struct for currentModule check

uint makeConstant(Compiler *compiler, MyMoObject *value)
{
    int constant = addConstant(compiler->parser->vm, currentChunk(compiler), value);
    if (constant > UINT16_MAX)
    {
        error(compiler, "Too many constants in one chunk.");
        return 0;
    }
    return (uint)constant;
}

uint makeConstantV(Compiler *compiler, Value v)
{
    int constant = addConstantV(compiler->parser->vm, currentChunk(compiler), v);
    if (constant > UINT16_MAX)
    {
        error(compiler, "Too many constants in one chunk.");
        return 0;
    }
    return (uint)constant;
}

uint identifierConstant(Compiler *compiler, Token *name)
{
    return makeConstant(compiler, NEW_STRING(compiler->parser->vm, name->token, name->length));
}

void emitConstant(Compiler *compiler, MyMoObject *value)
{
    emitBytes(compiler, OP_CONST, makeConstant(compiler, value));
}

void emitConstantV(Compiler *compiler, Value v)
{
    emitBytes(compiler, OP_CONST, makeConstantV(compiler, v));
}

static void emitNameOpWithIC(Compiler *compiler, u8 op, u8 nameIdx)
{
    emitBytes(compiler, op, nameIdx);
    // Reserve IC slots, all zero-initialized so dict_tag starts cold (0).
    // Cold tag is 0xff, but for a fresh emit any tag value works because the
    // version check immediately sees modifyCount > 0 mismatch on first hit.
    // We initialize to 0xff explicitly to be safe.
    emitByte(compiler, IC_TAG_COLD);
    for (int i = 1; i < IC_BYTES; i++) emitByte(compiler, 0);
}

void emitGetV(Compiler *compiler, u8 nameIdx)
{
    emitNameOpWithIC(compiler, OP_GETV, nameIdx);
}

void emitSetV(Compiler *compiler, u8 nameIdx)
{
    emitNameOpWithIC(compiler, OP_SETV, nameIdx);
}

void emitIncrVar(Compiler *compiler, u8 nameIdx, int32_t delta)
{
    emitBytes(compiler, OP_INCR_VAR, nameIdx);
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