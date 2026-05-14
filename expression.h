#ifndef __EXPRESSION_H__
#define __EXPRESSION_H__

#include "compiler.h"

void integer_(Compiler *compiler, bool canAssign);
void double_(Compiler *compiler, bool canAssign);
void string_(Compiler *compiler, bool canAssign);
void fstring_(Compiler *compiler, bool canAssign);
void unary(Compiler *compiler, bool canAssign);
void binary(Compiler *compiler, bool canAssign);
void grouping(Compiler *compiler, bool canAssign);
void literal(Compiler *compiler, bool canAssign);
void variable(Compiler *compiler, bool canAssign);
void dot(Compiler *compiler, bool canAssign);
void optDot(Compiler *compiler, bool canAssign);
void isOp(Compiler *compiler, bool canAssign);
void expression(Compiler *compiler);
void call(Compiler *compiler, bool canAssign);
void subScript(Compiler *compiler, bool canAssign);
void list(Compiler *compiler, bool canAssign);
void dictionary(Compiler *compiler, bool canAssign);
void pipeThrough(Compiler *compiler, bool canAssign);
void yeild(Compiler *compiler, bool canAssign);

#endif
