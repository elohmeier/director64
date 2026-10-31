#ifndef DIRECTOR64_NATIVE_POINTER_PROBE_H
#define DIRECTOR64_NATIVE_POINTER_PROBE_H
#include "input.h"
#include <stdio.h>
static void native_pointer_state(const input_state_t *p) {
  printf(",\"pointer\":{\"x\":%d,\"y\":%d}",
         p->x / INPUT_ONE, p->y / INPUT_ONE);
}
#endif
