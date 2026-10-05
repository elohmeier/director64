#include "d5_video.h"
#include <libdragon.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  unsigned sprite, member, serial;
  int frame;
  video_t *video;
  video_info_t info;
  wav64_t audio;
  bool audio_open, playing;
  double rate;
} video_slot_t;
static video_slot_t slots[D5_VIDEO_SLOTS];
static unsigned video_revision;

unsigned d5_video_revision(void) { return video_revision; }

void *__wrap_sys_hw_memset(void *ptr, uint8_t value, size_t length) {
  // Gopher64 0b96c50 rejects partial-word MI repeats used by H.264.
  // Preserve the SDK's CPU-memset semantics for cached AND uncached pointers;
  // publishing a dirty cached alias after an uncached write corrupts memory.
  return memset(ptr, value, length);
}

static unsigned voice(unsigned slot) {
  return DG_SOUND_CHANNELS * 2 + slot * 2;
}
static void close_slot(unsigned slot) {
  video_slot_t *v = &slots[slot];
  mixer_ch_stop(voice(slot));
  // Decoded planes can still be referenced by the previous display job.
  rspq_wait();
  if (v->audio_open)
    wav64_close(&v->audio);
  if (v->video)
    video_close(v->video);
  memset(v, 0, sizeof(*v));
  video_revision++;
}
void d5_video_init(void) {
  yuv_init();
  video_register_codec(&h264_codec);
}
void d5_video_close(void) {
  for (unsigned i = 0; i < D5_VIDEO_SLOTS; i++)
    close_slot(i);
}
void d5_video_update(dg_runtime_t *d, float volume) {
  for (unsigned i = 0; i < D5_VIDEO_SLOTS; i++) {
    video_slot_t *v = &slots[i];
    if (v->sprite && (!d->sprites[v->sprite].value.type ||
                      d->sprites[v->sprite].value.member != v->member))
      close_slot(i);
  }
  for (unsigned channel = 1; channel < DG_SPRITES; channel++) {
    dg_sprite_t *s = &d->sprites[channel];
    const dg_member_t *m = dg_member(d, s->value.member);
    if (!m || m->type != 10 || !s->value.type)
      continue;
    unsigned slot = 0;
    while (slot < D5_VIDEO_SLOTS && slots[slot].sprite != channel)
      slot++;
    if (slot == D5_VIDEO_SLOTS) {
      slot = 0;
      while (slot < D5_VIDEO_SLOTS && slots[slot].sprite)
        slot++;
      if (slot == D5_VIDEO_SLOTS) {
        lv_fail(d->values, "concurrent digital video capacity exceeded");
        return;
      }
      video_slot_t *v = &slots[slot];
      v->sprite = channel;
      v->member = m->id;
      v->frame = -1;
      video_revision++;
      char path[128];
      if (m->asset && *m->asset && !(m->video_flags & 512)) {
        snprintf(path, sizeof(path), "rom:/video/%s", m->asset);
        v->video = video_open(path, NULL);
        if (!v->video) {
          lv_fail(d->values, "digital video open failed");
          return;
        }
        v->info = video_get_info(v->video);
      }
      if (m->video_audio && *m->video_audio && (m->video_flags & 8)) {
        snprintf(path, sizeof(path), "rom:/audio/%s", m->video_audio);
        wav64_open(&v->audio, path);
        v->audio_open = true;
      }
      debugf("DIRECTOR64 VIDEO_OPEN sprite=%u member=%lu video=%u audio=%u\n",
             channel, (unsigned long)m->id, v->video != NULL, v->audio_open);
      heap_stats_t stats;
      sys_get_heap_stats(&stats);
      debugf("DIRECTOR64 VIDEO_INFO width=%d height=%d frame_rate=%.2f "
             "time=%.0f free=%d\n",
             v->info.width, v->info.height, v->info.framerate, s->video_time,
             stats.free);
    }
    video_slot_t *v = &slots[slot];
    if (v->video) {
      int target = (int)(s->video_time * v->info.framerate / 600.0);
      if (target != v->frame) {
        video_revision++;
        rspq_wait();
        if (target < v->frame || target > v->frame + 30)
          v->frame = video_seek(v->video, target) - 1;
        while (v->frame < target) {
          if (!video_next_frame(v->video))
            break;
          v->frame++;
        }
      }
    }
    if (v->audio_open) {
      unsigned ch = voice(slot);
      bool play = s->video_rate > 0 && s->video_time < m->samples;
      if (play && (!v->playing || v->serial != s->video_serial)) {
        if (!v->playing)
          wav64_play(&v->audio, ch);
        wav64_seek(&v->audio, ch, s->video_time / 600.0);
      } else if (!play && v->playing)
        mixer_ch_stop(ch);
      if (play)
        mixer_ch_set_freq(ch, v->audio.wave.frequency * s->video_rate);
      float gain = volume * s->video_volume / 255.0f;
      mixer_ch_set_vol(ch, gain, gain);
      v->playing = play;
    }
    v->serial = s->video_serial;
    v->rate = s->video_rate;
  }
}
void d5_video_draw(unsigned sprite, int l, int t, int r, int b) {
  for (unsigned i = 0; i < D5_VIDEO_SLOTS; i++) {
    video_slot_t *v = &slots[i];
    if (v->sprite != sprite || !v->video || v->frame < 0)
      continue;
    yuv_frame_t frame = video_get_frame(v->video);
    yuv_tex_blit(&frame, l, t,
                 &(rdpq_blitparms_t){
                     .scale_x = (float)(r - l) / v->info.width,
                     .scale_y = (float)(b - t) / v->info.height,
                 },
                 &v->info.colorspace);
  }
}
