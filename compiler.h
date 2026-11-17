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
    int tryDepth; // compiler->tryDepth when the loop started
    // A `for` keeps its iterator on the operand stack for the whole loop
    // (popped at normal exit). break/return jump past that pop, so they
    // must pop it themselves.
    bool isFor;
    int seq; // Compiler.scopeSeq at entry: orders loops against TryCtxs
    // A `for`'s target names: a block created inside the loop pins their
    // current values (OP_BLOCKVAR).
    Token names[8];
    int nameCount;
    struct Loop *enclosing;
} Loop;

// An open `try` (or the `final:` body of one) in the current function.
// return/break/continue that leave a try don't jump out directly: they
// push [Nil, k] and jump to its final block, whose tail dispatches on k
// and re-issues the exit from outside the try. While the final body
// itself compiles (inFinal), [pending, code] sit on the stack, so an exit
// from there pops them.
#define TRY_MAX_EXITS 32
typedef struct TryCtx
{
    int outerDepth; // compiler->tryDepth outside the try
    int seq;
    bool inFinal;
    int exitCount;
    u8 exitKinds[TRY_MAX_EXITS];
    int exitJumps[TRY_MAX_EXITS];
    struct TryCtx *enclosing;
} TryCtx;

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
    u16 bindingNameIdx[16];
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
    // `try` bodies currently open in this function. break/continue emit
    // one OP_ENDTRY per try they jump out of, or the VM's handler stack
    // would keep a stale entry (and overflow after 32).
    int tryDepth;
    TryCtx *tryCtx;
    int scopeSeq;
    // Chunk offset of the instruction the most recent OP_WIDE prefixes
    // (-1 if none). Peepholes that rewind an instruction must not split
    // it from its prefix.
    int lastWideTarget;
    // Chunk offset where the left operand of the infix operator being
    // compiled starts (set by parsePrecedence). `a if c else b` moves a's
    // code behind the condition so `a` only runs when `c` holds.
    int infixLeftStart;
    // Names this function declared `global` (kind 1) or `nonlocal`
    // (kind 2): assignments go to the module globals / the enclosing
    // function's variable instead of a new local.
    MyMoString *scopeNames[64];
    u8 scopeKinds[64];
    int scopeCount;
} Compiler;

#define SCOPE_GLOBAL 1
#define SCOPE_NONLOCAL 2

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
void attachSource(MyMoFunction *function, const char *src);

#endif