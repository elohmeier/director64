#include "input.h"

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

static int32_t clamp(int64_t value, int32_t low, int32_t high)
{
    return value < low ? low : value > high ? high : (int32_t)value;
}

void input_update(input_state_t *state, const input_sample_t *sample)
{
    uint16_t buttons = sample->connected ? sample->buttons : 0;
    state->pressed = buttons & ~state->held;
    state->released = state->held & ~buttons;
    state->held = buttons;
    state->connected = sample->connected;
    if (!sample->connected) return;

    int64_t x = state->x, y = state->y;
    if (sample->pointer_absolute) {
        x = sample->pointer_x;
        y = sample->pointer_y;
    } else {
        /* Synthetic prototype mapping: dead zone 8, 0.11 px/unit/service tick. */
        if (sample->stick_x > 8 || sample->stick_x < -8)
            x += sample->stick_x * (INPUT_ONE * 11 / 100);
        if (sample->stick_y > 8 || sample->stick_y < -8)
            y -= sample->stick_y * (INPUT_ONE * 11 / 100);
        if (buttons & INPUT_LEFT) x -= 5 * INPUT_ONE;
        if (buttons & INPUT_RIGHT) x += 5 * INPUT_ONE;
        if (buttons & INPUT_UP) y -= 5 * INPUT_ONE;
        if (buttons & INPUT_DOWN) y += 5 * INPUT_ONE;
    }
    state->x = clamp(x, 8 * INPUT_ONE, (SCREEN_W - 10) * INPUT_ONE);
    state->y = clamp(y, 8 * INPUT_ONE, (SCREEN_H - 10) * INPUT_ONE);
}
