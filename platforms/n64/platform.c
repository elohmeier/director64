#include "platform.h"
#include "family.h"

bool platform_init(void)
{
    debug_init_emulog();
    debug_init_usblog();
    int memory_size = get_memory_size();
    if (memory_size < REQUIRED_RDRAM) {
        debugf("DIRECTOR64 REQUIREMENT_FAIL rdram_bytes=%d required=%d\n",
            memory_size, REQUIRED_RDRAM);
        return false;
    }
    /* Preserve the qualified prototype's HALF policy until M64 comparison. */
    display_init(RESOLUTION_640x480, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
    rdpq_init();
    joypad_init();
    if (dfs_init(DFS_DEFAULT_LOCATION) != DFS_ESUCCESS) {
        debugf("DIRECTOR64 REQUIREMENT_FAIL reason=filesystem\n");
        platform_shutdown();
        return false;
    }
    // Assets carry their own level. A game's codec tiers may mix all three:
    // level 1 is always linked, the other two are opt-in decoders.
    asset_init_compression(2);
    asset_init_compression(3);
    debugf("DIRECTOR64 BOOT_OK rdram_bytes=%d target=m64-summercart\n", memory_size);
    return true;
}

void platform_shutdown(void)
{
    rspq_wait();
    joypad_close();
    rdpq_close();
    display_close();
}

uint64_t platform_now_us(void)
{
    return get_ticks_us();
}

/* Every declared port carries a player. Ports beyond the first are read the
 * same way, so a cursor behaves identically whichever controller drives it. */
static input_pad_t poll_port(joypad_port_t port)
{
    input_pad_t pad = {.connected = joypad_is_connected(port)};
    if (!pad.connected) return pad;
    joypad_buttons_t held = joypad_get_buttons_held(port);
    joypad_inputs_t inputs = joypad_get_inputs(port);
    if (held.a) pad.buttons |= INPUT_A;
    if (held.b) pad.buttons |= INPUT_B;
    if (held.start) pad.buttons |= INPUT_START;
#if DG_CAP_KEYBOARD // the C buttons are the arrow keys
    if (held.c_left) pad.buttons |= INPUT_C_LEFT;
    if (held.c_right) pad.buttons |= INPUT_C_RIGHT;
    if (held.c_up) pad.buttons |= INPUT_C_UP;
    if (held.c_down) pad.buttons |= INPUT_C_DOWN;
#endif
    if (held.d_left) pad.buttons |= INPUT_LEFT;
    if (held.d_right) pad.buttons |= INPUT_RIGHT;
    if (held.d_up) pad.buttons |= INPUT_UP;
    if (held.d_down) pad.buttons |= INPUT_DOWN;
    pad.stick_x = inputs.stick_x;
    pad.stick_y = inputs.stick_y;
    return pad;
}

input_sample_t platform_poll_input(void)
{
    joypad_poll();
    input_sample_t sample = {0};
    input_pad_t first = poll_port(JOYPAD_PORT_1);
    sample.connected = first.connected;
    sample.buttons = first.buttons;
    sample.stick_x = first.stick_x;
    sample.stick_y = first.stick_y;
    for (unsigned i = 1; i < DIRECTOR64_CONTROLLER_COUNT && i < INPUT_PLAYERS; i++)
        sample.pads[i - 1] = poll_port((joypad_port_t)(JOYPAD_PORT_1 + i));
    return sample;
}
