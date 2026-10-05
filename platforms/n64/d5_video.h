#ifndef DIRECTOR64_D5_VIDEO_H
#define DIRECTOR64_D5_VIDEO_H
#include "director.h"
enum { D5_VIDEO_SLOTS = 2, D5_MIXER_CHANNELS = DG_SOUND_CHANNELS * 2 + 4 };
void d5_video_init(void);
void d5_video_close(void);
void d5_video_update(dg_runtime_t *d, float volume);
// Advances whenever a slot decodes a new frame, opens or closes: a composited
// stage stays valid only while this holds still.
unsigned d5_video_revision(void);
void d5_video_draw(unsigned sprite, int left, int top, int right, int bottom);
#endif
