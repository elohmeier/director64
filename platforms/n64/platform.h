#ifndef DIRECTOR64_PLATFORM_H
#define DIRECTOR64_PLATFORM_H

#include "input.h"
#include <libdragon.h>

#define FONT_ID 1
#define STYLE_WHITE 1
#define STYLE_MUTED 2
#define STYLE_DARK 3
#define STYLE_ACCENT 4
#define REQUIRED_RDRAM (8 * 1024 * 1024)

bool platform_init(void);
void platform_shutdown(void);
uint64_t platform_now_us(void);
input_sample_t platform_poll_input(void);

#endif
