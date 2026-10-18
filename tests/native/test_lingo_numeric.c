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
  puts("lingo numeric conversions: ok");
}
