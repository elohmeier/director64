#include "willy_input.h"
#include <string.h>

bool willy_input(dg_runtime_t *d, const input_sample_t *sample) {
  unsigned buttons = sample->connected ? sample->buttons : 0;
  d->right_mouse_down = false;
  if (*d->notice) {
    unsigned dismiss = buttons & (INPUT_A | INPUT_B | INPUT_START);
    if (!dismiss) d->notice_armed = true;
    if (dismiss && d->notice_armed) {
      d->notice[0] = 0;
      d->notice_armed = false;
      d->await_release = true;
    }
    return false;
  }
  if (!d->await_release && d->movie &&
      !strcmp(d->movie->code->name, "05.DXR"))
    d->right_mouse_down = (buttons & INPUT_B) != 0;
  return true;
}
