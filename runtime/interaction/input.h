#ifndef DIRECTOR64_INPUT_H
#define DIRECTOR64_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#define INPUT_ONE 65536
#if defined(DIRECTOR64_NATIVE_STAGE) && DIRECTOR64_NATIVE_STAGE
#define SCREEN_W 800
#define SCREEN_H 600
#else
#define SCREEN_W 640
#define SCREEN_H 480
#endif
enum {
    INPUT_A = 1u << 0, INPUT_B = 1u << 1,
    INPUT_LEFT = 1u << 2, INPUT_RIGHT = 1u << 3,
    INPUT_UP = 1u << 4, INPUT_DOWN = 1u << 5,
    INPUT_START = 1u << 6,
    INPUT_C_LEFT = 1u << 7, INPUT_C_RIGHT = 1u << 8,
    INPUT_C_UP = 1u << 9, INPUT_C_DOWN = 1u << 10,
};

/* One logical controller and optional absolute pointer. Recorded single-player
 * journeys use these same leading controller fields. */
typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stick_x, stick_y;
    bool pointer_absolute;
    int32_t pointer_x, pointer_y;
} input_sample_t;

typedef struct {
    int32_t x, y;
    uint16_t held, pressed, released;
    bool connected;
} input_state_t;

void input_init(input_state_t *state);
void input_center(input_state_t *state);
void input_update(input_state_t *state, const input_sample_t *sample);

#endif
