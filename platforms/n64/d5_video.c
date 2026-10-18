#include "d5_video.h"
#if DG_D10
#include "flash_audio.h"
#endif
#include <libdragon.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  unsigned identity;
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
#if DG_D10
  flash_audio_close();
#endif
  for (unsigned i = 0; i < D5_VIDEO_SLOTS; i++)
    close_slot(i);
}
typedef struct { dg_runtime_t *owner; unsigned channel; const dg_member_t *member; bool sound, paused; } video_request_t;
static bool video_requests(dg_runtime_t *d, video_request_t *requests, unsigned *count, bool sound, bool paused) {
  for (unsigned ch = 1; ch < DG_SPRITES; ch++) {
    dg_sprite_t *s = &d->sprites[ch];
    if (!s->value.type) continue;
    const dg_member_t *m = dg_member(d, s->value.member);
    if (dg_has_media(m)) {
      if (*count == D5_VIDEO_SLOTS) { lv_fail(d->values, "concurrent digital video capacity exceeded"); return false; }
      requests[(*count)++] = (video_request_t){d,ch,m,sound,paused};
    }
#if DG_D10
    if (dg_linked_movie(d,ch)) {
      dg_runtime_t *child = dg_linked_begin(d,ch);
      bool ok = child && video_requests(child,requests,count,sound && m && (m->linked_flags & 8), paused || s->film_paused || child->linked_paused);
      dg_linked_end(d);
      if (!ok) return false;
    }
#endif
  }
  return true;
}
void d5_video_update(dg_runtime_t *d, float volume) {
  video_request_t requests[D5_VIDEO_SLOTS];
  unsigned count = 0;
  if (!video_requests(d,requests,&count,true,false)) return;
  // Never dereference a slot's old owner: its child context may have gone away.
  for (unsigned i = 0; i < D5_VIDEO_SLOTS; i++) {
    video_slot_t *v = &slots[i];
    bool found = false;
    for (unsigned n = 0; n < count; n++)
      if (v->identity == dg_video_player_id(requests[n].owner, requests[n].channel) &&
          v->member == requests[n].member->id) found = true;
    if (v->sprite && !found) close_slot(i);
  }
  for (unsigned n = 0; n < count; n++) {
    dg_runtime_t *owner = requests[n].owner;
    unsigned channel = requests[n].channel;
    dg_sprite_t *s = &owner->sprites[channel];
    const dg_member_t *m = requests[n].member;
    unsigned slot = 0;
    while (slot < D5_VIDEO_SLOTS && slots[slot].identity != dg_video_player_id(owner, channel))
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
      v->identity = dg_video_player_id(owner, channel);
      v->sprite = channel;
      v->member = m->id;
      v->frame = -1;
      video_revision++;
      char path[128];
      if (dg_is_video(m) && m->asset && *m->asset && !(m->video_flags & 512)) {
        snprintf(path, sizeof(path), "rom:/video/%s", m->asset);
        v->video = video_open(path, NULL);
        if (!v->video) {
          lv_fail(d->values, "digital video open failed");
          return;
        }
        v->info = video_get_info(v->video);
      }
      if (m->video_audio && *m->video_audio && dg_media_sound(m)) {
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
      bool play = requests[n].sound && !requests[n].paused && s->video_rate > 0 && s->video_time < dg_media_duration(m);
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
bool d5_video_dimensions(const dg_runtime_t *owner, unsigned sprite, unsigned *width, unsigned *height) {
  for (unsigned i=0;i<D5_VIDEO_SLOTS;i++) {
    const video_slot_t *v=&slots[i];
    if (v->identity == dg_video_player_id(owner,sprite) && v->video && v->frame>=0) {
      *width=v->info.width; *height=v->info.height; return true;
    }
  }
  return false;
}
void d5_video_draw(const dg_runtime_t *owner, unsigned sprite, int l, int t, int r, int b) {
  for (unsigned i = 0; i < D5_VIDEO_SLOTS; i++) {
    video_slot_t *v = &slots[i];
    if (v->identity != dg_video_player_id(owner, sprite) || !v->video || v->frame < 0)
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
