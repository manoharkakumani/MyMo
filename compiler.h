#ifndef __COMPILER_H__
#define __COMPILER_H__

#include "lexer.h"
#include "chunk.h"
#include "datatypes/function.h"
#include "debug.h"

typedef struct
{
    MVM *vm;
    Lexer *lexer;
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
    bool repl;
} Parser;

typedef struct Loop
{
    int loopStart;
    int loopJump;
    int *breakJumps;
    int breaksCount;
    int breaksCapacity;
    struct Loop *enclosing;
} Loop;

typedef enum
{
    COMPILE_SCRIPT,
    COMPILE_FUNCTION,
    COMPILE_REPL,
    COMPILE_STRING
} CompileType;

typedef struct CompilerFlags
{
    bool cl_fn;
    u32 pithru;
    u32 dontSetVar;
    u32 argv;
    u32 dict;
    u32 list;
    u32 tuple;
    u32 multiCase;
    u32 casePattern;       // >0 while parsing a case-arm pattern: bare `_` becomes OP_WILDCARD
    u32 casePatternDepth;  // tuple nesting depth inside the pattern
    // Position counter per nesting level (depth-1 indexed). Bumped after
    // each comma-separated element at that depth. Max depth 4 covers all
    // realistic patterns; deeper nests fall back to legacy variable lookup.
    u32 casePatternStackPos[4];
    // Per-arm binding sites discovered while parsing the pattern. Each
    // binding records a path of subscript indices from the scrutinee root
    // to the matched position, so nested patterns like `((_a, _b), _c)`
    // emit correct subscript chains. pathLen == 0 means the whole-
    // scrutinee binding (top-level `_name:` pattern).
    u8  bindingNameIdx[16];
    int bindingPath[16][4];
    int bindingPathLen[16];
    int bindingsCount;
    CompileType compileType;
} CompilerFlags;

typedef struct Compiler
{
    Parser *parser;
    MyMoFunction *function;
    CompilerFlags flags;
    Loop *loop;
} Compiler;

typedef enum
{
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_PITAR,      // |>
    PREC_OR,         // or
    PREC_AND,        // and
    PREC_BAND,       // &
    PREC_BXOR,       // |
    PREC_BOR,        // ^
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_SHIFT,      // << >>
    PREC_TERM,       // + -
    PREC_FACTOR,     // */ % //
    PREC_INDICES,    // **
    PREC_UNARY,      // ! -
    PREC_CALL,       // . () []
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(Compiler *compiler, bool canAssign);

typedef struct
{
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

void arrow(Compiler *compiler, bool canAssign);
void dot(Compiler *compiler, bool canAssign);

//=================TOKEN HANDLING===================
void advanceToken(Compiler *compiler);
void retreatNewLine(Compiler *compiler);
void consumeToken(Compiler *compiler, TokenType type, const char *message);
bool checkToken(Compiler *compiler, TokenType type);
size_t getIndent(Compiler *compiler);
bool matchToken(Compiler *compiler, TokenType type);
void skipNewLines(Compiler *compiler);

bool checkArrow(Compiler *compiler);

//=================PARSER===================
void parsePrecedence(Compiler *compiler, Precedence precedence);
ParseRule *getRule(TokenType type);

//=================COMPILER===================
Compiler *initCompiler(MVM *vm, Parser *parser, FunctionType type);
void freeCompiler(Compiler *compiler);
Chunk *currentChunk(Compiler *compiler);

MyMoFunction *compile(MVM *vm, const char *src, const char *path, CompileType type);

#endif