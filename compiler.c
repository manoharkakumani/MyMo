#include "common.h"
#include "compiler.h"
#include "error.h"
#include "memory.h"
#include "vm.h"
#include "bytecode.h"
#include "expression.h"
#include "statement.h"
#include "cache.h"
#include "debug.h"

//=================TOKEN HANDLING===================
void advanceToken(Compiler *compiler)
{
    compiler->parser->previous = compiler->parser->current;
    for (;;)
    {
        compiler->parser->current = getToken(compiler->parser->lexer);
        if (compiler->parser->current.type != ERROR)
            break;
        errorAtCurrent(compiler, compiler->parser->current.token);
    }
}

void retreatNewLine(Compiler *compiler)
{
    compiler->parser->lexer->srcLen -= compiler->parser->current.length;
    compiler->parser->lexer->currentChar = compiler->parser->lexer->src[compiler->parser->lexer->srcLen];
    compiler->parser->current = compiler->parser->previous;
}

void consumeToken(Compiler *compiler, TokenType type, const char *message)
{
    if (compiler->parser->current.type == type)
    {
        advanceToken(compiler);
        return;
    }
    errorAtCurrent(compiler, message);
}

bool checkToken(Compiler *compiler, TokenType type)
{
    return compiler->parser->current.type == type;
}

size_t getIndent(Compiler *compiler)
{
    return compiler->parser->current.indent;
}

bool matchToken(Compiler *compiler, TokenType type)
{
    if (!checkToken(compiler, type))
        return false;
    advanceToken(compiler);
    return true;
}

void skipNewLines(Compiler *compiler)
{
    while (matchToken(compiler, NEWLINE))
        continue;
}

bool checkArrow(Compiler *compiler)
{
    char *token = (char *)(compiler->parser->current.token);
    while (token[0] != ')')
    {
        if (token[0] == '(')
            return false;
        token++;
    }
    token++;
    while (isspace(token[0]))
        token++;
    if (token[0] == '=' && token[1] == '>')
        return true;
    return false;
}

ParseRule rules[] = {
    [LPAR] = {grouping, call, PREC_CALL},
    [RPAR] = {NULL, NULL, PREC_NONE},
    [LBRACE] = {dictionary, NULL, PREC_NONE},
    [RBRACE] = {NULL, NULL, PREC_NONE},
    [LSQB] = {list, subScript, PREC_CALL},
    [RSQB] = {NULL, NULL, PREC_NONE},
    [COMMA] = {NULL, NULL, PREC_NONE},
    [DOT] = {NULL, dot, PREC_CALL},
    [MINUS] = {unary, binary, PREC_TERM},
    [PLUS] = {unary, binary, PREC_TERM},
    [SEMI] = {NULL, NULL, PREC_NONE},
    [COLON] = {NULL, NULL, PREC_NONE},
    [NEWLINE] = {NULL, NULL, PREC_NONE},
    [SLASH] = {NULL, binary, PREC_FACTOR},
    [DSLASH] = {NULL, binary, PREC_FACTOR},
    [PERCENT] = {NULL, binary, PREC_FACTOR},
    [STAR] = {NULL, binary, PREC_FACTOR},
    [DSTAR] = {NULL, binary, PREC_INDICES},
    [AMPER] = {NULL, binary, PREC_BAND},
    [VBAR] = {NULL, binary, PREC_BOR},
    [CAP] = {NULL, binary, PREC_BXOR},
    [EVBAR] = {NULL, NULL, PREC_NONE},
    [EAMPER] = {NULL, NULL, PREC_NONE},
    [ECAP] = {NULL, NULL, PREC_NONE},
    [EQUAL] = {NULL, NULL, PREC_NONE},
    [EXCMARK] = {unary, NULL, PREC_UNARY},
    [NOT] = {unary, NULL, PREC_UNARY},
    [IS] = {NULL, isOp, PREC_EQUALITY},
    [DEQUAL] = {NULL, binary, PREC_EQUALITY},
    [NEQUAL] = {NULL, binary, PREC_EQUALITY},
    [GREATER] = {NULL, binary, PREC_COMPARISON},
    [DGREATER] = {NULL, binary, PREC_SHIFT},
    [EGREATER] = {NULL, binary, PREC_COMPARISON},
    [LESS] = {NULL, binary, PREC_COMPARISON},
    [DLESS] = {NULL, binary, PREC_SHIFT},
    [QMARK] = {NULL, trenaryCond, PREC_ASSIGNMENT},
    [QDOT] = {NULL, optDot, PREC_CALL},
    [ELESS] = {NULL, binary, PREC_COMPARISON},
    [ARROW] = {NULL, NULL, PREC_NONE},
    [PITHRU] = {NULL, pipeThrough, PREC_PITAR},
    [AT] = {NULL, NULL, PREC_NONE},
    [DOLLAR] = {NULL, NULL, PREC_NONE},
    [UPLUS] = {NULL, NULL, PREC_NONE},
    [NAME] = {variable, NULL, PREC_NONE},
    [STRING] = {string_, NULL, PREC_NONE},
    [INT] = {integer_, NULL, PREC_NONE},
    [DOUBLE] = {double_, NULL, PREC_NONE},
    [AND] = {NULL, and_, PREC_AND},
    [CLASS] = {NULL, NULL, PREC_NONE},
    [CASE] = {NULL, NULL, PREC_NONE},
    [COND] = {NULL, NULL, PREC_NONE},
    [ELSE] = {NULL, NULL, PREC_NONE},
    [FALSE] = {literal, NULL, PREC_NONE},
    [WHILE] = {NULL, NULL, PREC_NONE},
    [FOR] = {NULL, NULL, PREC_NONE},
    [BREAK] = {NULL, NULL, PREC_NONE},
    [CONTINUE] = {NULL, NULL, PREC_NONE},
    [FN] = {NULL, NULL, PREC_NONE},
    [IF] = {NULL, trenaryCond2, PREC_ASSIGNMENT},
    [ELIF] = {NULL, NULL, PREC_NONE},
    [OR] = {NULL, or_, PREC_OR},
    [NIL] = {literal, NULL, PREC_NONE},
    [RET] = {NULL, NULL, PREC_NONE},
    [TRUE] = {literal, NULL, PREC_NONE},
    [USE] = {NULL, NULL, PREC_NONE},
    [FROM] = {NULL, NULL, PREC_NONE},
    [PASS] = {NULL, NULL, PREC_NONE},
    [DEL] = {NULL, NULL, PREC_NONE},
    [ERROR] = {NULL, NULL, PREC_NONE},
    [END] = {NULL, NULL, PREC_NONE},
    [YIELD] = {yeild, NULL, PREC_NONE},
};

ParseRule *getRule(TokenType type)
{
    return &rules[type];
}

void parsePrecedence(Compiler *compiler, Precedence precedence)
{
    if (compiler->flags.tuple)
    {
        skipNewLines(compiler);
    }
    advanceToken(compiler);
    ParseFn prefixRule = getRule(compiler->parser->previous.type)->prefix;
    if (prefixRule == NULL)
    {
        error(compiler, "Expect expression.");
        return;
    }
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(compiler, canAssign);
    if (compiler->flags.tuple)
    {
        skipNewLines(compiler);
    }
    // printToken(&compiler->parser->current);
    while (precedence <= getRule(compiler->parser->current.type)->precedence)
    {
        advanceToken(compiler);
        ParseFn infixRule = getRule(compiler->parser->previous.type)->infix;
        infixRule(compiler, canAssign);
    }
    if (canAssign && matchToken(compiler, EQUAL))
    {
        error(compiler, "Invalid assignment target.");
    }
}

Parser *initParser(MVM *vm, const char *src)
{
    Parser *parser = New(Parser, 1);
    parser->vm = vm;
    parser->lexer = initLexer(src);
    parser->hadError = false;
    parser->panicMode = false;
    return parser;
}

void freeParser(Parser *parser)
{
    freeLexer(parser->lexer);
    free(parser);
}

Compiler *initCompiler(MVM *vm, Parser *parser, FunctionType type)
{
    Compiler *compiler = New(Compiler, 1);
    compiler->parser = parser;
    compiler->loop = NULL;
    compiler->flags.cl_fn = false;
    compiler->flags.dontSetVar = 0;
    compiler->flags.pithru = 0;
    compiler->flags.argv = 0;
    compiler->flags.dict = 0;
    compiler->flags.list = 0;
    compiler->flags.tuple = 0;
    compiler->flags.multiCase = 0;
    compiler->flags.casePattern = 0;
    compiler->flags.casePatternDepth = 0;
    for (int i = 0; i < 4; i++) compiler->flags.casePatternStackPos[i] = 0;
    compiler->flags.bindingsCount = 0;
    MyMoFunction *function = newFunction(vm);
    function->type = type;
    if (type != FN_SCRIPT && type != FN_ARROWFN && type != FN_COMPILED)
    {
        function->name = newString(parser->vm, parser->previous.token, parser->previous.length);
    }
    compiler->function = function;
    compiler->flags.compileType = COMPILE_FUNCTION;
    return compiler;
}

void freeCompiler(Compiler *compiler)
{
    freeParser(compiler->parser);
    free(compiler);
}

MyMoFunction *compile(MVM *vm, const char *src, const char *path, CompileType type)
{
    Parser *parser = initParser(vm, src);
    Compiler *compiler = initCompiler(vm, parser, type == COMPILE_STRING ? FN_COMPILED : FN_SCRIPT);
    compiler->flags.compileType = type;
    advanceToken(compiler);
    skipNewLines(compiler);
    if (!matchToken(compiler, END))
    {
        statements(compiler);
    }
    emitByte(compiler, OP_RET);
    MyMoFunction *function = compiler->parser->hadError ? NULL : compiler->function;
    freeCompiler(compiler);
    if (function)
    {
        function->name = newString(vm, path, strlen(path));
#ifdef NOCACHE
#else
        if (path && type != COMPILE_REPL)
        {
            char *cachePath = malloc(strlen(path) + 2);
            strcpy(cachePath, path);
            strcat(cachePath, "c");
            FILE *stream = fopen(cachePath, "wb");
            functionSerialize(function, stream);
            fclose(stream);
            free(cachePath);
        }
#endif
    }
    return function;
}

Chunk *currentChunk(Compiler *compiler)
{
    return compiler->function->chunk;
}