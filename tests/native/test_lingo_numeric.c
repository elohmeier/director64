#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>

static lv_runtime_t runtime;

static lv_t convert(const char *name, lv_t value) {
  lv_t result = lv_call_builtin(&runtime, NULL, name, 1, &value);
  assert(!runtime.failed && !runtime.script_error_count);
  return result;
}

int main(void) {
  lv_init(&runtime, (lv_services_t){0}, NULL, NULL, 0, 42);
  lv_t properties = lv_list(&runtime, 0, NULL, true);
  lv_t missing = lv_index_get(&runtime, properties,
                             lv_text(&runtime, "optional", false));
  const lv_t absent[] = {(lv_t){0}, missing, lv_text(&runtime, "", false)};
  const char *names[] = {"integer", "float"};
  // Original Director 8.5.1r104: integer(VOID), float(VOID), and
  // integer(EMPTY) remain VOID. Missing properties must not alias integer 0.
  for (unsigned n = 0; n < sizeof(names) / sizeof(*names); n++) {
    for (unsigned i = 0; i < sizeof(absent) / sizeof(*absent); i++)
      assert(lv_type(convert(names[n], absent[i])) == LV_VOID);
    lv_t zero = convert(names[n], lv_num(0));
    assert(lv_type(zero) == LV_NUMBER && lv_numeric(zero) == 0);
    assert(lv_is_float(zero) == (n == 1));
    zero = convert(names[n], lv_text(&runtime, "0", false));
    assert(lv_type(zero) == LV_NUMBER && lv_numeric(zero) == 0);
    assert(lv_is_float(zero) == (n == 1));
  }
  assert(lv_numeric(convert("integer", lv_float(7.6))) == 8);
  assert(lv_numeric(convert("integer", lv_float(-7.6))) == -8);
  lv_t decimal = convert("float", lv_text(&runtime, "7.25", false));
  assert(lv_is_float(decimal) && lv_numeric(decimal) == 7.25);
  // Mucklas KR negates the point returned by its direction helper. Keep
  // geometric identity and allocate a new value rather than changing it.
  lv_t direction = lv_point(&runtime, 3, -4.5);
  lv_t reverse = lv_unary(&runtime, "-", direction);
  assert(!runtime.failed && runtime.objects[lv_id(reverse)].geometry == 2);
  assert(lv_numeric(lv_at(&runtime, reverse, 1)) == -3);
  assert(lv_numeric(lv_at(&runtime, reverse, 2)) == 4.5);
  assert(lv_numeric(lv_at(&runtime, direction, 1)) == 3);
  assert(lv_numeric(lv_at(&runtime, direction, 2)) == -4.5);
  lv_set_at(&runtime, reverse, 1, lv_num(99));
  assert(lv_numeric(lv_at(&runtime, direction, 1)) == 3);
  lv_t bounds = lv_rect(&runtime, -1, 2, -3, 4);
  lv_t flipped = lv_unary(&runtime, "-", bounds);
  assert(!runtime.failed && runtime.objects[lv_id(flipped)].geometry == 4);
  for (unsigned i = 1; i <= 4; i++)
    assert(lv_numeric(lv_at(&runtime, flipped, i)) == -lv_numeric(lv_at(&runtime, bounds, i)));
  puts("lingo numeric conversions: ok");
}
