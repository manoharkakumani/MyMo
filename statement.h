#ifndef __STATEMENT_H__
#define __STATEMENT_H__

#include "compiler.h"

void statements(Compiler *compiler);
void statement(Compiler *compiler);
void simpleStatement(Compiler *compiler);
void compoundStatement(Compiler *compiler);
void flowStatement(Compiler *compiler);
void synchronize(Compiler *compiler);

void trenaryCond(Compiler *compiler, bool canAssign);
void trenaryCond2(Compiler *compiler, bool canAssign);
void or_(Compiler *compiler, bool canAssign);
void and_(Compiler *compiler, bool canAssign);

void block(Compiler *compiler, size_t indent);

void ifStatement(Compiler *compiler);
void elseStatement(Compiler *compiler);
void loopStatement(Compiler *compiler);
void startLoop(Compiler *compiler, Loop *loop);
void tryStatement(Compiler *compiler);
void raiseStatement(Compiler *compiler);

void breakStatement(Compiler *compiler);
void continueStatement(Compiler *compiler);
void deleteStatement(Compiler *compiler);

void functionStatement(Compiler *compiler);
void arrowFunctionStatement(Compiler *compiler);

void ClassStatement(Compiler *compiler);

void moduleStatement(Compiler *compiler);
void useStatement(Compiler *compiler);
void formStatement(Compiler *compiler);

MyMoFunction *endFunction(Compiler *compiler);

#endif
