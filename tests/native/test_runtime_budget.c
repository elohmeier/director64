#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
#if defined(EXPECT_OVERRIDE)
  assert(LV_HEAP_BYTES == 1572864 && LV_OBJECTS == 10000);
#else
  assert(LV_HEAP_BYTES == 4194304 && LV_OBJECTS == 32768);
#endif
  puts("Director 8.5 memory budgets and their overrides hold");
}
