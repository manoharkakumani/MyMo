#include "common.h"
#include "debug.h"
#include "value.h"

static int constantInstruction(const char *name, Chunk *chunk, int offset)
{
    u8 constant = chunk->code[offset + 1];
    printf("%-16s %4d ", name, constant);
    // printObject(chunk->constants.objects[constant]);
    printf("\n");
    return offset + 2;
}
static int simpleInstruction(const char *name, int offset)
{
    printf("%s\n", name);
    return offset + 1;
}
static int byteInstruction(const char *name, Chunk *chunk, int offset)
{
    u8 slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}
static int jumpInstruction(const char *name, int sign, Chunk *chunk, int offset)
{
    u16 jump = (u16)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

int disassembleInstruction(Chunk *chunk, int offset)
{
    printf("%04d ", offset);
    printf(" %d:%d\t", chunk->lines[offset], chunk->cols[offset]);
    uint8_t instruction = chunk->code[offset];
    switch (instruction)
    {
    case OP_NOP:
        return simpleInstruction("OP_NOP", offset);
    case OP_CONST:
        return constantInstruction("OP_CONST", chunk, offset);
    case OP_NIL:
        return simpleInstruction("OP_NIL", offset);
    case OP_TRUE:
        return simpleInstruction("OP_TRUE", offset);
    case OP_FALSE:
        return simpleInstruction("OP_FALSE", offset);
    case OP_LIST:
        return byteInstruction("OP_LIST", chunk, offset);
    case OP_TUPLE:
        return byteInstruction("OP_TUPLE", chunk, offset);
    case OP_DICT:
        return byteInstruction("OP_DICT", chunk, offset);
    case OP_SUBSCR:
        return byteInstruction("OP_SUBSCR", chunk, offset);
    case OP_SETSUBSCR:
        return byteInstruction("OP_SETSUBSCR", chunk, offset);
    case OP_CALLKW:
    {
        u8 argc = chunk->code[offset + 1], kwc = chunk->code[offset + 2];
        printf("%-16s %4d (%d keyword)\n", "OP_CALLKW", argc, kwc);
        return offset + 3 + 2 * kwc;
    }
    case OP_SUBSCRK:
        return simpleInstruction("OP_SUBSCRK", offset);
    case OP_DELSUBSCR:
        return simpleInstruction("OP_DELSUBSCR", offset);
    case OP_GETG:
        return constantInstruction("OP_GETG", chunk, offset);
    case OP_SETG:
        return constantInstruction("OP_SETG", chunk, offset);
    case OP_SETNL:
        return constantInstruction("OP_SETNL", chunk, offset);
    case OP_LEXTEND:
        return simpleInstruction("OP_LEXTEND", offset);
    case OP_DADD:
        return simpleInstruction("OP_DADD", offset);
    case OP_DMERGE:
        return simpleInstruction("OP_DMERGE", offset);
    case OP_CALLEX:
        return simpleInstruction("OP_CALLEX", offset);
    case OP_EXCMATCH:
        return simpleInstruction("OP_EXCMATCH", offset);
    case OP_RERAISE:
        return simpleInstruction("OP_RERAISE", offset);
    case OP_FORMAT:
        return byteInstruction("OP_FORMAT", chunk, offset);
    case OP_SET:
        return byteInstruction("OP_SET", chunk, offset);
    case OP_UNPACK:
        return byteInstruction("OP_UNPACK", chunk, offset);
    case OP_SLICE:
        return simpleInstruction("OP_SLICE", offset);
    case OP_DUP:
        return simpleInstruction("OP_DUP", offset);
    case OP_POP:
        return simpleInstruction("OP_POP", offset);
    case OP_GETV:
    case OP_SETV:
    {
        // OP + name_idx + 8 IC scratch bytes = 10 bytes total.
        u8 constant = chunk->code[offset + 1];
        printf("%-16s %4d\n", instruction == OP_GETV ? "OP_GETV" : "OP_SETV", constant);
        return offset + 2 + 8;
    }
    case OP_DELV:
        return constantInstruction("OP_DELV", chunk, offset);
    case OP_GETP:
        return constantInstruction("OP_GETP", chunk, offset);
    case OP_AGETP:
        return constantInstruction("OP_AGETP", chunk, offset);
    case OP_SETP:
        return constantInstruction("OP_SETP", chunk, offset);
    case OP_DELP:
        return constantInstruction("OP_DELP", chunk, offset);
    case OP_JMP:
        return jumpInstruction("OP_JMP", 1, chunk, offset);
    case OP_JIF:
        return jumpInstruction("OP_JIF", 1, chunk, offset);
    case OP_CJMP:
        return jumpInstruction("OP_CJMP", 1, chunk, offset);
    case OP_MCASE:
        return byteInstruction("OP_MCASE", chunk, offset);
    case OP_LOOP:
        return jumpInstruction("OP_LOOP", -1, chunk, offset);
    case OP_ITER:
        return jumpInstruction("OP_ITER", 1, chunk, offset);
    case OP_GETI:
        return simpleInstruction("OP_GETI", offset);
    case OP_EQUAL:
        return byteInstruction("OP_EQUAL", chunk, offset); // operand: in-place flag
    case OP_GREATER:
        return byteInstruction("OP_GREATER", chunk, offset); // operand: in-place flag
    case OP_LESS:
        return byteInstruction("OP_LESS", chunk, offset); // operand: in-place flag
    case OP_NOT:
        return simpleInstruction("OP_NOT", offset);
    case OP_ADD:
        return byteInstruction("OP_ADD", chunk, offset); // operand: in-place flag
    case OP_SUB:
        return byteInstruction("OP_SUB", chunk, offset); // operand: in-place flag
    case OP_MUL:
        return byteInstruction("OP_MUL", chunk, offset); // operand: in-place flag
    case OP_DIV:
        return byteInstruction("OP_DIV", chunk, offset); // operand: in-place flag
    case OP_IDIV:
        return byteInstruction("OP_IDIV", chunk, offset); // operand: in-place flag
    case OP_POW:
        return byteInstruction("OP_POW", chunk, offset); // operand: in-place flag
    case OP_MOD:
        return byteInstruction("OP_MOD", chunk, offset); // operand: in-place flag
    case OP_LSFT:
        return byteInstruction("OP_LSFT", chunk, offset); // operand: in-place flag
    case OP_RSFT:
        return byteInstruction("OP_RSFT", chunk, offset); // operand: in-place flag
    case OP_BAND:
        return byteInstruction("OP_BAND", chunk, offset); // operand: in-place flag
    case OP_BOR:
        return byteInstruction("OP_BOR", chunk, offset); // operand: in-place flag
    case OP_BXOR:
        return byteInstruction("OP_BXOR", chunk, offset); // operand: in-place flag
    case OP_NEG:
        return simpleInstruction("OP_NEG", offset);
    case OP_POS:
        return simpleInstruction("OP_POS", offset);
    case OP_FN:
    {
        offset++;
        u8 constant = chunk->code[offset++];
        printf("%-16s %4d ", "OP_FN", constant);
        printValue(chunk->constants.values[constant]);
        printf("\n");
        return offset;
    }
    case OP_MET:
    {
        return constantInstruction("OP_MET", chunk, offset);
    }
    case OP_CALL:
        return byteInstruction("OP_CALL", chunk, offset);
    case OP_PITHRU:
        return simpleInstruction("OP_PITHRU", offset);
    case OP_RET:
        return simpleInstruction("OP_RET", offset);
    case OP_FRET:
        return simpleInstruction("OP_FRET", offset);
    case OP_CLASS:
        return constantInstruction("OP_CLASS", chunk, offset);
    case OP_SUPERARGS:
        return simpleInstruction("OP_SUPERARGS", offset);
    case OP_ENDCLASS:
        return simpleInstruction("OP_ENDCLASS", offset);
    case OP_USE:
        return constantInstruction("OP_USE", chunk, offset);
    case OP_SETM:
        return simpleInstruction("OP_SETM", offset);
    case OP_COPY:
        return simpleInstruction("OP_COPY", offset);
    case OP_INCR_VAR:
    {
        u8 nameIdx = chunk->code[offset + 1];
        int32_t delta = (int32_t)((u32)chunk->code[offset + 10]
                       | ((u32)chunk->code[offset + 11] << 8)
                       | ((u32)chunk->code[offset + 12] << 16)
                       | ((u32)chunk->code[offset + 13] << 24));
        printf("%-16s %4d %+d\n", "OP_INCR_VAR", nameIdx, delta);
        return offset + 14;  // op + name + 8 IC + 4 delta
    }
    case OP_WILDCARD:
        return simpleInstruction("OP_WILDCARD", offset);
    case OP_GETARG:
        return byteInstruction("OP_GETARG", chunk, offset);
    case OP_SETARG:
        return byteInstruction("OP_SETARG", chunk, offset);
    case OP_INVOKE_GLOBAL:
    {
        u8 nameIdx = chunk->code[offset + 1];
        u8 argc = chunk->code[offset + 10];
        printf("%-16s %4d argc=%d\n", "OP_INVOKE_GLOBAL", nameIdx, argc);
        return offset + 11;  // op + name + 8 IC + argc
    }
    case OP_OGETP:
        return constantInstruction("OP_OGETP", chunk, offset);
    case OP_IS:
        return byteInstruction("OP_IS", chunk, offset);
    case OP_TOSTRING:
        return simpleInstruction("OP_TOSTRING", offset);
    case OP_LAPPEND:
        return simpleInstruction("OP_LAPPEND", offset);
    case OP_TRY:
        return jumpInstruction("OP_TRY", 1, chunk, offset);
    case OP_ENDTRY:
        return simpleInstruction("OP_ENDTRY", offset);
    case OP_RAISE:
        return simpleInstruction("OP_RAISE", offset);
    case OP_WIDE:
        // High bytes for the next instruction's constant operands; its
        // printed operand shows only the low byte.
        printf("%-16s hi=%d,%d\n", "OP_WIDE", chunk->code[offset + 1], chunk->code[offset + 2]);
        return offset + 3;
    case OP_DEFAULTS:
        return byteInstruction("OP_DEFAULTS", chunk, offset);
    case OP_METV:
        return constantInstruction("OP_METV", chunk, offset);
    case OP_IN:
        return simpleInstruction("OP_IN", offset);
    default:
        printf("Unknown opcode %d\n", instruction);
        return offset + 1;
    }
}

void debugChunk(MyMoFunction *function)
{
    ValueArray constants = function->chunk->constants;
    for (int i = 0; i < constants.count; i++)
    {
        Value v = constants.values[i];
        if (!V_IS_OBJ(v) || !IS_FUNCTION(V_AS_OBJ(v)))
        {
            continue;
        }
        debugChunk(AS_FUNCTION(V_AS_OBJ(v)));
    }
    printf("======== %s =========\n", function->name->value);
    printChunk(function->chunk);
}