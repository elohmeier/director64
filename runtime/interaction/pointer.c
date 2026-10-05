#include "pointer.h"
#include <stdint.h>
#include <string.h>

/* Player one keeps the centre a single controller has always started from;
 * the rest fan out so four cursors never boot stacked on one another. */
static const int start_offset[POINTER_PLAYERS] = {0, 56, -56, 112};
static const pointer_color_t palette[POINTER_PLAYERS] = {
    {248, 32, 32}, {48, 80, 248}, {248, 208, 16}, {32, 192, 48}};

pointer_color_t pointer_player_color(unsigned player) {
  return palette[player < POINTER_PLAYERS ? player : 0];
}
void pointer_init(pointer_control_t *p) {
  memset(p, 0, sizeof(*p));
  for (unsigned i = 0; i < POINTER_PLAYERS; i++) {
    input_init(&p->players[i].input);
    p->players[i].input.x += start_offset[i] * INPUT_ONE;
    /* A controller that is merely plugged in shows no cursor until used. */
    p->players[i].idle = POINTER_IDLE_TICKS;
  }
}
/* True while this player's click is still in flight, on either side of the
 * engine. Nobody takes the mouse out of their hand. */
static bool engaged(const pointer_player_t *q, const dg_runtime_t *d) {
  return (q->input.held & (INPUT_A | INPUT_B)) || d->mouse_down ||
         d->mouse_pressed || d->mouse_released || d->mouse_up_dispatch ||
         d->drag_sprite;
}
static bool moved(const input_pad_t *pad) {
  return (pad->buttons & (INPUT_LEFT | INPUT_RIGHT | INPUT_UP | INPUT_DOWN)) ||
         pad->stick_x > 8 || pad->stick_x < -8 ||
         pad->stick_y > 8 || pad->stick_y < -8;
}
/* Whoever presses takes the mouse; failing that, whoever is actually pointing,
 * so rollover follows them. The holder wins every tie and never loses the
 * mouse mid-click, which keeps one controller's behaviour unchanged. */
static unsigned choose(const pointer_control_t *p, const input_pad_t *pads,
                       const dg_runtime_t *d) {
  unsigned active = p->active;
  bool holding = pads[active].connected;
  if (holding && engaged(&p->players[active], d)) return active;
  const unsigned claim = INPUT_A | INPUT_B | INPUT_START;
  if (holding && (p->players[active].input.pressed & claim)) return active;
  for (unsigned i = 0; i < POINTER_PLAYERS; i++)
    if (pads[i].connected && (p->players[i].input.pressed & claim)) return i;
  if (holding && moved(&pads[active])) return active;
  for (unsigned i = 0; i < POINTER_PLAYERS; i++)
    if (pads[i].connected && moved(&pads[i])) return i;
  if (holding) return active;
  for (unsigned i = 0; i < POINTER_PLAYERS; i++)
    if (pads[i].connected) return i;
  return active;
}
bool pointer_visible(const pointer_control_t *p, unsigned player) {
  if (player >= POINTER_PLAYERS) return false;
  if (player == p->active) return true;
  const pointer_player_t *q = &p->players[player];
  return !p->absolute && q->input.connected && q->idle < POINTER_IDLE_TICKS;
}
void pointer_update(pointer_control_t *p, const dg_runtime_t *d,
                    const input_sample_t *sample) {
  int32_t old_x[POINTER_PLAYERS], old_y[POINTER_PLAYERS];
  input_pad_t pads[POINTER_PLAYERS];
  for (unsigned i = 0; i < POINTER_PLAYERS; i++) {
    pointer_player_t *q = &p->players[i];
    old_x[i] = q->input.x; old_y[i] = q->input.y;
    pads[i] = input_sample_pad(sample, i);
    if (i == 0) input_update(&q->input, sample);
    else input_update_pad(&q->input, &pads[i]);
    if (pads[i].connected && (pads[i].buttons || moved(&pads[i])))
      q->idle = 0;
    else if (q->idle < POINTER_IDLE_TICKS)
      q->idle++;
  }
  p->absolute = sample->pointer_absolute;
  if (p->absolute) {
    /* Recorded mouse journeys drive port one alone, and the other cursors stay
     * parked, so a replay reproduces exactly what it recorded. */
    p->active = 0;
    for (unsigned i = 1; i < POINTER_PLAYERS; i++) {
      p->players[i].input.x = old_x[i];
      p->players[i].input.y = old_y[i];
    }
    return;
  }
  p->active = choose(p, pads, d);
}
