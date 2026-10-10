#ifndef DIRECTOR64_ANDROID_VIDEO_H
#define DIRECTOR64_ANDROID_VIDEO_H
// Director 5 digital video, one player per sprite channel showing a video
// member, as the page's VideoPlayers (platforms/web/site/video.js): the
// runtime tells each player its time every service step; the player decodes
// with MediaCodec up to the frame for that time and keeps it for the
// compositor, and plays the movie's sound in step with it.
#include "packs.h"
#include <stdbool.h>
#include <stdint.h>

void video_init(const pack_t *videos);
// The runtime's per-step report for one sprite (web_runtime.c update_videos):
// a NULL asset closes the sprite's player, "" is a video without picture.
void video_update(unsigned sprite, const char *asset, const char *sound, double time600,
                  double rate, unsigned serial, float gain);
bool video_frame(unsigned sprite, uint32_t *out, uint32_t cap, unsigned *width,
                 unsigned *height);
// Changes whenever a player has a new frame.
unsigned video_revision(void);
void video_close_all(void);
#endif
