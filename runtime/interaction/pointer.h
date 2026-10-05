#ifndef DIRECTOR64_POINTER_H
#define DIRECTOR64_POINTER_H
#include "director.h"
#include "input.h"

#define POINTER_PLAYERS INPUT_PLAYERS
/* Service ticks (60 Hz) a player may sit untouched before their cursor
 * leaves the screen. */
#define POINTER_IDLE_TICKS 300

/* Per-port pointer state. Director owns a single mouse, so exactly one player
 * drives it at a time; the others keep their own cursor and take the mouse
 * over when they press or point. */
typedef struct {
  input_state_t input;
  uint16_t idle; /* Service ticks since this player last moved or pressed. */
} pointer_player_t;

typedef struct {
  pointer_player_t players[POINTER_PLAYERS];
  unsigned active; /* The player currently holding Director's mouse. */
  bool absolute;
} pointer_control_t;

/* One colour per port so several cursors stay apart on a shared screen. */
typedef struct {
  uint8_t r, g, b;
} pointer_color_t;
pointer_color_t pointer_player_color(unsigned player);

void pointer_init(pointer_control_t *);
/* Service at 60 Hz: move every connected cursor and settle who holds the mouse. */
void pointer_update(pointer_control_t *, const dg_runtime_t *,
                    const input_sample_t *);
/* The holder's cursor is the one the engine follows, so it always shows; the
 * others show only while someone is actually using that controller. */
bool pointer_visible(const pointer_control_t *, unsigned player);
static inline pointer_player_t *pointer_active(pointer_control_t *p) {
  return &p->players[p->active];
}
static inline const pointer_player_t *pointer_driver(
    const pointer_control_t *p) {
  return &p->players[p->active];
}
#endif
