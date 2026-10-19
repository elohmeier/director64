// The pinned, MIT-licensed PL_MPEG decoder behind a small decoder handle, for
// the media-repair tests (tests/test_mpeg_picture.py) to decode MPEG-1 movies.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

typedef struct {
  plm_video_t *video;
  plm_audio_t *audio;
  const uint8_t *source;
  size_t source_size, source_offset;
  uint8_t *picture;
  int16_t samples[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
  double time;
} d64_mpeg_t;

static void load_chunk(plm_buffer_t *buffer, void *context) {
  d64_mpeg_t *m = context;
  size_t size = m->source_size - m->source_offset;
  if (size > 65536) size = 65536;
  if (size) {
    plm_buffer_write(buffer, (uint8_t *)m->source + m->source_offset, size);
    m->source_offset += size;
  }
  if (m->source_offset == m->source_size) plm_buffer_signal_end(buffer);
}

void d64m_close(d64_mpeg_t *m) {
  if (!m) return;
  if (m->video) plm_video_destroy(m->video);
  if (m->audio) plm_audio_destroy(m->audio);
  free(m->picture);
  free(m);
}

d64_mpeg_t *d64m_open(uint8_t *bytes, unsigned length, int audio) {
  d64_mpeg_t *m = calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->source = bytes;
  m->source_size = length;
  plm_buffer_t *buffer = plm_buffer_create_with_capacity(65536);
  plm_buffer_set_load_callback(buffer, load_chunk, m);
  if (audio) m->audio = plm_audio_create_with_buffer(buffer, 1);
  else m->video = plm_video_create_with_buffer(buffer, 1);
  if (audio ? !plm_audio_has_header(m->audio) : !plm_video_has_header(m->video)) {
    d64m_close(m);
    return NULL;
  }
  if (!audio) {
    int width = plm_video_get_width(m->video), height = plm_video_get_height(m->video);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        !plm_video_get_framerate(m->video)) {
      d64m_close(m);
      return NULL;
    }
    m->picture = malloc((size_t)width * height * 4);
    if (!m->picture) {
      d64m_close(m);
      return NULL;
    }
  }
  return m;
}
unsigned d64m_width(d64_mpeg_t *m) { return plm_video_get_width(m->video); }
unsigned d64m_height(d64_mpeg_t *m) { return plm_video_get_height(m->video); }
double d64m_time(d64_mpeg_t *m) { return m->time; }
unsigned d64m_corrupt(d64_mpeg_t *m) { return plm_video_get_corrupt_slices(m->video); }
unsigned d64m_audio_streams(d64_mpeg_t *m) { return m->audio != NULL; }
unsigned d64m_sample_rate(d64_mpeg_t *m) { return plm_audio_get_samplerate(m->audio); }
uint8_t *d64m_frame(d64_mpeg_t *m) {
  plm_video_t *video = m->video;
  plm_frame_t *frame = plm_video_decode(video);
  // The lookahead that establishes EOF can leave the final picture pending.
  if (!frame && video->start_code == PLM_START_PICTURE && plm_video_has_ended(video))
    frame = plm_video_decode(video);
  // A stream ending in B pictures still has its final I/P reference waiting
  // for display. Upstream only flushes it when the last coded picture is I/P.
  if (!frame && video->has_reference_frame && plm_video_has_ended(video)) {
    video->has_reference_frame = FALSE;
    frame = &video->frame_backward;
    frame->time = video->time;
  }
  if (!frame) return NULL;
  // Include odd-sized edge pixels too (the upstream RGB helper rounds down
  // to chroma pairs). Planes include macroblock padding; output does not.
  for (unsigned y = 0; y < frame->height; y++) {
    for (unsigned x = 0; x < frame->width; x++) {
      size_t c = (y / 2) * frame->cb.width + x / 2;
      int luma = ((frame->y.data[y * frame->y.width + x] - 16) * 76309) >> 16;
      int cr = frame->cr.data[c] - 128, cb = frame->cb.data[c] - 128;
      size_t at = ((size_t)y * frame->width + x) * 4;
      m->picture[at] = plm_clamp(luma + ((cr * 104597) >> 16));
      m->picture[at + 1] = plm_clamp(luma - ((cb * 25674 + cr * 53278) >> 16));
      m->picture[at + 2] = plm_clamp(luma + ((cb * 132201) >> 16));
      m->picture[at + 3] = 255;
    }
  }
  m->time = frame->time;
  return m->picture;
}
int16_t *d64m_samples(d64_mpeg_t *m) {
  plm_samples_t *samples = plm_audio_decode(m->audio);
  if (!samples) return NULL;
  for (unsigned i = 0; i < samples->count * 2; i++) {
    float sample = samples->interleaved[i] * 32768;
    m->samples[i] = sample <= -32768 ? -32768 : sample >= 32767 ? 32767 : (int16_t)lrintf(sample);
  }
  return m->samples;
}
