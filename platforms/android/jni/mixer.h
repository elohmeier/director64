#ifndef DIRECTOR64_ANDROID_MIXER_H
#define DIRECTOR64_ANDROID_MIXER_H
// The runtime's sound channels through AAudio, as the page's AudioEngine
// plays them (platforms/web/site/audio.js): channel lifetime is the runtime's
// service clock; this plays what the runtime started, with its loop points
// and gains, and pauses with the game. Digital video sound has a voice per
// video sprite beside the channels.
#include "packs.h"
#include <stdbool.h>
#include <stdint.h>
#include "flash_runtime.h"

void audio_init(const pack_t *sounds, const pack_t *streams);
void audio_shutdown(void);
void audio_play(unsigned channel, const char *name, bool looping, uint32_t loop_start,
                uint32_t loop_end);
// A playFile stream; returns its length in milliseconds (0 when missing).
uint32_t audio_play_stream(unsigned channel, const char *name);
void audio_stop(unsigned channel);
void audio_pause(unsigned channel, bool paused);
void audio_break_loop(unsigned channel);
void audio_gain(unsigned channel, float gain);
// The player's volume over the game's own gains.
void audio_volume(float volume);
void audio_suspend(bool suspended);
void audio_stop_all(void);
// A sound's length in seconds, 0 when missing.
double audio_duration(const char *name);
void audio_play_video(unsigned sprite, const char *name, double offset);
void audio_video_rate(unsigned sprite, double rate, float gain);
void audio_stop_video(unsigned sprite);
#if DG_D10
void audio_flash_update(uint32_t id,const dg_flash_audio_t *sound);
#endif
// Reopens the output after the device changed (HDMI, Bluetooth speakers).
void audio_poll(void);
// Copies the first active playback failure; decoder errors must reach the UI.
bool audio_error(char *out, size_t capacity);
#endif
