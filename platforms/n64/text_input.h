#ifndef DIRECTOR64_TEXT_INPUT_H
#define DIRECTOR64_TEXT_INPUT_H
#include "director.h"
#include "input.h"
#include <libdragon.h>
bool text_input_active(void);
bool text_input_open(dg_runtime_t *, unsigned, uint16_t);
void text_input_update(dg_runtime_t *, const input_sample_t *);
void text_input_draw(unsigned, unsigned);
#endif
