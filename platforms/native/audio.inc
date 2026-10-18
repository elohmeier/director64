// Deterministic audio timing for native probes. The service clock continues
// while the score waits; pausing a sound freezes its sample position.
static unsigned sound_count;
typedef struct {
  uint32_t started, paused_at, rate, frames, loop_start, loop_end;
  uint64_t position;
  bool looping, paused;
} probe_voice_t;
static probe_voice_t probe_voices[DG_SOUND_CHANNELS];

static uint64_t probe_sound_frame(unsigned channel) {
  const probe_voice_t *v = &probe_voices[channel];
  uint32_t now = v->paused ? v->paused_at : director.ticks;
  uint64_t frame = v->position + (uint64_t)(now - v->started) * v->rate / 60;
  if (v->looping && frame >= v->loop_end && v->loop_end > v->loop_start)
    frame = v->loop_start + (frame - v->loop_start) % (v->loop_end - v->loop_start);
  return frame;
}
static void sound(void *ctx, unsigned channel, const dg_member_t *m) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS) { lv_fail(&values, "probe audio channel"); return; }
  memset(&probe_voices[channel], 0, sizeof(probe_voices[channel]));
  probe_voice_t *v = &probe_voices[channel];
  v->started = director.ticks;
  if (m) {
    v->rate = m->rate; v->frames = m->samples; v->looping = m->looping;
    v->loop_start = m->loop_start;
    v->loop_end = m->loop_end < m->samples ? m->loop_end : m->samples;
    if (v->loop_start >= v->loop_end) { v->loop_start = 0; v->loop_end = v->frames; }
  }
  sound_count++;
}
static bool busy(void *ctx, unsigned channel) {
  (void)ctx;
  return channel < DG_SOUND_CHANNELS && probe_voices[channel].rate &&
      probe_voices[channel].frames &&
      (probe_voices[channel].looping || probe_sound_frame(channel) < probe_voices[channel].frames);
}
static inline unsigned position(void *ctx, unsigned channel) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || !probe_voices[channel].rate) return 0;
  uint64_t frame = probe_sound_frame(channel);
  if (frame > probe_voices[channel].frames) frame = probe_voices[channel].frames;
  return (unsigned)(frame * 1000 / probe_voices[channel].rate);
}
static void sound_pause(void *ctx, unsigned channel, bool paused) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || probe_voices[channel].paused == paused ||
      (paused && !busy(NULL, channel))) return;
  probe_voice_t *v = &probe_voices[channel];
  if (paused) v->paused_at = director.ticks;
  else v->started += director.ticks - v->paused_at;
  v->paused = paused;
}
static void sound_break_loop(void *ctx, unsigned channel) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || !probe_voices[channel].looping) return;
  probe_voice_t *v = &probe_voices[channel];
  v->position = probe_sound_frame(channel);
  v->started = v->paused ? v->paused_at : director.ticks;
  v->looping = false;
}

#if DG_EXTENDED
#define PROBE_AUDIO_POSITION .sound_position=position,
#else
#define PROBE_AUDIO_POSITION
#endif
