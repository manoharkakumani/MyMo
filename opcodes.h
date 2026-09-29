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
    OP_INVOKE_GLOBAL, // fused OP_GETV+OP_CALL: look up global by name and call with N args in one dispatch
    // Exception handling.
    //   OP_TRY <s16 offset>      push handler { ip+offset, frame, sp }
    //   OP_ENDTRY                pop the top handler (normal try-block exit)
    //   OP_RAISE                 pop the top value and raise it as an exception
    OP_TRY,
    OP_ENDTRY,
    OP_RAISE,
    //   OP_WIDE <hi1> <hi2>      high bytes for the NEXT instruction's first
    //                            and second constant-pool operands, whose own
    //                            operand bytes hold the low bytes. Emitted
    //                            only when an index exceeds 255 (emitConstOp).
    OP_WIDE,
    //   OP_DEFAULTS <n>          stack: [d1..dn, fn] -> [fn]; sets fn's
    //                            parameter defaults (see functionStatement)
    OP_DEFAULTS,
    //   OP_METV <name>           [class, value, original] -> [class]:
    //                            install a decorated method (value = the
    //                            decorators' result) and bind the original
    OP_METV,
    //   OP_IN                    [item, container] -> [bool] (`in`)
    OP_IN,
    //   OP_SUBSCRK               [obj, key] -> [obj, key, obj[key]] (obj[key] op= v)
    OP_SUBSCRK,
    //   OP_DELSUBSCR             [obj, key] -> [] (del obj[key])
    OP_DELSUBSCR,
    //   OP_CALLKW argc kwc name*  call with keyword arguments: the last kwc
    //                             of the argc values are keyword values
    //                             named by kwc u16 constant indices
    OP_CALLKW,
    //   OP_SET n                 [x0 .. xn-1] -> [set] (set literal)
    OP_SET,
    //   OP_FORMAT conv           [value, spec] -> [string] (f"{value!conv:spec}")
    OP_FORMAT,
    //   OP_EXCMATCH              [exc, types] -> [bool] (catch Types)
    OP_EXCMATCH,
    //   OP_RERAISE               [pending] -> [] (end of final:; raises unless Nil)
    OP_RERAISE,
    //   f(*xs, **d) calls: build [callee, list, dict], then OP_CALLEX
    OP_LEXTEND, //                [list, iterable] -> [list]
    OP_DADD,    //                [dict, name, value] -> [dict]
    OP_DMERGE,  //                [dict, mapping] -> [dict]
    OP_CALLEX,  //                [callee, list, dict] -> [result]
    OP_GETG,    // name           [] -> [globals[name]]    (`global name`)
    OP_SETG,    // name           [v] -> [v]              globals[name] = v
    OP_SETNL,   // name           [v] -> [v]              enclosing function's name = v (`nonlocal`)
    OP_DUPUNDER, //               [a, b] -> [b, a, b]      (a < b < c)
    OP_NIP,      //               [a, b] -> [b]
    //   OP_SETB name             [v] -> [v]   assignment inside a trailing block:
    //                            the block's own local if it has one, else the
    //                            nearest enclosing scope (or global) that has
    //                            `name`, else a new block local
    OP_SETB,
    //   OP_CALLIF                [v] -> [v()] if v is callable, else [v]
    //                            (a bare `name` statement calls it)
    OP_CALLIF,
    //   OP_BLOCKVAR name         [block, v] -> [block]   block.bound[name] = v
    OP_BLOCKVAR
} OpCode;

// Number of opcodes (keep in sync with the last enum entry).
#define OP_COUNT (OP_BLOCKVAR + 1)
#endif