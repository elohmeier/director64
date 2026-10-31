// Independent synthetic original-projector observations, 2026-10-31.
// Wine 8.0, PF3.EXE 9.0r383 / PROJ.DLL 9.0r371; verified loaded
// DIRAPI.DLL and IML32.DLL 8.5.1r104, authored Director 8.5 media.
// Reproduce with tools/reference/list-relations.lingo. ScummVM 41ac2b3's
// compareArrays disagrees here; these expectations come from the original.
#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t runtime;
static const char *const inputs[] = {
    "[]", "[1]", "[1,2,3]", "[0,1]", "[-1,1]", "[2,3]", "[:]", "[#a:1,#b:2]"};
static const unsigned char observed[][9] = {
    {0,0,1,1,0,0,0,0,0},
    {1,0,1,0,0,1,0,1,0},
    {1,0,1,0,0,1,0,1,0},
    {1,0,1,0,0,1,0,1,0},
    {0,1,0,1,1,0,0,1,0},
    {1,0,1,0,0,1,0,1,0},
    {0,0,1,1,0,0,0,0,0},
    {1,0,1,0,0,1,0,1,0},
};

static void check(const char *op, lv_t a, lv_t b, unsigned expected) {
  lv_t result = lv_binary(&runtime, op, a, b);
  if (runtime.failed || lv_number(&runtime, result) != expected) {
    char left[128], right[128];
    assert(lv_format(&runtime, a, left, sizeof(left)));
    assert(lv_format(&runtime, b, right, sizeof(right)));
    fprintf(stderr, "%s %s %s: port %.0f, original %u; %s\n",
            left, op, right, lv_number(&runtime, result), expected, runtime.error);
  }
  assert(!runtime.failed && lv_number(&runtime, result) == expected);
}

int main(void) {
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(*inputs); i++) {
    lv_init(&runtime, (lv_services_t){0}, NULL, NULL, 0, 1);
    lv_t list = lv_literal(&runtime, inputs[i]);
    check(">", list, lv_num(0), observed[i][0]);
    check("<", list, lv_num(0), observed[i][1]);
    check(">=", list, lv_num(0), observed[i][2]);
    check("<=", list, lv_num(0), observed[i][3]);
    check(">", lv_num(0), list, observed[i][4]);
    check("<", lv_num(0), list, observed[i][5]);
    check(">", list, lv_num(3), observed[i][6]);
    check("<", list, lv_num(3), observed[i][7]);
    check("=", list, lv_num(0), observed[i][8]);
  }
  check(">", lv_literal(&runtime, "[1,2]"), lv_literal(&runtime, "[0,1]"), 1);
  check(">", lv_literal(&runtime, "[1,2]"), lv_literal(&runtime, "[0,3]"), 1);
  check(">", lv_literal(&runtime, "[]"), lv_literal(&runtime, "[]"), 0);
  // Independently observed follow-up from list-relations-extra.lingo.
  static const struct {
    const char *left, *right;
    unsigned char result[5]; // >, <, >=, <=, =
  } extra[] = {
      {"[7]", "3", {1,0,1,0,0}},
      {"[0,-1]", "0", {0,1,0,1,0}},
      {"[0,0]", "0", {0,0,1,1,0}},
      {"[1,2]", "1", {1,0,1,0,0}},
      {"[1,-1]", "1", {0,1,0,1,0}},
      {"[1,2]", "[1,2,99]", {0,1,0,1,0}},
      {"[1,2,99]", "[1,2]", {1,0,1,0,0}},
      {"[]", "[99]", {0,1,0,1,0}},
      {"[99]", "[]", {1,0,1,0,0}},
      {"[#z:-1,#a:7]", "0", {0,1,0,1,0}},
      {"0", "[#z:-1,#a:7]", {1,0,1,0,0}},
      {"[#z:1,#a:2]", "[#x:0,#y:99]", {1,0,1,0,0}},
      {"[[1,2],0]", "[[0,99],9]", {1,0,1,0,0}},
      {"[[0,1]]", "0", {1,0,1,0,0}},
      {"[[0,-1]]", "0", {0,1,0,1,0}},
      {"[[]]", "0", {0,0,1,1,0}},
  };
  static const char *const operators[] = {">", "<", ">=", "<=", "="};
  for (unsigned i = 0; i < sizeof(extra) / sizeof(*extra); i++) {
    lv_init(&runtime, (lv_services_t){0}, NULL, NULL, 0, 1);
    lv_t left = lv_literal(&runtime, extra[i].left), right = lv_literal(&runtime, extra[i].right);
    for (unsigned op = 0; op < sizeof(operators) / sizeof(*operators); op++)
      check(operators[op], left, right, extra[i].result[op]);
  }
  lv_init(&runtime, (lv_services_t){0}, NULL, NULL, 0, 1);
  lv_t cycle = lv_literal(&runtime, "[0]");
  lv_set_at(&runtime, cycle, 1, cycle);
  (void)lv_binary(&runtime, ">", cycle, lv_num(0));
  assert(runtime.failed && runtime.script_error);
  assert(strstr(runtime.error, "recursive list comparison"));
  puts("Director 8.5 original-projector list ordering observations passed");
}
