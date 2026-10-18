#include "lingo_runtime.h"
#if defined(DIRECTOR64_N64) && DIRECTOR64_N64
#include "director.h"
#include "memory_budget.h"
#endif
#include <assert.h>
#include <stdio.h>
int main(void) {
#if defined(EXPECT_OVERRIDE)
  assert(LV_HEAP_BYTES == 1572864 && LV_OBJECTS == 10000);
#elif defined(DIRECTOR64_N64) && DIRECTOR64_N64
  assert(LV_HEAP_BYTES == 1048576 && LV_OBJECTS == 8192);
  assert(sizeof(lv_runtime_t) < 2u * 1024u * 1024u);
#else
  assert(LV_HEAP_BYTES == 4194304 && LV_OBJECTS == 32768);
#endif
  puts("Director8.5 target and hosted memory budgets remain separate");
}
