#include "../../platforms/android/jni/host_input.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  host_pad_t pad = {0};
  unsigned pressed;
  assert(host_pad_update(&pad, true, 1, 0, 0, false));
  assert(host_pad_update(&pad, true, 0, 0, 0, false));
  assert(host_pad_sample(&pad, &pressed) == 1 && pressed == 1);
  assert(host_pad_sample(&pad, &pressed) == 0 && pressed == 0);
  // Distinct clicks survive a blocked renderer, in order, including a held
  // chord.
  unsigned values[] = {1, 3, 2, 0, 1, 0};
  for (unsigned i = 0; i < 6; i++)
    assert(host_pad_update(&pad, true, values[i], 0, 0, false));
  for (unsigned i = 0; i < 6; i++)
    assert(host_pad_sample(&pad, &pressed) == values[i]);
  host_pointer_t pointer = {0};
  assert(host_pointer_update(&pointer, (host_pointer_sample_t){10, 20, 1, true},
                             false));
  assert(host_pointer_update(&pointer, (host_pointer_sample_t){40, 50, 0, true},
                             false));
  host_pointer_sample_t sample = host_pointer_sample(&pointer, &pressed);
  assert(sample.x == 10 && sample.y == 20 && sample.buttons == 1 &&
         pressed == 1);
  sample = host_pointer_sample(&pointer, &pressed);
  assert(sample.x == 40 && sample.buttons == 0);
  // Held input belongs to the menu until its physical release; later presses
  // work.
  assert(host_pad_update(&pad, true, 1, 0, 0, true));
  assert(host_pointer_update(&pointer, (host_pointer_sample_t){40, 50, 1, true},
                             true));
  host_input_resume(&pad, 1, &pointer);
  assert(host_pad_sample(&pad, &pressed) == 0);
  assert(host_pointer_sample(&pointer, &pressed).buttons == 0);
  assert(host_pad_update(&pad, true, 0, 0, 0, false));
  assert(host_pad_update(&pad, true, 1, 0, 0, false));
  assert(host_pad_sample(&pad, &pressed) == 0);
  assert(host_pad_sample(&pad, &pressed) == 1 && pressed == 1);
  // Disconnect clears queued input. Queue limits are explicit, never overwrite.
  assert(host_pad_update(&pad, false, 0, 0, 0, false));
  assert(host_pad_sample(&pad, &pressed) == 0 && !pad.connected);
  for (unsigned i = 0; i < HOST_EDGES; i++)
    assert(host_pad_update(&pad, true, (i + 1) & 1, 0, 0, false));
  assert(!host_pad_update(&pad, true, 1, 0, 0, false));
  host_commands_t commands = {0};
  for (unsigned i = 0; i < HOST_COMMANDS; i++)
    assert(host_command_push(&commands,
                             (host_command_t){.kind = HOST_KEY, .code = i}));
  assert(!host_command_push(&commands, (host_command_t){0}));
  for (unsigned i = 0; i < HOST_COMMANDS; i++) {
    host_command_t c;
    assert(host_command_pop(&commands, &c) && c.code == i);
  }
  puts("Android input: ordered taps, coordinates, holds, disconnect and "
       "bounded commands pass");
}
