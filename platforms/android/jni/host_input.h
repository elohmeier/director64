#ifndef DIRECTOR64_ANDROID_HOST_INPUT_H
#define DIRECTOR64_ANDROID_HOST_INPUT_H
// UI producers and the game worker use these bounded queues under `state`.
// Motion coalesces; button transitions do not. Each game tick consumes one
// transition so a short press and its release cannot collapse into nothing.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
enum { HOST_EDGES = 64, HOST_COMMANDS = 128 };
typedef struct {
  int x, y;
  unsigned buttons;
  bool inside;
} host_pointer_sample_t;
typedef struct {
  host_pointer_sample_t current, edges[HOST_EDGES];
  unsigned head, count, delivered, masked;
  bool moved;
} host_pointer_t;
typedef struct {
  bool connected;
  unsigned buttons, masked, delivered, head, count, edges[HOST_EDGES];
  int stick_x, stick_y;
} host_pad_t;
typedef enum {
  HOST_KEY,
  HOST_TYPE,
  HOST_COMMIT,
  HOST_CANCEL
} host_command_kind_t;
typedef struct {
  host_command_kind_t kind;
  unsigned code, character;
  bool down;
  char text[64];
} host_command_t;
typedef struct {
  host_command_t items[HOST_COMMANDS];
  unsigned head, count;
} host_commands_t;
static inline bool host_command_push(host_commands_t *q,
                                     host_command_t command) {
  if (q->count == HOST_COMMANDS)
    return false;
  q->items[(q->head + q->count++) % HOST_COMMANDS] = command;
  return true;
}
static inline bool host_command_pop(host_commands_t *q,
                                    host_command_t *command) {
  if (!q->count)
    return false;
  *command = q->items[q->head];
  q->head = (q->head + 1) % HOST_COMMANDS;
  q->count--;
  return true;
}
// Keyboard transitions keep FIFO order and a typed character reserves both
// down/up slots. Closing an active key entry follows its preceding characters.
// Editable-field controls may bypass blocked keys while HOLD_TEXT stops ticks.
static inline int host_command_index(const host_commands_t *q, unsigned slots,
                                     unsigned token, bool typing) {
  bool blocked = false;
  for (unsigned i = 0; i < q->count; i++) {
    const host_command_t *c = &q->items[(q->head + i) % HOST_COMMANDS];
    unsigned need = c->kind == HOST_KEY                                  ? 1
                    : c->kind == HOST_TYPE && c->code == token && typing ? 2
                                                                         : 0;
    if (need && (blocked || need > slots)) {
      blocked = true;
      continue;
    }
    if (blocked && typing && c->kind == HOST_CANCEL && c->code == token)
      continue;
    return (int)i;
  }
  return -1;
}
static inline bool host_command_ready(host_commands_t *q, unsigned slots,
                                      unsigned token, bool typing,
                                      host_command_t *out) {
  int index = host_command_index(q, slots, token, typing);
  if (index < 0)
    return false;
  unsigned at = (unsigned)index;
  *out = q->items[(q->head + at) % HOST_COMMANDS];
  for (unsigned i = at; i + 1 < q->count; i++)
    q->items[(q->head + i) % HOST_COMMANDS] =
        q->items[(q->head + i + 1) % HOST_COMMANDS];
  q->count--;
  return true;
}

static inline bool host_pad_update(host_pad_t *p, bool connected,
                                   unsigned buttons, int x, int y, bool held) {
  if (!connected) {
    memset(p, 0, sizeof(*p));
    return true;
  }
  bool changed = buttons != p->buttons;
  p->connected = true;
  p->masked &= buttons;
  p->stick_x = x;
  p->stick_y = y;
  p->buttons = buttons;
  if (held) {
    p->count = p->head = 0;
    p->masked = buttons;
    return true;
  }
  if (!changed)
    return true;
  if (p->count == HOST_EDGES)
    return false;
  p->edges[(p->head + p->count++) % HOST_EDGES] = buttons;
  return true;
}
static inline unsigned host_pad_sample(host_pad_t *p, unsigned *pressed) {
  unsigned buttons = p->buttons;
  if (p->count) {
    buttons = p->edges[p->head];
    p->head = (p->head + 1) % HOST_EDGES;
    p->count--;
  }
  buttons &= ~p->masked;
  *pressed = buttons & ~p->delivered;
  p->delivered = buttons;
  return buttons;
}
static inline bool host_pointer_update(host_pointer_t *p,
                                       host_pointer_sample_t sample,
                                       bool held) {
  bool changed = p->current.buttons != sample.buttons;
  p->moved |= p->current.x != sample.x || p->current.y != sample.y || changed;
  p->current = sample;
  p->masked &= sample.buttons;
  if (held) {
    p->count = p->head = 0;
    p->masked = sample.buttons;
    return true;
  }
  if (!changed)
    return true;
  if (p->count == HOST_EDGES)
    return false;
  p->edges[(p->head + p->count++) % HOST_EDGES] = sample;
  return true;
}
static inline host_pointer_sample_t host_pointer_sample(host_pointer_t *p,
                                                        unsigned *pressed) {
  host_pointer_sample_t sample = p->current;
  if (p->count) {
    sample = p->edges[p->head];
    p->head = (p->head + 1) % HOST_EDGES;
    p->count--;
  }
  sample.buttons &= ~p->masked;
  *pressed = sample.buttons & ~p->delivered;
  p->delivered = sample.buttons;
  return sample;
}
static inline void host_input_resume(host_pad_t *pads, unsigned count,
                                     host_pointer_t *pointer) {
  for (unsigned i = 0; i < count; i++) {
    pads[i].masked = pads[i].buttons;
    pads[i].count = pads[i].head = 0;
  }
  pointer->masked = pointer->current.buttons;
  pointer->count = pointer->head = 0;
}
#endif
