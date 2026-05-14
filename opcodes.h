#ifndef __OPCODE_H__
#define __OPCODE_H__
typedef enum
{
    OP_NOP,
    OP_CONST,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_LIST,
    OP_TUPLE,
    OP_DICT,
    OP_SUBSCR,
    OP_SETSUBSCR,
    OP_UNPACK,
    OP_SLICE,
    OP_NOT,
    OP_DUP,
    OP_POP,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_BAND,
    OP_BOR,
    OP_BXOR,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_POW,
    OP_MOD,
    OP_LSFT,
    OP_RSFT,
    OP_IDIV,
    OP_NEG, //-
    OP_POS, //+
    OP_JIF,
    OP_JMP,
    OP_CJMP,
    OP_MCASE,
    OP_LOOP,
    OP_ITER,
    OP_GETI,
    OP_GETV,
    OP_SETV,
    OP_DELV,
    OP_MET,
    OP_FN,
    OP_CALL,
    OP_PITHRU,
    OP_RET,
    OP_FRET,
    OP_CLASS,
    OP_SUPERARGS,
    OP_ENDCLASS,
    OP_SETP,
    OP_AGETP, // for assign property
    OP_GETP,
    OP_OGETP, // optional-chain get property: nil-safe `?.`
    OP_IS,    // identity / inline-bit-pattern comparison
    OP_TOSTRING, // pop any value, push its MyMoString representation
    OP_LAPPEND,  // pop a value, append it to the list one below; leaves the list on top (for list comprehensions)
    OP_DELP,
    OP_USE,
    OP_SETM,
    OP_COPY,
    OP_INCR_VAR, // super-instruction: globals/locals[name] += i32_delta (no stack churn)
    OP_WILDCARD, // pushes the OBJ_WILDCARD singleton; emitted only inside case patterns for `_`
    OP_GETARG,    // push frame->args[u8] — direct array access for function parameters
    OP_SETARG,    // store stack-top into frame->args[u8] (for compound assignments to params)
    OP_INVOKE_GLOBAL // fused OP_GETV+OP_CALL: look up global by name and call with N args in one dispatch
} OpCode;
#endif