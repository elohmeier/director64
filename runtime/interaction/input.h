#ifndef DIRECTOR64_INPUT_H
#define DIRECTOR64_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#define INPUT_ONE 65536
#define SCREEN_W 640
#define SCREEN_H 480
/* One player per console port. A player's index is its port number less one. */
#define INPUT_PLAYERS 4
/* How many of those ports the ROM declares; the build passes the game policy. */
#ifndef DIRECTOR64_CONTROLLER_COUNT
#define DIRECTOR64_CONTROLLER_COUNT 1
#endif

enum {
    INPUT_A = 1u << 0, INPUT_B = 1u << 1,
    INPUT_LEFT = 1u << 2, INPUT_RIGHT = 1u << 3,
    INPUT_UP = 1u << 4, INPUT_DOWN = 1u << 5,
    INPUT_START = 1u << 6,
    INPUT_C_LEFT = 1u << 7, INPUT_C_RIGHT = 1u << 8,
    INPUT_C_UP = 1u << 9, INPUT_C_DOWN = 1u << 10,
};

typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stick_x, stick_y;
} input_pad_t;

/* Port one stays in the leading fields, so recorded journeys, host probes and
 * the services that read a sample directly keep their existing shape. */
typedef struct {
    bool connected;
    uint16_t buttons;
    int8_t stick_x, stick_y;
    bool pointer_absolute;
    int32_t pointer_x, pointer_y;
    input_pad_t pads[INPUT_PLAYERS - 1]; /* Ports two through four. */
} input_sample_t;

typedef struct {
    int32_t x, y;
    uint16_t held, pressed, released;
    bool connected;
} input_state_t;

input_pad_t input_sample_pad(const input_sample_t *sample, unsigned player);
/* One player's controls as a whole sample, for the modal services that own the
 * screen: whoever holds the pointer drives them, and nobody else interferes. */
input_sample_t input_sample_for(const input_sample_t *sample, unsigned player);

void input_init(input_state_t *state);
void input_center(input_state_t *state);
void input_update(input_state_t *state, const input_sample_t *sample);
void input_update_pad(input_state_t *state, const input_pad_t *pad);

#endif
