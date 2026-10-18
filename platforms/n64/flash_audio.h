#ifndef DIRECTOR64_N64_FLASH_AUDIO_H
#define DIRECTOR64_N64_FLASH_AUDIO_H
#include "director.h"
#include "d5_video.h"
enum { FLASH_AUDIO_SLOTS=6, FLASH_AUDIO_CHANNELS=D5_MIXER_CHANNELS+FLASH_AUDIO_SLOTS*2 };
void flash_audio_close(void);
void flash_audio_update(dg_runtime_t *,float volume);
#endif
