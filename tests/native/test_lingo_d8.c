#include "lingo_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Host contracts corroborated against ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc:
// lingo-code.cpp LC::call/chunkRef/c_delete and lingo-builtins.cpp list services.
// These are not an original-projector or N64 hardware comparison.
static lv_runtime_t runtime;
static unsigned xtra_constructors;
static const char *const names[] = {"saved", "other"};
static const char *const properties[] = {"position", "counter"};
static const char *const method_names[] = {"me", "amount"};
static lv_flow_t idle(lv_runtime_t *r, lv_frame_t *f) {
  (void)r; (void)f;
  return LV_RETURN;
}
static lv_flow_t construct(lv_runtime_t *r, lv_frame_t *f) {
  assert(lv_id(f->self) == lv_id(f->locals[0]));
  lv_self_set(r, f, "counter", f->locals[1]);
  if (lv_numeric(f->locals[1]) == 99) {
    lv_t xtra = lv_make(LV_XTRA, 1);
    lv_t file = lv_call(r, f, "new", 1, &xtra);
    lv_self_set(r, f, "position", file);
  }
  r->result = f->self;
  return LV_RETURN;
}
static lv_flow_t delayed(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc++) return LV_YIELD;
  lv_t counter = lv_self_get(r, f, "counter");
  lv_self_set(r, f, "counter", lv_binary(r, "+", counter, f->locals[1]));
  r->result = lv_self_get(r, f, "counter");
  return LV_RETURN;
}
static lv_flow_t singleton_step(lv_runtime_t *r, lv_frame_t *f) {
  lv_self_set(r, f, "counter", lv_binary(r, "+", lv_self_get(r, f, "counter"), lv_num(1)));
  r->result = lv_self_get(r, f, "counter");
  return LV_RETURN;
}
static lv_flow_t cancel(lv_runtime_t *r, lv_frame_t *f) {
  lv_invoke(r, f, "abort", 0, NULL);
  return LV_RETURN;
}
static const lv_handler_t handlers[] = {
  {.name="test", .member=1, .cast="Internal", .kind="MovieScript", .step=idle},
  {.name="new", .member=2, .cast="Internal", .kind="MovieScript", .arguments=2,
   .locals=2, .step=construct, .local_names=method_names, .property_count=2, .property_names=properties},
  {.name="increment", .member=2, .cast="Internal", .kind="MovieScript", .arguments=2,
   .locals=2, .step=delayed, .local_names=method_names, .property_count=2, .property_names=properties},
  {.name="singleton", .member=2, .cast="Internal", .kind="MovieScript",
   .step=singleton_step, .property_count=2, .property_names=properties},
  {.name="cancel", .member=2, .cast="Internal", .kind="MovieScript", .step=cancel},
};
static const lv_movie_t movie = {"D8-fixture", 5, handlers,NULL, NULL, NULL, NULL, 0};
static bool xtra_call(lv_runtime_t *r, const char *name, unsigned argc,
                       const lv_t *args, lv_t *result, bool *yield) {
  (void)r; (void)yield;
  if (!strcmp(name, "sound") && argc == 1) {
    *result = lv_make(LV_SOUND, lv_integer(r, args[0]));
    return true;
  }
  if (strcmp(name, "new") || argc != 1 || lv_type(args[0]) != LV_XTRA) return false;
  xtra_constructors++;
  *result = lv_make(LV_FILE, 2);
  return true;
}
static lv_t script(lv_runtime_t *r, const lv_movie_t *m, const lv_handler_t *h) {
  (void)r; (void)m;
  return lv_make(LV_SCRIPT, h->member);
}
static const lv_handler_t *resolve(lv_runtime_t *r, lv_t owner,
                                    const char *name, const lv_movie_t **out) {
  (void)r;
  *out = &movie;
  return lv_find(NULL, &movie, name, (unsigned)lv_id(owner));
}
static void reset(void) {
  lv_init(&runtime, (lv_services_t){.script=script, .resolve=resolve, .call=xtra_call}, NULL, names, 2, 1);
  xtra_constructors = 0;
  runtime.current = &movie;
  assert(lv_start(&runtime, &movie, "test", 1, 0, NULL));
}
static lv_t call(const char *name, unsigned argc, const lv_t *args) {
  lv_t value = lv_call(&runtime, &runtime.frames[0], name, argc, args);
  if (runtime.failed) fprintf(stderr, "%s\n", runtime.error);
  assert(!runtime.failed);
  return value;
}
// Through the site cache, as a bytecode call site with a pooled name goes.
static lv_t call_pooled(const char *name, unsigned argc, const lv_t *args) {
  lv_t value = lv_call_pooled(&runtime, &runtime.frames[0], name, argc, args);
  if (runtime.failed) fprintf(stderr, "%s\n", runtime.error);
  assert(!runtime.failed);
  return value;
}
static void text_is(lv_t value, const char *text) {
  assert(!strcmp(lv_cstr(&runtime, value), text));
  assert(!runtime.failed);
}
static void collected_handle_reuse(void) {
  reset();
  // Fill the table, keeping a low and a near-capacity handle rooted. GC must
  // reclaim every hole while preserving the identities of surviving values.
  const unsigned low = 137, high = LV_OBJECTS - 2;
  for (unsigned i = 0; i < LV_OBJECTS; i++) {
    lv_t value = lv_text(&runtime, i == low ? "low" : "high", false);
    assert(!runtime.failed && (unsigned)lv_id(value) == i);
    if (i == low) runtime.globals[0] = value;
    if (i == high) runtime.globals[1] = value;
  }
  lv_collect(&runtime);
  text_is(runtime.globals[0], "low");
  text_is(runtime.globals[1], "high");
  lv_collect(&runtime); // Repeated collections must retain already free holes.
  for (unsigned i = 0; i < LV_OBJECTS; i++) {
    if (i == low || i == high) continue;
    lv_t value = lv_text(&runtime, "replacement", false);
    assert(!runtime.failed && (unsigned)lv_id(value) == i);
  }
  runtime.globals[1] = (lv_t){0};
  lv_collect(&runtime);
  text_is(runtime.globals[0], "low");
  assert(lv_id(lv_text(&runtime, "reused", false)) == 0);
  runtime.globals[0] = (lv_t){0};
  lv_collect(&runtime);
  assert(!runtime.failed && runtime.heap_used == 0);
  assert(lv_id(lv_text(&runtime, "empty collection", false)) == 0);
}
static lv_flow_t shadow_count(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  r->result = lv_num(99);
  return LV_RETURN;
}
static void compaction_with_reordered_handles(void) {
  reset();
  // Reuse alternating low handles behind surviving high handles. Variable-size
  // values exercise overlapping moves; a handle-order copy destroys live data.
  lv_t empty[512] = {0};
  runtime.globals[0] = lv_list(&runtime, 512, empty, false);
  unsigned generation[512] = {0};
  for (unsigned round = 1; round <= 12; round++) {
    for (unsigned i = 0; i < 512; i++)
      if (round == 1 || (i + round) % 3 == 0)
        lv_set_at(&runtime, runtime.globals[0], i + 1, (lv_t){0});
    lv_collect(&runtime);
    for (unsigned i = 0; i < 512; i++) {
      if (lv_type(lv_at(&runtime, runtime.globals[0], i + 1))) continue;
      char text[80];
      generation[i] = round;
      snprintf(text, sizeof(text), "%u/%u/%0*u", i, round, (int)(i % 40), i);
      lv_set_at(&runtime, runtime.globals[0], i + 1, lv_text(&runtime, text, false));
    }
    lv_collect(&runtime);
    for (unsigned i = 0; i < 512; i++) {
      char text[80];
      snprintf(text, sizeof(text), "%u/%u/%0*u", i, generation[i], (int)(i % 40), i);
      text_is(lv_at(&runtime, runtime.globals[0], i + 1), text);
    }
    assert(!runtime.failed);
  }
}
static void colliding_calls(void) {
  reset();
  lv_t items[] = {lv_num(7), lv_num(8)}, channel = lv_num(2);
  lv_t list = lv_list(&runtime, 2, items, false);
  runtime.globals[0] = list;
  // The source's sound checker alternates these negative lookups from one
  // site each. Both stay cached, as negative entries of their own sites,
  // while their language/platform results stay distinct.
  static const char count_name[] = "count", sound_name[] = "sound";
  for (unsigned i = 0; i < 1000; i++) {
    assert(lv_numeric(call_pooled(count_name, 1, &list)) == 2);
    lv_t sound = call_pooled(sound_name, 1, &channel);
    assert(lv_type(sound) == LV_SOUND && lv_id(sound) == 2);
  }
  unsigned cached = 0;
  for (unsigned i = 0; i < LV_CALL_SLOTS; i++) {
    lv_call_cache_t *entry = &runtime.call_cache[i];
    if (entry->valid && entry->caller == runtime.frames[0].handler && !entry->handler &&
        (entry->name == count_name || entry->name == sound_name)) cached++;
  }
  assert(cached == 2);
  // Loading a shared movie can turn a previously missing handler into a source
  // override. Use the same cache invalidation as Director overlay replacement.
  const lv_handler_t handler = {.name="count", .member=1, .cast="Shared",
                                .kind="MovieScript", .step=shadow_count};
  const lv_movie_t shared = {"shared-fixture", 1, &handler,NULL, NULL, NULL, NULL, 0};
  runtime.shared[0] = &shared;
  runtime.shared_count = 1;
  runtime.call_generation++;
  assert(lv_numeric(call_pooled(count_name, 1, &list)) == 99);
  assert(lv_type(call_pooled(sound_name, 1, &channel)) == LV_SOUND);
  runtime.shared_count = 0;
  runtime.call_generation++;
  assert(lv_numeric(call_pooled(count_name, 1, &list)) == 2);
}
static void missing_method_lookup(void) {
  reset();
  lv_t child = lv_instance(&runtime, lv_make(LV_SCRIPT, 2));
  runtime.globals[0] = child;
  unsigned used = runtime.heap_used;
  for (unsigned i = 0; i < 1000; i++)
    assert(!lv_start_method(&runtime, child, "missing", 0, NULL));
  assert(!runtime.failed && runtime.heap_used == used);
  lv_t parent = lv_instance(&runtime, lv_make(LV_SCRIPT, 1));
  lv_set(&runtime, NULL, "ancestor", child, parent);
  used = runtime.heap_used;
  for (unsigned i = 0; i < 1000; i++)
    assert(!lv_start_method(&runtime, child, "missing", 0, NULL));
  assert(!runtime.failed && runtime.heap_used == used);
  assert(lv_start_method(&runtime, child, "test", 0, NULL));
  assert(runtime.frames[runtime.depth - 1].handler->member == 1);
  assert(lv_id(runtime.frames[runtime.depth - 1].self) == lv_id(parent));
  assert(lv_id(runtime.frames[runtime.depth - 1].locals[0]) == lv_id(child));
  assert(lv_run(&runtime, 4) && !runtime.depth);
}
static void random_and_keyed_spacing(void) {
  // FA:raknaUtVag can pass zero after abs(distance - 40). D8 accepts that
  // bound, truncates fractional arguments, and caps the random range at 65535.
  const double bounds[] = {0, -1, 0.5, -0.5, 1, 1.9, 40, 65535, 65536, 1e100};
  for (unsigned i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
    reset();
    lv_t arg = lv_num(bounds[i]);
    unsigned maximum = bounds[i] < 1 || bounds[i] > 65535 ? 65535 : (unsigned)bounds[i];
    for (unsigned j = 0; j < 100; j++) {
      lv_t value = call("random", 1, &arg);
      assert(!runtime.failed && lv_numeric(value) >= 1 && lv_numeric(value) <= maximum);
    }
  }
  const double invalid[] = {NAN, INFINITY, -INFINITY};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    reset();
    lv_t arg = lv_num(invalid[i]);
    lv_call(&runtime, &runtime.frames[0], "random", 1, &arg);
    assert(runtime.failed && strstr(runtime.error, "invalid random bound"));
  }
  reset();
  lv_t spacing = lv_list(&runtime, 2,
      (lv_t[]){lv_text(&runtime, "br_50_0", true), lv_num(-19)}, true);
  runtime.globals[0] = spacing;
  lv_t lookup[] = {spacing, lv_text(&runtime, "br_50_0", true)};
  assert(lv_numeric(call("getaprop", 2, lookup)) == -19);
  lv_call(&runtime, &runtime.frames[0], "getat", 2, lookup);
  assert(runtime.failed); // The BR port correction must not broaden getAt.
}
int main(void) {
  missing_method_lookup();
  random_and_keyed_spacing();
  collected_handle_reuse();
  compaction_with_reordered_handles();
  colliding_calls();
  reset();
  lv_t prototype = lv_make(LV_SCRIPT, 2), amount = lv_num(7);
  lv_t first = lv_call_method(&runtime, &runtime.frames[0], "new", prototype, 1, &amount);
  runtime.globals[0] = first;
  lv_t second = lv_call_method(&runtime, &runtime.frames[0], "new", prototype, 1, &amount);
  runtime.globals[1] = second;
  assert(lv_type(first) == LV_INSTANCE && lv_id(first) != lv_id(second));
  assert(lv_start_method(&runtime, first, "increment", 1, &amount));
  assert(lv_run(&runtime, 1) && runtime.yielded);
  // Frame.self must keep a receiver alive even if the source has no remaining
  // variable containing it. Property graphs and temporary strings also survive.
  runtime.globals[0] = (lv_t){0};
  for (unsigned i = 0; i < 300; i++) (void)lv_text(&runtime, "garbage", false);
  lv_collect(&runtime);
  assert(lv_run(&runtime, 1));
  assert(lv_numeric(runtime.result) == 14);
  assert(lv_numeric(lv_get(&runtime, NULL, "counter", second)) == 7);
  assert(!lv_truth(&runtime, lv_binary(&runtime, "=", first, second)));
  runtime.globals[0] = first;
  lv_set(&runtime, NULL, "position", first, second);
  runtime.globals[1] = (lv_t){0};
  lv_collect(&runtime);
  assert(lv_id(lv_get(&runtime, NULL, "position", first)) == lv_id(second));
  for (unsigned i = 1; i <= 3; i++) {
    assert(lv_start(&runtime, &movie, "singleton", 2, 0, NULL));
    assert(lv_run(&runtime, 1));
    assert(lv_numeric(runtime.result) == i);
    lv_collect(&runtime);
  }
  assert(lv_numeric(lv_get(&runtime, NULL, "counter", first)) == 14);
  lv_t command = lv_text(&runtime, "the globals[#saved]=[7,8]", false);
  call("do", 1, &command);
  assert(lv_count(&runtime, runtime.globals[0]) == 2);
  assert(lv_start(&runtime, &movie, "singleton", 2, 0, NULL));
  command = lv_text(&runtime, "counter=the globals[#saved]", false);
  lv_call(&runtime, &runtime.frames[runtime.depth - 1], "do", 1, &command);
  assert(lv_count(&runtime, lv_self_get(&runtime, &runtime.frames[runtime.depth - 1], "counter")) == 2);
  command = lv_text(&runtime, "the globals[#other]=counter", false);
  lv_call(&runtime, &runtime.frames[runtime.depth - 1], "do", 1, &command);
  assert(lv_count(&runtime, runtime.globals[1]) == 2);

  reset();
  amount = lv_num(99);
  first = lv_call_method(&runtime, &runtime.frames[0], "new", prototype, 1, &amount);
  assert(!runtime.failed && xtra_constructors == 1);
  lv_t file = lv_get(&runtime, NULL, "position", first);
  assert(lv_type(file) == LV_FILE && lv_id(file) == 2);
  assert(lv_has_reference(&runtime, LV_FILE, 2)); // receiver in result
  runtime.result = (lv_t){0};
  assert(!lv_has_reference(&runtime, LV_FILE, 2)); // unreachable object graph
  runtime.globals[0] = first;
  runtime.atomic_depth++;
  assert(lv_has_reference(&runtime, LV_FILE, 2));
  assert(!lv_has_reference(&runtime, LV_FILE, 3));
  runtime.atomic_depth--;
  lv_t sound_list = lv_list(&runtime, 1, &first, false);
  lv_t sound_copy = call("duplicate", 1, &sound_list);
  assert(lv_id(sound_copy) != lv_id(sound_list));
  assert(lv_id(lv_at(&runtime, sound_copy, 1)) == lv_id(first));
  lv_set(&runtime, NULL, "counter", lv_at(&runtime, sound_copy, 1), lv_num(123));
  assert(lv_numeric(lv_get(&runtime, NULL, "counter", first)) == 123);
  unsigned heap_before = runtime.heap_used;
  unsigned allocations_before = runtime.allocations_since_gc;
  for (unsigned i = 0; i < 10000; i++) {
    lv_set(&runtime, NULL, "COUNTER", first, lv_num(i));
    assert(lv_numeric(lv_get(&runtime, NULL, "counter", first)) == i);
    assert(lv_type(lv_get(&runtime, NULL, "absent", first)) == LV_VOID);
  }
  assert(runtime.heap_used == heap_before);
  assert(runtime.allocations_since_gc == allocations_before);

  reset();
  lv_call_method(&runtime, &runtime.frames[0], "cancel", prototype, 0, NULL);
  assert(!runtime.failed && runtime.depth == 0 && runtime.aborted);
  assert(lv_start(&runtime, &movie, "test", 1, 0, NULL));
  assert(lv_run(&runtime, 1) && runtime.depth == 0 && !runtime.aborted);
  reset();
  call("cancel", 0, NULL);
  assert(runtime.depth == 0 && runtime.aborted);

  reset();
  // GENMODUL score/reward colors and the documented hexadecimal form.
  // https://www.manualslib.com/manual/389805/Macromedia-Director-Mx-Lingo-Dictionary.html?page=554
  const unsigned colors[] = {0x000000, 0x0071a3, 0x160043, 0x9f005b, 0xffffff};
  for (unsigned i = 0; i < sizeof(colors) / sizeof(*colors); i++) {
    lv_t channels[] = {lv_num(colors[i] >> 16), lv_num((colors[i] >> 8) & 255),
                      lv_num(colors[i] & 255)};
    lv_t color = call("rgb", 3, channels);
    assert(lv_type(color) == LV_COLOR && (unsigned)lv_id(color) == colors[i]);
    assert(lv_integer(&runtime, color) == (int32_t)colors[i]);
  }
  lv_t hex = lv_text(&runtime, "0033fF", false);
  lv_t color = call("rgb", 1, &hex);
  assert(lv_type(color) == LV_COLOR && lv_id(color) == 0x0033ff);

  reset();
  lv_t props = lv_literal(&runtime, "[#actor: 12, #other: 20]");
  text_is(lv_get(&runtime, NULL, "ilk", props), "proplist");
  lv_t args[3] = {props, lv_num(2), lv_num(25)};
  assert(lv_numeric(call("getat", 2, args)) == 20);
  assert(lv_numeric(lv_index_get(&runtime, props, lv_num(2))) == 20);
  lv_index_set(&runtime, props, lv_num(2), lv_num(22));
  assert(lv_numeric(lv_get(&runtime, NULL, "other", props)) == 22);
  call("setat", 3, args);
  assert(lv_numeric(call("getat", 2, args)) == 25);
  text_is(call("getpropat", 2, args), "other");
  lv_set(&runtime, NULL, "ACTOR", props, lv_num(13));
  assert(lv_numeric(lv_index_get(&runtime, props, lv_text(&runtime, "actor", true))) == 13);
  assert(lv_count(&runtime, props) == 2);
  lv_call_method(&runtime, &runtime.frames[0], "deleteat", props, 1, &args[1]);
  assert(lv_count(&runtime, props) == 1);
  lv_t keyed = lv_literal(&runtime, "[9: 100, 1: 200]");
  assert(lv_numeric(lv_index_get(&runtime, keyed, lv_num(1))) == 100);
  lv_t lookup[] = {keyed, lv_num(1)};
  assert(lv_numeric(call("getaprop", 2, lookup)) == 200);
  lv_t point_args[] = {lv_num(3), lv_num(4)};
  lv_t point = call("point", 2, point_args);
  assert(lv_numeric(lv_get(&runtime, NULL, "loch", point)) == 3);
  lv_set(&runtime, NULL, "locv", point, lv_num(8));
  assert(lv_numeric(lv_at(&runtime, point, 2)) == 8);
  lv_t shifted = lv_binary(&runtime, "+", point, lv_num(2));
  assert(lv_numeric(lv_get(&runtime, NULL, "loch", shifted)) == 5);
  text_is(lv_get(&runtime, NULL, "ilk", point), "point");
  text_is(lv_get(&runtime, NULL, "string", lv_num(12)), "12");
  lv_t symbol = lv_get(&runtime, NULL, "symbol", lv_text(&runtime, "universal", false));
  assert(lv_type(symbol) == LV_SYMBOL);
  text_is(symbol, "universal");
  assert(lv_numeric(lv_get(&runtime, NULL, "integerp", lv_num(12))) == 1);
  lv_t copy = call("duplicate", 1, &point);
  lv_set(&runtime, NULL, "loch", copy, lv_num(99));
  assert(lv_numeric(lv_get(&runtime, NULL, "loch", point)) == 3);
  lv_t cyclic = lv_list(&runtime, 0, NULL, false);
  lv_set_at(&runtime, cyclic, 1, cyclic);
  copy = call("duplicate", 1, &cyclic);
  assert(lv_id(copy) != lv_id(cyclic) && lv_id(lv_at(&runtime, copy, 1)) == lv_id(copy));

  reset();
  // Newline is a word separator; Director line chunks specifically use CR.
  lv_t words = lv_text(&runtime, "  one  two\n three ", false);
  assert(lv_chunk_count(&runtime, "word", words) == 3);
  text_is(lv_chunk(&runtime, "word", words, lv_num(2), lv_num(3)), "two\n three");
  text_is(lv_chunk(&runtime, "word", words, lv_num(-30000), lv_num(-30000)), "three");
  text_is(lv_chunk_delete(&runtime, "word", words, lv_num(2), lv_num(2)), "  one  three ");
  lv_t items = lv_text(&runtime, "one,two,,four", false);
  assert(lv_chunk_count(&runtime, "item", items) == 4);
  text_is(lv_chunk_delete(&runtime, "item", items, lv_num(1), lv_num(1)), "two,,four");
  text_is(lv_chunk_delete(&runtime, "item", items, lv_num(2), lv_num(2)), "one,,four");
  text_is(lv_chunk_delete(&runtime, "item", items, lv_num(-30000), lv_num(-30000)), "one,two,");
  text_is(lv_chunk_delete(&runtime, "item", items, lv_num(20), lv_num(20)), "one,two,,four");
  lv_set(&runtime, NULL, "itemdelimiter", (lv_t){0}, lv_text(&runtime, "|", false));
  items = lv_text(&runtime, "one|two|", false);
  assert(lv_chunk_count(&runtime, "item", items) == 3);
  text_is(lv_chunk(&runtime, "item", items, lv_num(-30000), lv_num(-30000)), "");
  text_is(lv_chunk_set(&runtime, "item", items, lv_num(2), lv_num(2), lv_text(&runtime, "2", false)), "one|2|");
  assert(lv_chunk_count(&runtime, "line", lv_text(&runtime, "one\rtwo\nthree", false)) == 2);

  reset();
  command = lv_text(&runtime, "the globals[saved]=[1, [#name: \"player\"]]", false);
  call("do", 1, &command);
  lv_t key = lv_text(&runtime, "saved", false);
  lv_t saved = call("value", 1, &key);
  assert(lv_type(saved) == LV_LIST && lv_count(&runtime, saved) == 2);
  call("nothing", 0, NULL);
  runtime.globals[1] = lv_instance(&runtime, prototype);
  call("clearglobals", 0, NULL);
  assert(lv_type(runtime.globals[0]) == LV_VOID);
  assert(lv_type(runtime.globals[1]) == LV_INSTANCE);
  command = lv_text(&runtime, "the globals[saved]=runSomething()", false);
  lv_call(&runtime, &runtime.frames[0], "do", 1, &command);
  assert(runtime.failed && strstr(runtime.error, "literal data"));
  puts("Director 8 native objects/chunks: PASS");
}
