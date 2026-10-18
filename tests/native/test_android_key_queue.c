// Production host queue + production web keyboard adapter + real Director
// dispatch.
#include "../../platforms/android/jni/host_input.h"
#include "director.h"
#include <assert.h>
#include <stdio.h>
static dg_runtime_t director;
static lv_runtime_t values;
static bool booted;
#define EMSCRIPTEN_KEEPALIVE
#include "../../platforms/web/keyboard.inc"
static unsigned downs, ups;
static lv_flow_t keyboard(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  if (f->handler->name[3] == 'd')
    downs++;
  else
    ups++;
  return LV_RETURN;
}
int main(void) {
  static const lv_handler_t handlers[] = {
      {.name = "keydown", .kind = "MovieScript", .step = keyboard},
      {.name = "keyup", .kind = "MovieScript", .step = keyboard}};
  static const lv_movie_t code = {
      .name = "KEYS.DXR", .count = 2, .handlers = handlers};
  static const dg_movie_t movie = {.id = 1, .tempo = 999, .code = &code};
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  booted = true;
  host_commands_t queue = {0};
  // Replace a 20-character name: twenty deletes + twenty typed characters.
  for (unsigned i = 0; i < 40; i++)
    assert(host_command_push(&queue,
                             (host_command_t){.kind = HOST_TYPE,
                                              .code = 7,
                                              .character = i < 20 ? 8 : 'a'}));
  // OK immediately after the replacement must follow all 80 key transitions.
  assert(host_command_push(&queue,
                           (host_command_t){.kind = HOST_CANCEL, .code = 7}));
  bool typing = true, closed = false;
  unsigned ticks = 0, submitted = 0;
  while (queue.count || director.key_count) {
    host_command_t command;
    while (
        host_command_ready(&queue, d64_key_capacity(), 7, typing, &command)) {
      if (command.kind == HOST_CANCEL) {
        assert(submitted == 40 && typing);
        closed = true;
        typing = false;
      } else {
        assert(typing && command.kind == HOST_TYPE && d64_key_capacity() >= 2);
        d64_type_key(command.character, 1);
        d64_type_key(command.character, 0);
        submitted++;
      }
    }
    assert(!values.failed);
    assert(dg_tick(&director, 0, 0, false, DG_SERVICE_BUDGET));
    assert(++ticks < 200);
  }
  assert(closed && downs == 40 && ups == 40);
  for (unsigned i = 0; i < 4; i++)
    assert(!director.keys_down[i]);
  // A paused score with a full key queue cannot trap its text cancel/commit.
  for (unsigned i = 0; i < 16; i++) {
    d64_type_key('a', 1);
    d64_type_key('a', 0);
  }
  assert(d64_key_capacity() == 0);
  assert(host_command_push(&queue, (host_command_t){.kind = HOST_KEY,
                                                    .code = 0,
                                                    .character = 'a',
                                                    .down = true}));
  assert(host_command_push(&queue,
                           (host_command_t){.kind = HOST_CANCEL, .code = 7}));
  host_command_t command;
  assert(host_command_ready(&queue, 0, 7, false, &command) &&
         command.kind == HOST_CANCEL);
  assert(!host_command_ready(&queue, 0, 7, false, &command));
  // Closed-entry commands may be discarded even at zero engine capacity.
  assert(host_command_push(&queue,
                           (host_command_t){.kind = HOST_TYPE, .code = 7}));
  assert(host_command_ready(&queue, 0, 8, true, &command) &&
         command.kind == HOST_TYPE);
  puts("Android key queue: 80 transitions before immediate OK, pairs atomic, "
       "held "
       "control commands progress");
}
