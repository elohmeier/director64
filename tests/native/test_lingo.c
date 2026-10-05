#include "lingo_bytecode.h"
#include "lingo_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_runtime_t runtime;
static lv_flow_t idle(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  (void)f;
  return LV_RETURN;
}
static const lv_handler_t handler = {
    .name = "test", .kind = "MovieScript", .locals = 2, .step = idle};
static const lv_movie_t movie = {"synthetic", 1, &handler,NULL, NULL, NULL, NULL, 0};
static const char *const globals[] = {"root", "other"};
// down(n, caller): recurse through the compact ops until n reaches zero. Each
// level passes its own n as the second argument, which the callee checks, so
// arguments popped off the caller's temps must survive the callee's push.
static unsigned deepest;
static lv_flow_t down(lv_runtime_t *r, lv_frame_t *f) {
  switch (f->pc) {
  case 0: r->result = (lv_t){0}; return LV_RETURN;
  case 1: lx_local(r, 0); lx_num(r, 1); lx_binary(r, LB_BIN_SUB); lx_local(r, 0);
    f->pc = 0; return lx_invoke(r, 0, 2);
  default:
    if (lv_type(f->locals[1]) != LV_VOID)
      assert(lv_number(r, f->locals[1]) == lv_number(r, f->locals[0]) + 1);
    if (r->depth > deepest) deepest = r->depth;
    lx_local(r, 0); lx_num(r, 0); lx_binary(r, LB_BIN_GT); return lx_branch(r, 1, 0);
  }
}
static const lv_name_t down_names[] = {{"down", LB_NAME_COUNT, LV_NO_SYMBOL}, {"-", LB_NAME_COUNT, LV_NO_SYMBOL}, {">", LB_NAME_COUNT, LV_NO_SYMBOL}};
static const lv_handler_t down_handler = {
    .name = "down", .kind = "MovieScript", .arguments = 2, .locals = 2,
    .entry = 2, .step = down};
static const lv_movie_t down_movie = {"recursion", 1, &down_handler,
                                      down_names, NULL, NULL, NULL, 0};
static const char *const named_locals[] = {"recept3", "other"};
static const lv_handler_t named_handler = {
    .name = "named", .kind = "MovieScript", .locals = 2, .step = idle,
    .local_names = named_locals};
static const lv_movie_t named_movie = {"named", 1, &named_handler, NULL, NULL, NULL, NULL, 0};
static void recurse(int n) {
  lv_init(&runtime, (lv_services_t){0}, NULL, globals, 2, 42);
  lv_t args[] = {lv_num(n)};
  deepest = 0;
  assert(lv_start(&runtime, &down_movie, "down", 0, 1, args));
  lv_run(&runtime, 1000000);
}
static void reset(void) {
  lv_init(&runtime, (lv_services_t){0}, NULL, globals, 2, 42);
  assert(lv_start(&runtime, &movie, "test", 0, 0, NULL));
}
static lv_t call(const char *name, unsigned n, const lv_t *args) {
  lv_t result = lv_call(&runtime, &runtime.frames[0], name, n, args);
  if (runtime.failed)
    fprintf(stderr, "%s\n", runtime.error);
  assert(!runtime.failed);
  return result;
}
// The generated handler index answers exactly what the table walk answers:
// the first entry in table order, name folded, member and kind filters
// intact, for every query shape the runtime makes.
static void test_handler_index(void) {
  static const lv_handler_t table[] = {
      {.name = "exitframe", .member = 3, .cast = "Internal", .kind = "BehaviorScript", .step = idle},
      {.name = "ExitFrame", .member = 0, .cast = "Internal", .kind = "MovieScript", .step = idle},
      {.name = "enterframe", .member = 3, .cast = "Internal", .kind = "BehaviorScript", .step = idle},
      {.name = "helper", .member = 3, .cast = "External", .kind = "BehaviorScript", .step = idle},
      {.name = "helper", .member = 3, .cast = "Internal", .kind = "BehaviorScript", .step = idle},
      {.name = "helper", .member = 7, .cast = "Internal", .kind = "ParentScript", .step = idle},
      {.name = "new", .member = 7, .cast = "Internal", .kind = "ParentScript", .step = idle},
  };
  enum { COUNT = sizeof table / sizeof *table, BUCKETS = 8 };
  static uint16_t order[COUNT], buckets[BUCKETS + 1];
  unsigned n = 0;
  for (unsigned b = 0; b < BUCKETS; b++) {
    buckets[b] = (uint16_t)n;
    for (unsigned i = 0; i < COUNT; i++)
      if ((lv_text_hash(table[i].name) & (BUCKETS - 1)) == b) order[n++] = (uint16_t)i;
  }
  buckets[BUCKETS] = (uint16_t)n;
  assert(n == COUNT);
  // The converters compute the same key (tests/test_aot_bytecode.py).
  assert(lv_text_hash("exitframe") == 945048422u && lv_text_hash("ExitFrame") == 945048422u);
  assert(lv_text_hash("") == 2166136261u);
  const lv_movie_t walked = {"INDEX.DXR", COUNT, table, NULL, NULL, NULL, NULL, 0};
  const lv_movie_t indexed = {"INDEX.DXR", COUNT, table, NULL, NULL, order, buckets, BUCKETS};
  static const char *const names[] = {"exitframe", "EXITFRAME", "enterframe", "helper", "Helper",
                                      "new", "missing", "", "e", "exitframes", "exitfram"};
  static const char *const casts[] = {NULL, "Internal", "External", "internal", ""};
  for (unsigned q = 0; q < sizeof names / sizeof *names; q++)
    for (unsigned c = 0; c < sizeof casts / sizeof *casts; c++)
      for (unsigned member = 0; member < 9; member++)
        assert(lv_scan(&indexed, names[q], member, casts[c]) ==
               lv_scan(&walked, names[q], member, casts[c]));
  assert(lv_scan(&indexed, "EXITFRAME", 0, NULL) == &table[1]);
  assert(lv_scan(&indexed, "exitframe", 3, NULL) == &table[0]);
  assert(lv_scan(&indexed, "helper", 3, NULL) == &table[3]);
  assert(lv_scan(&indexed, "helper", 3, "Internal") == &table[4]);
  assert(lv_scan(&indexed, "helper", 0, NULL) == NULL);
  assert(lv_scan(&indexed, "new", 7, "internal") == &table[6]);
  assert(lv_scan(&indexed, "new", 0, "Internal") == NULL);
}
// Symbols are ids: equal spellings intern to one id whatever their case,
// the text reads back, and property keys compare by id (or by text against
// a string key) without touching the heap.
static void test_symbols(void) {
  reset();
  lv_t a = lv_text(&runtime, "Alpha", true), b = lv_text(&runtime, "alpha", true);
  lv_t c = lv_text(&runtime, "beta", true);
  assert(lv_type(a) == LV_SYMBOL && lv_id(a) == lv_id(b) && lv_id(a) != lv_id(c));
  assert(!strcmp(lv_cstr(&runtime, a), "Alpha") && lv_count(&runtime, a) == 5);
  assert(lv_symbol_id(&runtime, "ALPHA") == (unsigned)lv_id(a));
  assert(runtime.heap_used == 0); // no symbol lives in the value heap
  lv_t list = lv_literal(&runtime, "[#alpha: 1, \"beta\": 2]");
  assert(lv_number(&runtime, lv_get(&runtime, NULL, "alpha", list)) == 1);
  assert(lv_number(&runtime, lv_get(&runtime, NULL, "Beta", list)) == 2);
  assert(lv_number(&runtime, lv_index_get(&runtime, list, c)) == 2);
  assert(lv_type(lv_get(&runtime, NULL, "gamma", list)) == LV_VOID);
  lv_set(&runtime, NULL, "gamma", list, lv_num(3));
  assert(lv_type(lv_at(&runtime, list, 5)) == LV_SYMBOL && lv_number(&runtime, lv_at(&runtime, list, 6)) == 3);
  assert(lv_truth(&runtime, lv_binary(&runtime, "=", a, b)));
  assert(!lv_truth(&runtime, lv_binary(&runtime, "=", a, c)));
  char text[32];
  assert(lv_format(&runtime, list, text, sizeof text) && !strcmp(text, "[#Alpha:1,\"beta\":2,#gamma:3]"));
  // The resident table is bounded, and exhaustion is a failure, not aliasing.
  for (unsigned i = 0; i < LV_DYNAMIC_SYMBOLS + 8 && !runtime.failed; i++) {
    char name[16];
    snprintf(name, sizeof name, "s%u", i);
    lv_text(&runtime, name, true);
  }
  assert(runtime.failed && strstr(runtime.error, "symbol table exhausted"));
  reset();
  assert(!runtime.failed);
}
int main(void) {
  test_handler_index();
  test_symbols();
  // Names that arrive as strings fold case into the sorted vocabulary.
  assert(lb_name_id("mouseh") == LN_MOUSEH && lb_name_id("MouseH") == LN_MOUSEH);
  assert(lb_name_id("ticks") == LN_TICKS && lb_name_id("notaname") == LB_NAME_COUNT);
  assert(lb_name_id("") == LB_NAME_COUNT && lb_name_id("_root") == LN__ROOT);
  reset();
  assert((uintptr_t)runtime.heap % _Alignof(lv_t) == 0);
  lv_t a = lv_num(7), b = lv_num(2);
  assert(lv_number(&runtime, lv_binary(&runtime, "/", a, b)) == 3);
  b = lv_float(lv_numeric(b));
  assert(lv_number(&runtime, lv_binary(&runtime, "/", a, b)) == 3.5);
  lv_t n = lv_num(-1.5);
  assert(lv_number(&runtime, call("integer", 1, &n)) == -2);
  lv_t f = call("float", 1, &a);
  assert(!lv_truth(&runtime, call("integerp", 1, &f)));
  const char *literal = "[7, 0, [1, 2], \"hello\", [#key: 3]]";
  a = lv_literal(&runtime, literal);
  runtime.globals[0] = a;
  assert(lv_count(&runtime, a) == 5);
  assert(lv_count(&runtime, lv_at(&runtime, a, 5)) == 1);
  lv_t copy = lv_literal(&runtime, literal);
  runtime.globals[1] = copy;
  assert(lv_truth(&runtime, lv_binary(&runtime, "=", a, copy)));
  for (unsigned i = 0; i < 10000; i++) {
    lv_t args[] = {a, lv_num(2), lv_num(i)};
    call("setat", 3, args);
    (void)lv_text(&runtime, "garbage", false);
    if (!(i % 31))
      lv_collect(&runtime);
    assert(lv_number(&runtime, lv_at(&runtime, a, 2)) == i);
  }
  assert(!lv_truth(&runtime, lv_binary(&runtime, "=", a, copy)));
  assert(runtime.heap_high_water < 10000);
  a = lv_text(&runtime, "abc", false);
  b = lv_chunk(&runtime, "char", a, lv_num(9999), lv_num(10000));
  assert(!strcmp(lv_cstr(&runtime, b), ""));
  b = lv_text(&runtime, "DEF", false);
  lv_t joined = lv_binary(&runtime, "&&", a, b);
  assert(!strcmp(lv_cstr(&runtime, joined), "abc DEF"));
  assert(lv_truth(&runtime, lv_binary(&runtime, "contains", joined,
                                      lv_text(&runtime, "C d", false))));
  assert(lv_type(lv_literal(&runtime, "runSomething()")) == LV_VOID);
  assert(lv_type(lv_literal(&runtime, "[1,broken]")) == LV_VOID);
  assert(lv_type(lv_literal(&runtime, "1e999")) == LV_VOID);
  assert(!runtime.failed);
  reset();
  a = lv_literal(&runtime, "[1,2,[3,4]]");
  b = lv_binary(&runtime, "+", a, lv_num(10));
  char text[100];
  assert(lv_format(&runtime, b, text, sizeof(text)));
  assert(!strcmp(text, "[11,12,[13,14]]"));
  assert(lv_number(&runtime, lv_at(&runtime, a, 1)) == 1);
  b = lv_binary(&runtime, "-", lv_literal(&runtime, "[5,6,7]"),
                lv_literal(&runtime, "[1,2]"));
  assert(lv_format(&runtime, b, text, sizeof(text)) && !strcmp(text, "[4,4]"));
  // Property-list arithmetic keeps the left operand's keys and maps values;
  // on the right the list contributes only values (junk-pile part counting).
  a = lv_literal(&runtime, "[#p1: 1, #p2: [2,3]]");
  b = lv_binary(&runtime, "+", a, lv_num(1));
  assert(lv_format(&runtime, b, text, sizeof(text)));
  assert(!strcmp(text, "[#p1:2,#p2:[3,4]]"));
  b = lv_binary(&runtime, "+", lv_num(10), a);
  assert(lv_format(&runtime, b, text, sizeof(text)));
  assert(!strcmp(text, "[11,[12,13]]"));
  assert(!runtime.failed);
  // inside() with an uninitialized rect returns FALSE instead of raising.
  {
    lv_t inside_args[2] = {lv_point(&runtime, 5, 5),
                           lv_text(&runtime, "", false)};
    b = call("inside", 2, inside_args);
    assert(lv_number(&runtime, b) == 0);
    inside_args[1] = lv_point(&runtime, 1, 2);
    b = call("inside", 2, inside_args);
    assert(lv_number(&runtime, b) == 0);
    inside_args[1] = lv_rect(&runtime, 0, 0, 10, 10);
    b = call("inside", 2, inside_args);
    assert(lv_number(&runtime, b) == 1);
  }
  reset();
  a = lv_list(&runtime, 0, NULL, false);
  runtime.globals[0] = a;
  assert(lv_set_at(&runtime, a, 1, a));
  lv_collect(&runtime);
  assert(lv_id(lv_at(&runtime, a, 1)) == lv_id(a));
  reset();
  (void)lv_integer(&runtime, lv_num(INFINITY));
  assert(runtime.failed);
  reset();
  // Lingo coercions: numeric text in arithmetic, numbers in string operators.
  a = lv_text(&runtime, "3", false);
  assert(lv_number(&runtime, a) == 3);
  b = lv_binary(&runtime, "+", a, lv_num(1));
  assert(!lv_is_float(b) && lv_number(&runtime, b) == 4);
  b = lv_binary(&runtime, "/", lv_text(&runtime, "10", false),
                lv_text(&runtime, " 4 ", false));
  assert(!lv_is_float(b) && lv_number(&runtime, b) == 2);
  b = lv_binary(&runtime, "/", lv_text(&runtime, "2.5", false), lv_num(2));
  assert(lv_is_float(b) && lv_number(&runtime, b) == 1.25);
  assert(lv_truth(&runtime, lv_binary(&runtime, "contains", lv_num(123),
                                      lv_text(&runtime, "2", false))));
  assert(!lv_truth(&runtime, lv_binary(&runtime, "starts", lv_num(123),
                                       lv_text(&runtime, "2", false))));
  a = lv_point(&runtime, 3, 4);
  b = call("float", 1, &a);
  assert(lv_type(b) == LV_LIST && lv_count(&runtime, b) == 2);
  assert(lv_is_float(lv_at(&runtime, b, 1)));
  assert(lv_number(&runtime, lv_at(&runtime, b, 2)) == 4);
  assert(runtime.objects[lv_id(b)].geometry == 2);
  assert(!runtime.failed);
  // Script-class failures recover like the original alert-and-continue.
  (void)lv_number(&runtime, lv_text(&runtime, "abc", false));
  assert(runtime.failed && runtime.script_error);
  assert(lv_recover_script(&runtime));
  assert(!runtime.failed && runtime.script_error_count == 1);
  assert(strstr(runtime.last_script_error, "non-numeric"));
  (void)lv_at(&runtime, (lv_t){0}, 1);
  assert(runtime.failed && runtime.script_error);
  assert(lv_recover_script(&runtime) && runtime.script_error_count == 2);
  (void)lv_integer(&runtime, lv_num(INFINITY));
  assert(runtime.failed && runtime.script_error && lv_recover_script(&runtime));
  // value() reads the calling handler's locals first (Christmas recipes).
  lv_init(&runtime, (lv_services_t){0}, NULL, globals, 2, 42);
  assert(lv_start(&runtime, &named_movie, "named", 0, 0, NULL));
  runtime.frames[0].locals[0] = lv_literal(&runtime, "[1, 2]");
  runtime.globals[0] = lv_num(9);
  a = lv_text(&runtime, "recept3", false);
  b = call("value", 1, &a);
  assert(lv_type(b) == LV_LIST && lv_count(&runtime, b) == 2);
  a = lv_text(&runtime, "root", false);
  assert(lv_number(&runtime, call("value", 1, &a)) == 9);
  a = lv_text(&runtime, "[3, 4]", false);
  assert(lv_count(&runtime, call("value", 1, &a)) == 2);
  // Lists compare element by element; a list against a number is false from D6.
  reset();
  assert(lv_truth(&runtime, lv_binary(&runtime, "<=", lv_point(&runtime, 5, 5),
                                      lv_point(&runtime, 10, 10))));
  assert(!lv_truth(&runtime, lv_binary(&runtime, "<=", lv_point(&runtime, 5, 20),
                                       lv_point(&runtime, 10, 10))));
  assert(lv_truth(&runtime, lv_binary(&runtime, "<", lv_literal(&runtime, "[1, 2]"),
                                      lv_literal(&runtime, "[2, 3, 0]"))));
  assert(!lv_truth(&runtime, lv_binary(&runtime, "<", lv_point(&runtime, 1, 1), lv_num(5))));
  assert(!runtime.failed);
  // A numeric symbol finds the integer key (Mucklas card offsets).
  a = lv_literal(&runtime, "[3: [60, 0, 3], 4: [61, 0, 3]]");
  b = lv_index_get(&runtime, a, lv_text(&runtime, "4", true));
  assert(lv_type(b) == LV_LIST && lv_number(&runtime, lv_at(&runtime, b, 1)) == 61);
  a = lv_literal(&runtime, "[#a: 1]");
  assert(lv_number(&runtime, lv_index_get(&runtime, a, lv_text(&runtime, "a", true))) == 1);
  assert(!runtime.failed);
  // Authored recursion goes far deeper than the widest handler would allow
  // if every frame reserved LV_LOCALS, and running out stays recoverable.
  recurse(200);
  assert(!runtime.failed && !runtime.depth && deepest == 201);
  recurse(100000);
  assert(runtime.failed && runtime.script_error);
  assert(strstr(runtime.error, "native call stack exhausted"));
  assert(deepest == LV_FRAMES && lv_recover_script(&runtime));
  puts("native value contracts: PASS");
}
