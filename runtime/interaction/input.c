#include "input.h"
#include <stddef.h>

void input_center(input_state_t *state)
{
    state->x = SCREEN_W / 2 * INPUT_ONE;
    state->y = SCREEN_H / 2 * INPUT_ONE;
}

void input_init(input_state_t *state)
{
    *state = (input_state_t){0};
    input_center(state);
}

input_pad_t input_sample_pad(const input_sample_t *sample, unsigned player)
{
    if (player == 0)
        return (input_pad_t){.connected = sample->connected,
                             .buttons = sample->buttons,
                             .stick_x = sample->stick_x,
                             .stick_y = sample->stick_y};
    if (player >= INPUT_PLAYERS) return (input_pad_t){0};
    return sample->pads[player - 1];
}

input_sample_t input_sample_for(const input_sample_t *sample, unsigned player)
{
    input_pad_t pad = input_sample_pad(sample, player);
    input_sample_t one = {.connected = pad.connected, .buttons = pad.buttons,
                          .stick_x = pad.stick_x, .stick_y = pad.stick_y};
    /* Recorded absolute journeys only ever drive port one. */
    if (player == 0) {
        one.pointer_absolute = sample->pointer_absolute;
        one.pointer_x = sample->pointer_x;
        one.pointer_y = sample->pointer_y;
    }
    return one;
}

static int32_t clamp(int64_t value, int32_t low, int32_t high)
{
    return value < low ? low : value > high ? high : (int32_t)value;
}

static void update(input_state_t *state, const input_pad_t *pad,
                   const int32_t *absolute)
{
    uint16_t buttons = pad->connected ? pad->buttons : 0;
    state->pressed = buttons & ~state->held;
    state->released = state->held & ~buttons;
    state->held = buttons;
    state->connected = pad->connected;
    if (!pad->connected) return;

    int64_t x = state->x, y = state->y;
    if (absolute) {
        x = absolute[0];
        y = absolute[1];
    } else {
        /* Synthetic prototype mapping: dead zone 8, 0.11 px/unit/service tick. */
        if (pad->stick_x > 8 || pad->stick_x < -8)
            x += pad->stick_x * (INPUT_ONE * 11 / 100);
        if (pad->stick_y > 8 || pad->stick_y < -8)
            y -= pad->stick_y * (INPUT_ONE * 11 / 100);
        if (buttons & INPUT_LEFT) x -= 5 * INPUT_ONE;
        if (buttons & INPUT_RIGHT) x += 5 * INPUT_ONE;
        if (buttons & INPUT_UP) y -= 5 * INPUT_ONE;
        if (buttons & INPUT_DOWN) y += 5 * INPUT_ONE;
    }
    state->x = clamp(x, 8 * INPUT_ONE, (SCREEN_W - 10) * INPUT_ONE);
    state->y = clamp(y, 8 * INPUT_ONE, (SCREEN_H - 10) * INPUT_ONE);
}

void input_update(input_state_t *state, const input_sample_t *sample)
{
    const input_pad_t pad = input_sample_pad(sample, 0);
    const int32_t absolute[2] = {sample->pointer_x, sample->pointer_y};
    update(state, &pad, sample->pointer_absolute ? absolute : NULL);
}

void input_update_pad(input_state_t *state, const input_pad_t *pad)
{
    update(state, pad, NULL);
}
