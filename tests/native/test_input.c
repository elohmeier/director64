#include "input.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  input_state_t state;
  input_init(&state);
  assert(state.x == SCREEN_W / 2 * INPUT_ONE && state.y == SCREEN_H / 2 * INPUT_ONE);
  input_sample_t sample = {.connected = true, .buttons = INPUT_A, .stick_x = 8, .stick_y = -8};
  input_update(&state, &sample);
  assert(state.x == SCREEN_W / 2 * INPUT_ONE && state.y == SCREEN_H / 2 * INPUT_ONE);
  assert(state.held == INPUT_A && state.pressed == INPUT_A && !state.released);
  sample.stick_x = 80; sample.stick_y = 80;
  input_update(&state, &sample);
  assert(state.x > SCREEN_W / 2 * INPUT_ONE && state.y < SCREEN_H / 2 * INPUT_ONE);
  assert(state.held == INPUT_A && !state.pressed && !state.released);
  sample.connected = false;
  int32_t x = state.x, y = state.y;
  input_update(&state, &sample);
  assert(!state.connected && !state.held && state.released == INPUT_A);
  assert(state.x == x && state.y == y);
  input_update(&state, &sample);
  assert(!state.released);
  // A reconnect keeps the one pointer at its previous position.
  sample = (input_sample_t){.connected = true, .buttons = INPUT_RIGHT | INPUT_DOWN};
  input_update(&state, &sample);
  assert(state.x == x + 5 * INPUT_ONE && state.y == y + 5 * INPUT_ONE);
  // Absolute recordings override stick/D-pad movement and retain button edges.
  sample.pointer_absolute = true;
  sample.pointer_x = 56 * INPUT_ONE; sample.pointer_y = 340 * INPUT_ONE;
  sample.buttons |= INPUT_A;
  input_update(&state, &sample);
  assert(state.x == sample.pointer_x && state.y == sample.pointer_y);
  assert(state.pressed == INPUT_A);
  sample.pointer_x = INT32_MIN; sample.pointer_y = INT32_MAX;
  input_update(&state, &sample);
  assert(state.x == 8 * INPUT_ONE && state.y == (SCREEN_H - 10) * INPUT_ONE);
  sample.pointer_absolute = false;
  sample.buttons = INPUT_LEFT | INPUT_DOWN; sample.stick_x = -128; sample.stick_y = -128;
  for (unsigned i = 0; i < 500; i++) input_update(&state, &sample);
  assert(state.x == 8 * INPUT_ONE && state.y == (SCREEN_H - 10) * INPUT_ONE);
  puts("single input: edges, disconnect/reconnect, absolute recordings and bounded motion passed");
}
