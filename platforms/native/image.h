#ifndef DIRECTOR64_NATIVE_IMAGE_H
#define DIRECTOR64_NATIVE_IMAGE_H
#include "director.h"
bool native_image_hit(lv_runtime_t *, const dg_member_t *, unsigned, int, int);
bool native_cursor_bitmap(lv_runtime_t *, const dg_member_t *,
                          const dg_member_t *, dg_cursor_bitmap_t *);
#endif
