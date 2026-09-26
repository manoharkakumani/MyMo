#ifndef __BYTECODE_H__
#define __BYTECODE_H__

#include "compiler.h"
#include "value.h"

uint makeConstant(Compiler *compiler, MyMoObject *value);
uint makeConstantV(Compiler *compiler, Value v);
uint identifierConstant(Compiler *compiler, Token *name);

void emitConstant(Compiler *compiler, MyMoObject *value);
void emitConstantV(Compiler *compiler, Value v);

// Variable get/set with reserved inline-cache bytes. Layout per call:
//   opcode (1) | name_constant_idx (1) | IC scratch (8 bytes, all zero) = 10
// VM-side cache state: (dict_tag u8 | reserved u8 | entry_idx u16 | modifyCount u32).
// dict_tag = 0xff means cold; 0 = vm->globals; 1 = frame->locals.
#define IC_BYTES 8
#define IC_TAG_COLD    0xff
#define IC_TAG_GLOBALS 0
#define IC_TAG_LOCALS  1
void emitGetV(Compiler *compiler, uint nameIdx);
void emitSetV(Compiler *compiler, uint nameIdx);

// Emit `op` followed by one (or, for emitConstOp2, two) constant-pool
// operands. Indices above 255 get an OP_WIDE prefix carrying their high
// bytes, so a function can use up to 65536 constants.
void emitConstOp(Compiler *compiler, u8 op, uint index);
void emitConstOp2(Compiler *compiler, u8 op, uint first, uint second);

// Super-instruction emit: name += delta (delta is i32, can be negative).
// Layout: opcode (1) + name_idx (1) + IC (8) + delta (4) = 14 bytes.
// Replaces 25 bytes of GETV+CONST+ADD+SETV+POP for the common
// `i += literal` pattern. Eliminates 4 dispatches per increment.
void emitIncrVar(Compiler *compiler, uint nameIdx, int32_t delta);
void emitByte(Compiler *compiler, u8 byte);
void emitBytes(Compiler *compiler, u8 byte1, u8 byte2);
void patchJump(Compiler *compiler, int offset);
int emitJump(Compiler *compiler, u8 instruction);
void emitLoop(Compiler *compiler, int loopStart);
void emitReturn(Compiler *compiler);

#endif