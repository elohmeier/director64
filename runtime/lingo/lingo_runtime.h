#ifndef DIRECTOR64_LINGO_RUNTIME_H
#define DIRECTOR64_LINGO_RUNTIME_H
#include "family.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "lingo_names.h"

// Runtime services for generated native state machines. No source/bytecode
// evaluator.
#define LV_GLOBALS 1024
#define LV_LOCALS 96
// Call depth, and the slots every frame's locals and expression temps share.
// A frame takes only what its handler declares plus the temps it reaches, so
// the recursion authored code does (a puzzle piece dragged against a wall
// recurses once per stage update) is bounded by these, not by the widest
// handler in the corpus.
#define LV_FRAMES 256
#define LV_STACK_SLOTS 4096
#define LV_TEXT_BYTES 16384
#ifndef DIRECTOR64_DIRECTOR_VERSION
#define DIRECTOR64_DIRECTOR_VERSION 6
#endif
#ifndef DIRECTOR64_EXTENDED_D6
#define DIRECTOR64_EXTENDED_D6 0
#endif
#ifndef LV_HEAP_BYTES
#if DG_D10
// The D10 main screen peaks past 320 KiB; overruns fail visibly, never
// silently, and the console budget covers this after the plane-pool removal.
#define LV_HEAP_BYTES (384u * 1024u)
#else
#define LV_HEAP_BYTES (512u * 1024u)
#endif
#endif
#if DG_D5
#define LV_OBJECTS 4096
#define LV_ROOTS 256
#define LV_SHARED 4
#elif DG_D10
#define LV_OBJECTS 8192
// A thousand sprite behavior roots follow the widened field roots (192
// mutable fields for the exercise stamp sets), then the timeout and Flash
// object slots; see director.h.
#define LV_ROOTS 1536
// An exercise stage movie links eleven external cast files while the
// controller window keeps its own code and casts resident beside it.
#define LV_SHARED 24
#elif DG_CAP_WIDE
#define LV_OBJECTS 8192
#define LV_ROOTS 1024
#define LV_SHARED 12
#elif DG_EXTENDED
#define LV_OBJECTS 8192
#define LV_ROOTS 256
#define LV_SHARED 5
#else
#define LV_OBJECTS 2048
#define LV_ROOTS 136
#define LV_SHARED 4
#endif
#define LV_SCRIPTS 512
// Objects threaded in heap order, so compaction walks them instead of sorting
// a handle array. Every profile that has it allocates only at the heap's bump
// top between collections, which is what keeps the thread in heap order for
// free; only Director 7 and later ever place an object in a freed hole, and
// only that path has to insert rather than append.
#if DG_CAP_ALLOCATION_CHAIN
#define LV_ALLOCATION_CHAIN 1
#else
#define LV_ALLOCATION_CHAIN 0
#endif
typedef enum {
  LV_VOID,
  LV_NUMBER,
  LV_STRING,
  LV_SYMBOL,
  // A list of byte values, stored one byte per element instead of a full
  // value each: a task database read with baReadBinFile is thousands of
  // them, and the authored parser only counts and indexes it. Kept below
  // LV_LIST so the collector and allocator treat its storage as bytes.
  LV_BYTES,
  LV_LIST,
  LV_PROPLIST,
  LV_SPRITE,
  LV_MEMBER,
  LV_FIELD,
  LV_FILE,
  LV_SCRIPT,
  LV_INSTANCE,
  LV_XTRA,
  LV_SOUND,
  LV_COLOR, // Packed 0xRRGGBB in id; distinct from a numeric palette index.
  LV_CASTLIB,
  LV_WINDOW
#if DG_D10
  // Live view over the fixed global table: `the globals` reads and writes
  // resolve straight to the named global slots.
  ,
  LV_GLOBALS_VIEW,
  // Handle to a member's pixel surface (imaging Lingo). Mutation operations
  // stay traced approximations until a native compositor exists.
  LV_IMAGE,
  // Authored timeout object; id is the registry slot plus one.
  LV_TIMEOUT
#endif
} lv_type_t;
// A value is one 64-bit word, returned in a register: the o64 ABI returns
// any struct through memory, which put the C stack at the top of the
// console's cache-miss profile while a value was a 16-byte struct. Handles
// and integers are boxed with a zero top half, the type in bits 32..47 and
// a 32-bit id or integer below, so the all-zero word is VOID and zeroed
// memory reads as VOID values. A float is its IEEE double with 2^49 added
// to the bits: no double comes out of that with a zero top half, and NaNs
// are canonicalized so the negative-NaN space is never stored.
typedef uint64_t lv_t;
#define LV_FLOAT_OFFSET (UINT64_C(1) << 49)
static inline unsigned lv_type(lv_t v) {
  return (v >> 48) ? (unsigned)LV_NUMBER : (unsigned)((v >> 32) & 0xFFFF);
}
// The id of a handle, or the value of an integer.
static inline int32_t lv_id(lv_t v) { return (int32_t)(uint32_t)v; }
static inline bool lv_is_float(lv_t v) { return (v >> 48) != 0; }
// A macro rather than a function so that static tables can hold values.
#define lv_make(type, id) (((uint64_t)(type) << 32) | (uint32_t)(int32_t)(id))
static inline lv_t lv_float(double d) {
  if (d != d) d = __builtin_nan("");
  uint64_t bits;
  memcpy(&bits, &d, sizeof bits);
  return bits + LV_FLOAT_OFFSET;
}
static inline double lv_double(lv_t v) {
  uint64_t bits = v - LV_FLOAT_OFFSET;
  double d;
  memcpy(&d, &bits, sizeof d);
  return d;
}
// A number's value, integer or float. Valid only for LV_NUMBER.
static inline double lv_numeric(lv_t v) {
  return lv_is_float(v) ? lv_double(v) : (double)lv_id(v);
}
typedef enum { LV_CONTINUE, LV_RETURN, LV_YIELD, LV_CALL } lv_flow_t;
typedef struct lv_runtime lv_runtime_t;
typedef struct lv_frame lv_frame_t;
typedef lv_flow_t (*lv_step_fn)(lv_runtime_t *, lv_frame_t *);
typedef struct {
  const char *name;
  uint16_t member;
  const char *cast, *kind;
  unsigned arguments, locals, entry;
  lv_step_fn step;
  const char *const *local_names;
  unsigned property_count;
  const char *const *property_names;
  // Generated handlers carry bytecode (lingo_bytecode.h) instead of a native
  // step function; lv_vm_step runs one state of it per call over the same
  // compact ops. A NULL code pointer selects step, which hand-written
  // handlers in tests and adapters still use.
  const uint8_t *code;
  uint32_t code_size;
} lv_handler_t;
// A symbol is an id, not a heap string. The converter numbers every symbol
// the corpus spells (lb_symbols, generated per game as symbols.c, in bucket
// order of lv_text_hash so a lookup by text reads one bucket); the runtime
// interns the rest (symbol(), value("#x"), names a service looks up that
// the corpus never spelled) in a small resident table behind them. Equal
// symbols have equal ids, and the text is read only to print, concatenate
// or compare a symbol against a string.
typedef struct {
  const char *const *text;
  unsigned count;
  const uint16_t *buckets; // bucket b is the id range [buckets[b], buckets[b + 1])
  unsigned bucket_count;   // a power of two
} lb_symbol_table_t;
extern const lb_symbol_table_t lb_symbols; // symbols.c, or the runtime's empty default
#define LV_NO_SYMBOL 0xFFFFu
#define LV_DYNAMIC_SYMBOL_SLOTS 512
#define LV_DYNAMIC_SYMBOLS 384
#define LV_SYMBOL_BYTES 6144
// One pooled name: the text a service receives, the runtime's dispatch id
// (LB_NAME_COUNT where it dispatches on no such name) and the symbol id
// (LV_NO_SYMBOL where no op reads the name as one), resolved by the
// converter so a generated handler never searches for either. One record
// is one cache line per bind, where three parallel tables were three.
typedef struct {
  const char *text;
  uint16_t name_id, symbol_id;
} lv_name_t;
typedef struct {
  const char *name;
  unsigned count;
  const lv_handler_t *handlers;
  // Interned tables for the compact expression ops: every name and float
  // literal a movie's generated code references, so call sites carry
  // two-byte indices instead of materializing pointer constants.
  const lv_name_t *names;
  const double *doubles;
  // The converter's index of the handler table by name: bucket b of the
  // bucket_count (a power of two) covers handler_order[handler_buckets[b]
  // .. handler_buckets[b + 1]), entry indices in table order, and a name
  // belongs to bucket lv_text_hash(name) & (bucket_count - 1). A lookup
  // reads one bucket instead of walking every entry; hand-written movies
  // leave the index NULL and are walked.
  const uint16_t *handler_order, *handler_buckets;
  unsigned bucket_count;
} lv_movie_t;
struct lv_frame {
  const lv_movie_t *movie;
  const lv_handler_t *handler;
  unsigned pc, line;
  bool expression;
  lv_t self;
  // Both point into lv_runtime_t.stack. temp_extent is the deepest the temps
  // have reached: values popped as call arguments stay addressable up to it,
  // so a callee's slots begin above it rather than above temp_count.
  lv_t *locals, *temps;
  unsigned temp_count, temp_extent, temp_limit;
};
typedef struct {
  lv_t (*get)(lv_runtime_t *, const char *, lv_t);
  void (*set)(lv_runtime_t *, const char *, lv_t, lv_t);
  lv_t (*reference)(lv_runtime_t *, const char *, lv_t, lv_t);
  bool (*call)(lv_runtime_t *, const char *, unsigned, const lv_t *, lv_t *,
               bool *);
  void (*trace)(lv_runtime_t *, lv_t);
  const lv_handler_t *(*resolve)(lv_runtime_t *, lv_t, const char *,
                                 const lv_movie_t **);
  lv_t (*script)(lv_runtime_t *, const lv_movie_t *, const lv_handler_t *);
  bool (*format)(lv_runtime_t *, lv_t, char *, size_t);
} lv_services_t;
typedef struct {
  uint32_t offset, count, capacity;
  uint8_t type, mark;
  uint8_t geometry, sorted;
  int32_t script;
#if LV_ALLOCATION_CHAIN
  uint16_t previous, next; // Allocation order; LV_OBJECTS is the end sentinel.
#endif
  // Allocation serial at birth. Every profile records it: the value
  // formatter names an instance by it, so a probe transcript does not
  // depend on when the collector ran or which handle it reused. From
  // Director 7 on the in-place collector that runs on allocation failure
  // also reads it, preserving everything born during the current
  // interpreter step because native helpers hold those values where no
  // root table can see them.
  uint32_t birth;
} lv_object_t;
// What a by-name call from one site resolved to. Keyed by the pooled
// name's address and the calling handler, so a hit is three word compares
// and no string compare; the generation retires every entry when an
// overlay comes or goes. Sized to the bytes the old name-keyed set took,
// so nothing after it in the runtime moves in the data cache.
typedef struct {
  const char *name;
  const lv_handler_t *caller, *handler; // handler NULL: resolved to nothing
  const lv_movie_t *owner;
  uint32_t generation;
  uint8_t valid, self_void;
} lv_call_cache_t;
// The old table's bytes on the console, where the runtime's offsets are tuned.
enum { LV_CALL_BYTES = 4864, LV_CALL_SLOTS = LV_CALL_BYTES / sizeof(lv_call_cache_t) };
struct lv_runtime {
  // The scalars every interpreter step reads or writes, in the structure's
  // first cache lines. They used to sit a megabyte apart at the end of it,
  // on four different lines that the direct-mapped cache evicted between
  // steps (measured with the emulator's miss profile, 2026-09-26).
  unsigned depth;
  uint32_t steps;
  unsigned allocations_since_gc;
  // Occupied handles, live or not yet collected. Only a collection frees a
  // handle, so LV_OBJECTS minus this is exactly what remains allocatable
  // before the next pass.
  unsigned object_count;
  uint32_t heap_used;
  // Monotonic allocation serial and its value at the current step boundary;
  // see lv_object_t.birth. Only Director 7 and later move step_serial.
  uint32_t allocation_serial, step_serial;
  unsigned atomic_depth;
  unsigned arithmetic_depth;
  bool failed, yielded, aborted;
  // Script-class failures reproduce original Lingo alerts: the projector
  // discards the call contexts and keeps playing. dg_service recovers them.
  bool script_error;
  // The name a generated handler is about to pass to a service, and its id:
  // the dispatch chains read the id through lv_name_id and compare integers
  // instead of walking string literals. Any other caller misses the pointer
  // compare and pays lb_name_id's search.
  uint16_t name_id;
  const char *name_id_for;
  // Likewise the symbol id of that name, for the property protocol.
  const char *symbol_for;
  unsigned symbol;
  lv_t result;
  lv_services_t services;
  void *context;
  const char *const *global_names;
  unsigned global_count;
  const lv_movie_t *current, *shared[LV_SHARED];
  unsigned shared_count;
  lv_call_cache_t call_cache[LV_CALL_SLOTS];
  uint8_t call_cache_pad[LV_CALL_BYTES - LV_CALL_SLOTS * sizeof(lv_call_cache_t)];
  uint32_t call_generation;
  lv_t globals[LV_GLOBALS], roots[LV_ROOTS], the_result;
  lv_t script_objects[LV_SCRIPTS];
  unsigned script_count;
  unsigned char item_delimiter;
  // The frames, then the slots every frame's locals and temps are cut from.
  // The runtime is placed at a fixed offset into the data cache's period
  // (platforms/n64/director_main.c), so where these land in the cache is
  // decided here and measured with tools/cache-profile.
  lv_frame_t frames[LV_FRAMES];
  lv_t stack[LV_STACK_SLOTS];
  lv_object_t objects[LV_OBJECTS];
#if LV_ALLOCATION_CHAIN
  uint16_t allocation_first, allocation_last;
#endif
#if DG_EXTENDED
  // The dynamic symbols' hash slots. The extended-D6 profile keeps this
  // kilobyte here, where the symbol cache it replaced was measured; every
  // other profile appends it below so that adding it moves none of the
  // offsets above.
  uint16_t dynamic_symbol_slot[LV_DYNAMIC_SYMBOL_SLOTS];
#endif
  _Alignas(lv_t) uint8_t heap[LV_HEAP_BYTES];
  char text_scratch[LV_TEXT_BYTES];
  uint32_t heap_high_water, random_state;
  // Collector activity counters for performance evidence: full collections
  // and in-place emergency passes since boot. Monotonic; readers diff them.
  uint32_t collect_passes, emergency_passes;
  // Microseconds spent collecting, when the platform supplies a clock. A
  // collection is the longest uninterruptible span the runtime has, so it is
  // worth separating from the interpreter time around it.
  uint64_t (*clock_us)(void);
  uint64_t collect_us;
  unsigned object_hint, object_limit;
  uint32_t script_error_count;
  char error[160];
  char last_script_error[160];
  // Appended rather than grouped with the other tables: the offsets above
  // are tuned against a direct-mapped 8 KiB data cache.
#if !DG_EXTENDED
  uint16_t dynamic_symbol_slot[LV_DYNAMIC_SYMBOL_SLOTS];
#endif
  // Global slots by folded name hash (index + 1, 0 empty): the dynamic
  // paths (do, value) look globals up by name, and a walk over hundreds
  // of names per lookup showed in the console's miss profile.
  uint16_t global_slots[2048];
  // The symbols interned at run time, behind the converter's table: slot
  // holds index + 1 (0 empty), offsets point into the text arena.
  unsigned dynamic_symbol_count, symbol_text_used;
  uint16_t dynamic_symbol_offset[LV_DYNAMIC_SYMBOLS];
  char symbol_text[LV_SYMBOL_BYTES];
  // Which script_objects entry answers for a script, validated against the
  // entry before use: singleton() otherwise walked the table and touched a
  // random object header per entry, a few percent of all data misses.
  struct {
    int32_t script;
    uint16_t index;
  } singleton_memo[64];
};
void lv_init(lv_runtime_t *, lv_services_t, void *, const char *const *,
             unsigned, uint32_t);
// The id of a name the runtime dispatches on: a pointer compare for a name
// a generated handler bound, the sorted-table search for anything else.
static inline unsigned lv_name_id(lv_runtime_t *r, const char *name) {
  return r->name_id_for == name ? r->name_id : lb_name_id(name);
}
void lv_fail(lv_runtime_t *, const char *);
void lv_script_fail(lv_runtime_t *, const char *);
bool lv_recover_script(lv_runtime_t *);
lv_t lv_num(double);
double lv_number(lv_runtime_t *, lv_t);
int32_t lv_integer(lv_runtime_t *, lv_t);
bool lv_truth(lv_runtime_t *, lv_t);
const char *lv_cstr(lv_runtime_t *, lv_t);
lv_t lv_text(lv_runtime_t *, const char *, bool);
lv_t lv_bytes(lv_runtime_t *, const void *, unsigned);
#if DG_D10
lv_t lv_byte_list(lv_runtime_t *, const unsigned char *, unsigned);
#endif
lv_t lv_list(lv_runtime_t *, unsigned, const lv_t *, bool);
lv_t lv_point(lv_runtime_t *, double, double);
lv_t lv_rect(lv_runtime_t *, double, double, double, double);
void lv_self_declare(lv_runtime_t *, lv_frame_t *, const char *);
lv_t lv_at(lv_runtime_t *, lv_t, unsigned);
bool lv_set_at(lv_runtime_t *, lv_t, unsigned, lv_t);
unsigned lv_count(lv_runtime_t *, lv_t);
lv_t lv_unary(lv_runtime_t *, const char *, lv_t);
lv_t lv_binary(lv_runtime_t *, const char *, lv_t, lv_t);
lv_t lv_get(lv_runtime_t *, lv_frame_t *, const char *, lv_t);
bool lv_has_property(lv_runtime_t *, lv_t, const char *);
void lv_set(lv_runtime_t *, lv_frame_t *, const char *, lv_t, lv_t);
lv_t lv_reference(lv_runtime_t *, const char *, lv_t, lv_t);
lv_t lv_chunk(lv_runtime_t *, const char *, lv_t, lv_t, lv_t);
lv_t lv_chunk_set(lv_runtime_t *, const char *, lv_t, lv_t, lv_t, lv_t);
lv_t lv_chunk_delete(lv_runtime_t *, const char *, lv_t, lv_t, lv_t);
unsigned lv_chunk_count(lv_runtime_t *, const char *, lv_t);
lv_t lv_index_get(lv_runtime_t *, lv_t, lv_t);
void lv_index_set(lv_runtime_t *, lv_t, lv_t, lv_t);
lv_t lv_self_get(lv_runtime_t *, lv_frame_t *, const char *);
void lv_self_set(lv_runtime_t *, lv_frame_t *, const char *, lv_t);
lv_t lv_instance(lv_runtime_t *, lv_t);
bool lv_start_method(lv_runtime_t *, lv_t, const char *, unsigned, const lv_t *);
bool lv_start_cast_args(lv_runtime_t *, const lv_movie_t *, const char *,
                        unsigned, const char *, unsigned, const lv_t *);
lv_t lv_call_method(lv_runtime_t *, lv_frame_t *, const char *, lv_t, unsigned,
                     const lv_t *);
// As lv_call_method, but collection keeps running between the callee's
// steps; the caller's live values must be collector-visible.
lv_t lv_dispatch_method(lv_runtime_t *, lv_frame_t *, const char *, lv_t,
                        unsigned, const lv_t *);
lv_flow_t lv_invoke_method(lv_runtime_t *, lv_frame_t *, const char *, lv_t,
                            unsigned, const lv_t *);
lv_flow_t lv_invoke_method_expr(lv_runtime_t *, lv_frame_t *, const char *, lv_t,
                                 unsigned, const lv_t *);
lv_t lv_call(lv_runtime_t *, lv_frame_t *, const char *, unsigned,
             const lv_t *);
lv_t lv_call_pooled(lv_runtime_t *, lv_frame_t *, const char *, unsigned, const lv_t *);
lv_flow_t lv_invoke(lv_runtime_t *, lv_frame_t *, const char *, unsigned,
                    const lv_t *);
lv_flow_t lv_invoke_expr(lv_runtime_t *, lv_frame_t *, const char *, unsigned,
                         const lv_t *);
void lv_trace(lv_runtime_t *, lv_t);
const lv_handler_t *lv_find(lv_runtime_t *, const lv_movie_t *, const char *, unsigned);
// FNV-1a over the name with A-Z folded: the key of every generated name
// index (handler tables here, symbols, member names in the director tables).
uint32_t lv_text_hash(const char *);
// The symbol spelled `text`: the converter's id where the corpus spells it,
// an interned one otherwise (VOID with the runtime failed when the resident
// table is full). lv_text(r, text, true) is the same call.
lv_t lv_symbol(lv_runtime_t *, const char *);
const char *lv_symbol_text(const lv_runtime_t *, lv_t);
// The id of the symbol a generated handler bound (op_bind), else interned;
// LV_NO_SYMBOL when the runtime failed to intern it.
unsigned lv_symbol_id(lv_runtime_t *, const char *);
// The first handler of a movie's table named `name` that belongs to
// `member` (0 selects MovieScript handlers when cast is NULL). A non-NULL
// cast additionally requires the handler's cast name, and then member 0 is
// an ordinary member.
const lv_handler_t *lv_scan(const lv_movie_t *, const char *name, unsigned member,
                            const char *cast);
bool lv_start(lv_runtime_t *, const lv_movie_t *, const char *, unsigned,
              unsigned, const lv_t *);
bool lv_start_cast(lv_runtime_t *, const lv_movie_t *, const char *, unsigned,
                   const char *);
bool lv_run(lv_runtime_t *, unsigned);
// One bytecode state of the frame's handler, as its step function ran one case.
lv_flow_t lv_vm_step(lv_runtime_t *, lv_frame_t *);
void lv_collect(lv_runtime_t *);
bool lv_has_reference(lv_runtime_t *, lv_type_t, int32_t);
bool lv_format(lv_runtime_t *, lv_t, char *, size_t);
lv_t lv_literal(lv_runtime_t *, const char *);
int lv_global_id(lv_runtime_t *, const char *);

// Compact expression ops for generated native handlers. They evaluate over
// the running frame's temp array — which the collector already roots — so a
// call site is one jal with small immediates instead of 16-byte value
// plumbing through memory, and a suspension keeps its operands live with no
// spill code at all. Name and float-literal indices resolve through the
// frame's movie tables. Every op keeps its stack delta even after a
// failure, so generated sequences stay balanced.
void lx_num(lv_runtime_t *, int32_t);
void lx_numd(lv_runtime_t *, unsigned);
void lx_dbl(lv_runtime_t *, unsigned);
void lx_text(lv_runtime_t *, unsigned);
void lx_sym(lv_runtime_t *, unsigned);
void lx_void(lv_runtime_t *);
void lx_local(lv_runtime_t *, unsigned);
void lx_global(lv_runtime_t *, unsigned);
void lx_self(lv_runtime_t *, unsigned);
void lx_the(lv_runtime_t *, unsigned);
void lx_get(lv_runtime_t *, unsigned);
void lx_unary(lv_runtime_t *, unsigned);  // lb_unary_t
void lx_binary(lv_runtime_t *, unsigned); // lb_binary_t
void lx_index(lv_runtime_t *);
void lx_chunk(lv_runtime_t *, unsigned);
void lx_chunk_count(lv_runtime_t *, unsigned);
void lx_chunk_set(lv_runtime_t *, unsigned);
void lx_chunk_delete(lv_runtime_t *, unsigned);
void lx_last_chunk(lv_runtime_t *, unsigned);
void lx_list(lv_runtime_t *, unsigned, unsigned);
void lx_list_extend(lv_runtime_t *, unsigned);
void lx_reference(lv_runtime_t *, unsigned);
void lx_call(lv_runtime_t *, unsigned, unsigned);
void lx_result(lv_runtime_t *);
void lx_trace(lv_runtime_t *);
void lx_set_local(lv_runtime_t *, unsigned);
void lx_set_global(lv_runtime_t *, unsigned);
void lx_set_self(lv_runtime_t *, unsigned);
void lx_set_the(lv_runtime_t *, unsigned);
void lx_set(lv_runtime_t *, unsigned);
void lx_set_index(lv_runtime_t *);
lv_flow_t lx_branch(lv_runtime_t *, unsigned, unsigned);
lv_flow_t lx_return(lv_runtime_t *);
lv_flow_t lx_invoke(lv_runtime_t *, unsigned, unsigned);
// lv_call for a name no handler defines: the builtin first (LB_CALL_BUILTIN_EXPR).
lv_t lv_call_builtin(lv_runtime_t *, lv_frame_t *, const char *, unsigned, const lv_t *);
lv_flow_t lx_invoke_method(lv_runtime_t *, unsigned, unsigned);
lv_flow_t lx_invoke_expr(lv_runtime_t *, unsigned, unsigned);
lv_flow_t lx_invoke_method_expr(lv_runtime_t *, unsigned, unsigned);
#endif
