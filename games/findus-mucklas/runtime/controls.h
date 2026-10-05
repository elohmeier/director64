#ifndef MUCKLAS_CONTROLS_H
#define MUCKLAS_CONTROLS_H
#include "director.h"
#include "input.h"
bool mucklas_name_target(dg_runtime_t *, unsigned sprite);
void mucklas_pad_keys(dg_runtime_t *, const input_sample_t *);
unsigned mucklas_key_code(unsigned character);
#endif
