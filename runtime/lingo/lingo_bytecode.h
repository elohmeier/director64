#ifndef DIRECTOR64_LINGO_BYTECODE_H
#define DIRECTOR64_LINGO_BYTECODE_H
// Bytecode contract for generated handlers. src/director64/aot.py holds the
// same table (OPCODES) and tests/test_aot_bytecode.py compares the two, so a
// number changed on one side without the other fails before anything runs.
//
// Operands are big-endian, so the bytes are the same on the host probe and
// the console. Every op below LB_JUMP pushes, pops or stores over the
// running frame's temps exactly as the lx_* service of the same name does;
// every op from LB_JUMP on ends a state and returns its flow, which is what
// keeps step accounting, collection points and yields where the native
// state machines had them.
typedef enum {
  LB_LINE = 0,                // u16 line: the state's source line
  LB_NUM8 = 1,                // i8 literal
  LB_NUM32 = 2,               // i32 literal
  LB_NUMD = 3,                // u16 double-pool index, integral semantics
  LB_DBL = 4,                 // u16 double-pool index, authored float
  LB_TEXT = 5,                // u16 name-pool index
  LB_SYM = 6,                 // u16 name-pool index
  LB_VOID = 7,                //
  LB_LOCAL = 8,               // u8 slot
  LB_GLOBAL = 9,              // u16 slot
  LB_SELF = 10,               // u16 name-pool index
  LB_THE = 11,                // u16 name-pool index
  LB_GET = 12,                // u16 name-pool index
  LB_UNARY = 13,              // u8 lb_unary_t
  LB_BINARY = 14,             // u8 lb_binary_t
  LB_INDEX = 15,              //
  LB_CHUNK = 16,              // u16 name-pool index
  LB_CHUNK_COUNT = 17,        // u16 name-pool index
  LB_CHUNK_SET = 18,          // u16 name-pool index
  LB_CHUNK_DELETE = 19,       // u16 name-pool index
  LB_LAST_CHUNK = 20,         // u16 name-pool index
  LB_LIST = 21,               // u8 count, u8 property-list flag
  LB_LIST_EXTEND = 22,        // u8 count
  LB_REFERENCE = 23,          // u16 name-pool index
  LB_CALL = 24,               // u16 name-pool index, u8 argc
  LB_RESULT = 25,             //
  LB_TRACE = 26,              //
  LB_SET_LOCAL = 27,          // u8 slot
  LB_SET_GLOBAL = 28,         // u16 slot
  LB_SET_SELF = 29,           // u16 name-pool index
  LB_SET_THE = 30,            // u16 name-pool index
  LB_SET = 31,                // u16 name-pool index
  LB_SET_INDEX = 32,          //
  LB_DECLARE = 33,            // u16 name-pool index
  // A call to a name no handler in the corpus defines: the runtime's
  // builtin answers it directly, and the handler search invoke would have
  // made first cannot find anything. The expression form runs any frames
  // the builtin starts to completion, as lx_call does.
  LB_CALL_BUILTIN_EXPR = 34,  // u16 name-pool index, u8 argc
  LB_JUMP = 40,               // u16 pc
  LB_YIELD = 41,              // u16 pc
  LB_BRANCH = 42,             // u16 pc if true, u16 pc if false
  LB_RETURN = 43,             //
  LB_RETURN_VOID = 44,        //
  LB_INVOKE = 45,             // u16 name-pool index, u8 argc, u16 pc
  LB_INVOKE_METHOD = 46,      // u16 name-pool index, u8 argc, u16 pc
  LB_INVOKE_EXPR = 47,        // u16 name-pool index, u8 argc, u16 pc
  LB_INVOKE_METHOD_EXPR = 48, // u16 name-pool index, u8 argc, u16 pc
  // A call the converter bound to one of the movie's own handlers: the
  // calling script defines the name (own = 1, and the callee keeps the
  // caller's receiver), or a movie script defines it and no receiver can
  // shadow it (own = 0), which is where the runtime's lookup would have
  // ended. An explicit receiver in the first argument still resolves by
  // name at run time, as classic method syntax requires.
  LB_INVOKE_LOCAL = 49,       // u16 handler-table index, u8 argc, u8 own, u16 pc
  LB_INVOKE_LOCAL_EXPR = 50,  // u16 handler-table index, u8 argc, u8 own, u16 pc
  LB_CALL_BUILTIN = 51,       // u16 name-pool index, u8 argc, u16 pc (see LB_CALL_BUILTIN_EXPR)
  // A declared script property read or written at the pair its declaration
  // order put it: the u8 is the key's index in the instance's property
  // pairs, checked by one word compare before the named lookup runs.
  LB_SELF_SLOT = 52,          // u16 name-pool index, u8 pair index
  LB_SET_SELF_SLOT = 53       // u16 name-pool index, u8 pair index
} lb_op_t;
// Operators resolve at build time to these ids (aot.py BINARY_OPERATORS and
// UNARY_OPERATORS mirror the tables, keyed by the operator text in each
// comment). The order is the order lv_binary's chain tested them in.
typedef enum {
  LB_BIN_EQ = 0,            // =
  LB_BIN_NE = 1,            // <>
  LB_BIN_AND = 2,           // and
  LB_BIN_OR = 3,            // or
  LB_BIN_CONCAT = 4,        // &
  LB_BIN_CONCAT_SPACE = 5,  // &&
  LB_BIN_CONTAINS = 6,      // contains
  LB_BIN_STARTS = 7,        // starts
  LB_BIN_LT = 8,            // <
  LB_BIN_GT = 9,            // >
  LB_BIN_LE = 10,           // <=
  LB_BIN_GE = 11,           // >=
  LB_BIN_WITHIN = 12,       // within
  LB_BIN_INTERSECTS = 13,   // intersects
  LB_BIN_ADD = 14,          // +
  LB_BIN_SUB = 15,          // -
  LB_BIN_MUL = 16,          // *
  LB_BIN_DIV = 17,          // /
  LB_BIN_MOD = 18,          // mod
  LB_BIN_POW = 19,          // ^
  LB_BIN_COUNT = 20
} lb_binary_t;
typedef enum {
  LB_UN_NEG = 0,            // -
  LB_UN_NOT = 1,            // not
  LB_UN_COUNT = 2
} lb_unary_t;
#endif
