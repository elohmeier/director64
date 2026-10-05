#include "pointer.h"
#include <assert.h>
#include <stdio.h>

static lv_runtime_t values;
static dg_runtime_t director;
static pointer_control_t pointer;
/* One controller always drives, so player one is the driver here. */
static pointer_player_t *const one = &pointer.players[0];
static pointer_player_t *const two = &pointer.players[1];

static void init(void) {
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  pointer_init(&pointer);
}
static input_pad_t stick(int x, int y, unsigned buttons) {
  return (input_pad_t){.connected = true, .buttons = (uint16_t)buttons,
                       .stick_x = (int8_t)x, .stick_y = (int8_t)y};
}
static void pads(input_pad_t first, input_pad_t second) {
  input_sample_t s = {.connected = first.connected, .buttons = first.buttons,
                      .stick_x = first.stick_x, .stick_y = first.stick_y};
  s.pads[0] = second;
  pointer_update(&pointer, &director, &s);
}
static void players(void) {
  init();
  /* Every port has its own colour, or the cursors cannot be told apart. */
  for (unsigned i = 0; i < POINTER_PLAYERS; i++)
    for (unsigned j = i + 1; j < POINTER_PLAYERS; j++) {
      pointer_color_t a = pointer_player_color(i), b = pointer_player_color(j);
      assert(a.r != b.r || a.g != b.g || a.b != b.b);
    }
  /* Port one keeps the centre a single controller has always started from;
   * the rest fan out so four cursors never boot stacked on one another. */
  assert(one->input.x == 320 * INPUT_ONE && one->input.y == 240 * INPUT_ONE);
  for (unsigned i = 1; i < POINTER_PLAYERS; i++)
    assert(pointer.players[i].input.x != one->input.x);
  assert(pointer.active == 0 && pointer_driver(&pointer) == one);
}
static void handover(void) {
  init();
  const input_pad_t idle = stick(0, 0, 0), absent = {0};
  assert((pads(idle, idle), pointer.active == 0));

  /* Pointing hands the mouse over, so rollover follows whoever is pointing.
   * A positive stick reading moves that player's cursor up the screen. */
  int32_t parked = two->input.y;
  pads(idle, stick(0, 70, 0));
  assert(pointer.active == 1 && pointer_driver(&pointer) == two);
  assert(two->input.y < parked && one->input.y == 240 * INPUT_ONE);
  pads(stick(0, 70, 0), idle);
  assert(pointer.active == 0);

  /* A press claims the mouse, but never out of another player's hand. */
  pads(stick(0, 0, INPUT_A), idle);
  assert(pointer.active == 0 && (one->input.held & INPUT_A));
  pads(stick(0, 0, INPUT_A), stick(0, 0, INPUT_A));
  assert(pointer.active == 0);
  /* The engine is still dispatching the click after the button is released. */
  director.mouse_down = true;
  pads(idle, stick(0, 0, INPUT_A));
  assert(pointer.active == 0);
  director.mouse_down = false;
  pads(idle, idle);
  pads(idle, stick(0, 0, INPUT_A));
  assert(pointer.active == 1);

  /* An unplugged controller hides its cursor and gives the mouse back. */
  pads(idle, idle);
  parked = two->input.x;
  pads(idle, absent);
  assert(pointer.active == 0 && !two->input.connected &&
         two->input.x == parked);
}
static void visibility(void) {
  init();
  const input_pad_t idle = stick(0, 0, 0), absent = {0};
  /* A controller that is only plugged in shows no cursor; the holder always
   * does, so a single player never loses theirs by standing still. */
  pads(idle, idle);
  assert(pointer_visible(&pointer, 0) && !pointer_visible(&pointer, 1));
  pads(absent, absent);
  assert(pointer_visible(&pointer, 0) && !pointer_visible(&pointer, 1));

  /* Using the controller shows its cursor, and it stays through a pause. */
  pads(idle, stick(0, 70, 0));
  pads(stick(0, 70, 0), idle);
  assert(pointer.active == 0 && pointer_visible(&pointer, 1));
  for (unsigned i = 2; i < POINTER_IDLE_TICKS; i++) pads(idle, idle);
  assert(pointer_visible(&pointer, 1));
  /* Left alone long enough, it leaves the screen where it was parked. */
  int32_t parked = two->input.x;
  pads(idle, idle);
  assert(!pointer_visible(&pointer, 1) && two->input.x == parked);
  /* A press alone brings it back, as does unplugging hide it at once. */
  pads(idle, stick(0, 0, INPUT_START));
  assert(pointer_visible(&pointer, 1));
  pads(idle, idle);
  pads(idle, absent);
  assert(!pointer_visible(&pointer, 1));
}
static void compatibility(void) {
  init();
  input_state_t legacy;
  input_init(&legacy);
  /* One controller points exactly as the shared input controller does: the
   * stick and D-pad move the cursor and A is the mouse button. */
  input_sample_t s = {.connected = true, .stick_x = 64, .stick_y = -20,
                      .buttons = INPUT_A | INPUT_UP};
  s.pads[0] = stick(70, 0, 0);
  for (unsigned i = 0; i < 50; i++) {
    input_update(&legacy, &s);
    pointer_update(&pointer, &director, &s);
    assert(one->input.x == legacy.x && one->input.y == legacy.y);
    assert(one->input.pressed == legacy.pressed && one->input.held == legacy.held);
  }
  /* Recorded absolute journeys keep their exact semantics: port one follows
   * the recording and the other cursors stay parked. */
  int32_t parked = two->input.x;
  s.pointer_absolute = true;
  s.pointer_x = 56 * INPUT_ONE;
  s.pointer_y = 340 * INPUT_ONE;
  pointer_update(&pointer, &director, &s);
  assert(pointer.absolute && pointer.active == 0);
  assert(one->input.x == s.pointer_x && one->input.y == s.pointer_y);
  assert(two->input.x == parked);
}
int main(void) {
  players();
  handover();
  visibility();
  compatibility();
  puts("Pointer: per-port cursors, mouse handover, idle hiding, disconnect and "
       "recorded journey compatibility passed");
}
