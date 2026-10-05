#include "shape.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  int spans[4];
  assert(!dg_shape_spans(1, 20, 10, 3, 0, false, false, spans));
  assert(dg_shape_spans(1, 20, 10, 3, 1, false, false, spans) == 2);
  assert(spans[0] == 0 && spans[1] == 1 && spans[2] == 19 && spans[3] == 20);
  for (unsigned kind = 1; kind <= 4; kind++)
    for (int y = 0; y < 13; y++) {
      unsigned n = dg_shape_spans(kind, 21, 13, y, 2, true, false, spans);
      assert(n == 1 && spans[0] >= 0 && spans[0] < spans[1] && spans[1] <= 21);
    }
  assert(dg_shape_spans(3, 20, 20, 0, 1, true, false, spans) == 1);
  assert(spans[0] > 0 && spans[1] < 20);
  assert(dg_shape_spans(4, 20, 20, 0, 1, true, true, spans) == 1);
  assert(spans[0] > 17 && spans[1] == 20);
  puts("shape geometry: rectangle, round rectangle, oval, line OK");
}
