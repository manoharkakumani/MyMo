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
void emitGetV(Compiler *compiler, u8 nameIdx);
void emitSetV(Compiler *compiler, u8 nameIdx);

// Super-instruction emit: name += delta (delta is i32, can be negative).
// Layout: opcode (1) + name_idx (1) + IC (8) + delta (4) = 14 bytes.
// Replaces 25 bytes of GETV+CONST+ADD+SETV+POP for the common
// `i += literal` pattern. Eliminates 4 dispatches per increment.
void emitIncrVar(Compiler *compiler, u8 nameIdx, int32_t delta);
void emitByte(Compiler *compiler, u8 byte);
void emitBytes(Compiler *compiler, u8 byte1, u8 byte2);
void patchJump(Compiler *compiler, int offset);
int emitJump(Compiler *compiler, u8 instruction);
void emitLoop(Compiler *compiler, int loopStart);
void emitReturn(Compiler *compiler);

#endif