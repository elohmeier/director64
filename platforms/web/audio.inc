// Shared host audio timing, exercised directly by native contracts.
// Sounds started since the module loaded; like the native probe's, it
// survives a reboot.
static unsigned sound_count;
static unsigned busy_until[DG_SOUND_CHANNELS];
static unsigned sound_paused_at[DG_SOUND_CHANNELS];
static bool sound_paused[DG_SOUND_CHANNELS];
static struct {
  uint32_t started, rate, frames, start, end;
  uint64_t position;
  bool looping;
} sound_loops[DG_SOUND_CHANNELS];
#if DG_EXTENDED
static unsigned sound_started[DG_SOUND_CHANNELS];
#endif

// Channel lifetime follows the service clock, as the native probe's does: a
// sound is busy for its authored length in ticks, looping sounds until they
// are replaced. The score's waits on a sound therefore end on the same tick
// here as on the probe, and the page's audio plays alongside that clock.
static void sound(void *ctx, unsigned channel, const dg_member_t *m) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS) {
    lv_fail(&values, "native audio channel");
    return;
  }
  sound_paused[channel] = false;
  sound_loops[channel].started = director.ticks;
  sound_loops[channel].position = 0;
  sound_loops[channel].looping = m && m->looping;
  sound_loops[channel].rate = m ? m->rate : 0;
  sound_loops[channel].frames = m ? m->samples : 0;
  sound_loops[channel].start = m ? m->loop_start : 0;
  sound_loops[channel].end = m && m->loop_end < m->samples ? m->loop_end : m ? m->samples : 0;
  if (sound_loops[channel].start >= sound_loops[channel].end) {
    sound_loops[channel].start = 0;
    sound_loops[channel].end = sound_loops[channel].frames;
  }
  busy_until[channel] =
      director.ticks +
      (m && m->rate ? (uint32_t)(((uint64_t)m->samples * 60 + m->rate - 1) / m->rate) : 0);
  if (m && m->looping)
    busy_until[channel] = UINT32_MAX;
#if DG_EXTENDED
  sound_started[channel] = director.ticks;
#endif
  sound_count++;
  if (!host.sound)
    return;
  if (!m || !m->asset || !*m->asset)
    host.sound(host.ctx, channel, NULL, 0, 0, 0);
  else
    host.sound(host.ctx, channel, m->asset, m->looping, m->loop_start,
               m->loop_end);
}
static bool sound_busy(void *ctx, unsigned channel) {
  (void)ctx;
  return channel < DG_SOUND_CHANNELS &&
         (sound_paused[channel] || director.ticks < busy_until[channel]);
}
static void sound_pause(void *ctx, unsigned channel, bool paused) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || sound_paused[channel] == paused ||
      (paused && !sound_busy(NULL, channel)))
    return;
  if (paused) {
    sound_paused_at[channel] = director.ticks;
  } else {
    unsigned held = director.ticks - sound_paused_at[channel];
    sound_loops[channel].started += held;
    if (busy_until[channel] != UINT32_MAX)
      busy_until[channel] += held;
#if DG_EXTENDED
    sound_started[channel] += held;
#endif
  }
  sound_paused[channel] = paused;
  if (host.sound_pause)
    host.sound_pause(host.ctx, channel, paused);
}
static uint64_t sound_frame(unsigned channel, unsigned now) {
  uint64_t position = sound_loops[channel].position +
      (uint64_t)(now - sound_loops[channel].started) * sound_loops[channel].rate / 60;
  uint32_t end = sound_loops[channel].end, start = sound_loops[channel].start;
  if (sound_loops[channel].looping && position >= end && end > start)
    position = start + (position - start) % (end - start);
  return position;
}
static void sound_break_loop(void *ctx, unsigned channel) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || !sound_loops[channel].looping) return;
  if (host.sound && !host.sound_break_loop) {
    lv_fail(&values, "host sound loop release service unavailable");
    return;
  }
  unsigned now = sound_paused[channel] ? sound_paused_at[channel] : director.ticks;
  uint32_t rate = sound_loops[channel].rate, frames = sound_loops[channel].frames;
  uint64_t position = sound_frame(channel, now);
  busy_until[channel] = now + (rate && position < frames ?
      (uint32_t)(((frames - position) * 60 + rate - 1) / rate) : 0);
  sound_loops[channel].looping = false;
  sound_loops[channel].position = position;
  sound_loops[channel].started = now;
  if (host.sound_break_loop) host.sound_break_loop(host.ctx, channel);
}
#if DG_EXTENDED
// How far a channel's sound has played, in milliseconds, for cue points. Like
// channel lifetime it follows the service clock, as on the native probe.
static unsigned sound_position(void *ctx, unsigned channel) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS)
    return 0;
  unsigned now = sound_paused[channel] ? sound_paused_at[channel] : director.ticks;
  if (sound_loops[channel].rate) {
    uint64_t position = sound_frame(channel, now);
    if (position > sound_loops[channel].frames) position = sound_loops[channel].frames;
    return (unsigned)(position * 1000 / sound_loops[channel].rate);
  }
  return now > sound_started[channel] ? (now - sound_started[channel]) * 1000u / 60u : 0;
}
#endif
