#ifndef DIRECTOR64_DIRECTOR_REPLAY_H
#define DIRECTOR64_DIRECTOR_REPLAY_H
#include "director.h"
#include "input.h"
const char *director_replay_init(lv_runtime_t *);
void director_replay_sample(input_sample_t *, dg_runtime_t *);
#endif
