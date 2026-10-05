// Compaction has to visit live objects in heap-offset order, or a memmove
// overwrites one that has not moved yet and the heap is silently wrong. Two
// mechanisms produce that order: an allocation chain threaded in heap order
// (LV_ALLOCATION_CHAIN), and a handle array sorted at collection time for the
// profiles without one. This runs at both settings, over live sets churned
// until handle order and heap order have come apart -- which is the state
// that tells them apart. Breaking either the chain's append order or the
// sort makes it abort.
//
// Everything here is checked through the values themselves. A wrong order
// does not fail an assertion inside the collector; it corrupts strings.
#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t runtime;

// A distinct, checkable payload per slot, long enough to span several objects
// when it moves and to make an overlapping memmove visible.
static void text_for(unsigned slot, char *out, unsigned size) {
  snprintf(out, size, "slot-%06u-%.*s", slot, (int)(slot % 40),
           "padpadpadpadpadpadpadpadpadpadpadpadpadpad");
}

static void check_all(lv_t *values, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (lv_type(values[i]) == LV_VOID) continue;
    char expected[80];
    text_for(i, expected, sizeof(expected));
    assert(!strcmp(lv_cstr(&runtime, values[i]), expected));
  }
}

// Hold the live set in the root table so the collector can see it, and keep a
// parallel copy for checking. Roots move with compaction; the copy does not,
// so read values back out of the roots.
static void cycle(unsigned count, unsigned keep_every, unsigned churn) {
  assert(count < LV_ROOTS);
  for (unsigned i = 0; i < count; i++) {
    char text[80];
    text_for(i, text, sizeof(text));
    runtime.roots[i] = lv_text(&runtime, text, false);
    assert(!runtime.failed);
  }
  check_all(runtime.roots, count);

  // Drop most of the set, allocate over the holes, then restore the dropped
  // slots. The survivors keep their old offsets while the new objects take
  // handles below them, which is exactly how handle order and heap order come
  // apart between collections.
  for (unsigned round = 0; round < churn; round++) {
    for (unsigned i = 0; i < count; i++)
      if (i % keep_every) runtime.roots[i] = (lv_t){0};
    lv_collect(&runtime);
    check_all(runtime.roots, count);
    for (unsigned i = 0; i < count; i++)
      if (lv_type(runtime.roots[i]) == LV_VOID) {
        char text[80];
        text_for(i, text, sizeof(text));
        runtime.roots[i] = lv_text(&runtime, text, false);
        assert(!runtime.failed);
      }
    lv_collect(&runtime);
    check_all(runtime.roots, count);
  }
  memset(runtime.roots, 0, sizeof(runtime.roots));
  lv_collect(&runtime);
}

int main(void) {
  static const char *const globals[] = {"g"};
  lv_init(&runtime, (lv_services_t){0}, NULL, globals, 1, 42);

  // The root table is the whole live set the collector can see, and the plain
  // D6 profile has 136 slots against extended D6's 256.
  const unsigned wide = LV_ROOTS - 8 > 240 ? 240u : (unsigned)LV_ROOTS - 8;

  // Lightly churned: one long-lived object in six, so compaction keeps most
  // of the order it produced last time.
  cycle(wide, 6, 4);
  assert(!runtime.failed);

  // Heavily churned: keeping every other object and refilling each round
  // interleaves generations until handle order says little about heap order.
  cycle(wide, 2, 12);
  assert(!runtime.failed);

  // Every third object retained, which leaves two generations interleaved.
  cycle(wide, 3, 6);
  assert(!runtime.failed);

  // The heap must come back to nothing once every root is cleared, which only
  // holds if compaction accounted for every live object exactly once.
  assert(runtime.heap_used == 0);
  assert(runtime.object_count == 0);
  puts("lingo collect: compaction order survives handle churn");
  return 0;
}
