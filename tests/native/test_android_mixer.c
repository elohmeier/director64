#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <time.h>
static double stamp;
static int fake_clock(clockid_t id, struct timespec *out) {
  (void)id;
  out->tv_sec = (time_t)stamp;
  out->tv_nsec = (long)((stamp - out->tv_sec) * 1e9);
  return 0;
}
#define clock_gettime fake_clock
#include "../../platforms/android/jni/mixer.c"
#undef clock_gettime
static unsigned trial;
int __android_log_print(int p, const char *tag, const char *fmt, ...) {
  (void)p;
  (void)tag;
  (void)fmt;
  return 0;
}
sound_t *sound_retain(sound_t *s) {
  if (s)
    atomic_fetch_add(&s->references, 1);
  return s;
}
void sound_release(sound_t *s) {
  if (s && atomic_fetch_sub(&s->references, 1) == 1) {
    free(s->pcm);
    free(s);
  }
}
size_t sound_bytes(const sound_t *s) { return s->frames * 2; }
bool is_wav(const uint8_t *d, size_t n) {
  (void)d;
  (void)n;
  return false;
}
sound_t *decode_wav(const uint8_t *d, size_t n) {
  (void)d;
  (void)n;
  return NULL;
}
sound_t *decode_media(int fd, uint64_t o, uint64_t n, char *error, size_t cap) {
  (void)fd;
  (void)o;
  (void)n;
  (void)error;
  (void)cap;
  if (trial == 0) {
    audio_suspend(true);
    stamp = 30;
  }
  if (trial == 1) {
    stamp = 1;
    audio_pause(0, true);
    audio_suspend(true);
    stamp = 31;
    audio_suspend(false);
    stamp = 33;
    audio_pause(0, false);
    stamp = 34;
  }
  if (trial == 2) {
    stamp = 2;
    video_sprite[0] = 19;
    audio_video_rate(19, 2.0, 1.0f);
    stamp = 3;
    audio_suspend(true);
    stamp = 33;
    audio_suspend(false);
    stamp = 34;
  }
  if (trial == 3) {
    stop_voice(0);
    stamp = 30;
  }
  stopping = true;
  sound_t *s = calloc(1, sizeof(*s));
  s->rate = 100;
  s->channels = 1;
  s->frames = (trial == 0 ? 5 : 60) * 100;
  s->pcm = calloc(s->frames, 2);
  for (unsigned i = 0; i < s->frames; i++)
    s->pcm[i] = 10000;
  atomic_init(&s->references, 1);
  return s;
}
int main(void) {
  pack_t pack = {.fd = 1};
  sound_pack = &pack;
  stream_pack = &pack;
  output_rate = 100;
  for (trial = 0; trial < 4; trial++) {
    stamp = 0;
    stopping = false;
    job_count = 1;
    suspended_total = suspended_at = 0;
    atomic_store(&suspended, false);
    unsigned v = trial == 2 ? CHANNELS : 0;
    voices[v] = (voice_t){.pending = 1, .rate = 1, .release_offset = -1};
    gains[v] = 1;
    jobs[0] = (job_t){.voice = v, .token = 1, .name = "pending.opus"};
    decoder(NULL);
    if (trial == 3)
      assert(!voices[v].sound);
    else {
      assert(voices[v].sound);
      assert(voices[v].position == (trial == 0 ? 0 : trial == 1 ? 200 : 600));
      if (trial == 0) {
        float out[20];
        render(NULL, NULL, out, 10);
        for (unsigned i = 0; i < 20; i++)
          assert(out[i] == 0);
        assert(voices[v].position == 0);
        audio_suspend(false);
        render(NULL, NULL, out, 10);
        assert(out[0] > 0.3f && voices[v].position == 10);
      }
    }
    stop_voice(v);
    for (unsigned i = 0; i < cache_count; i++) {
      sound_release(cache[i].sound);
      free(cache[i].name);
    }
    free(cache);
    cache = NULL;
    cache_count = cache_cap = 0;
    cache_bytes = 0;
  }
  puts("Android mixer: pending decode excludes global/channel holds, preserves "
       "rates and cancellation");
}
