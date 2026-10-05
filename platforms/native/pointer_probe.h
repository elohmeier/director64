#ifndef DIRECTOR64_NATIVE_POINTER_PROBE_H
#define DIRECTOR64_NATIVE_POINTER_PROBE_H
#include "pointer.h"
#include <stdio.h>
static void native_pointer_state(const pointer_control_t *p) {
  const pointer_player_t *q = pointer_driver(p);
  printf(",\"pointer\":{\"player\":%u,\"x\":%d,\"y\":%d}", p->active + 1,
         q->input.x / INPUT_ONE, q->input.y / INPUT_ONE);
}
#endif
