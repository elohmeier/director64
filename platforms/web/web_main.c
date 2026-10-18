// Emscripten entry: the runtime host's services are the embedder's JavaScript
// (library.js forwards each call to Module.director64Host).
#include "web_runtime.h"
#include <emscripten.h>

extern uint32_t d64_host_asset(const char *name, uint8_t *out, uint32_t cap);
extern int d64_host_text(const char *text, int width, int height, int font,
                         int size, int align, int ascent, int line_height,
                         uint8_t *coverage);
extern int d64_host_layout(const char *text, int width, int height, int font,
                           int size, int align, int ascent, int line_height,
                           int16_t *boxes, int max_chars);
extern void d64_host_sound(unsigned channel, const char *asset, int looping,
                           uint32_t loop_start, uint32_t loop_end);
extern void d64_host_sound_pause(unsigned channel, int paused);
extern void d64_host_sound_break_loop(unsigned channel);
extern void d64_host_gain(unsigned channel, float gain);
extern void d64_host_save_commit(int ok);
extern void d64_host_trace(const char *text);
extern uint32_t d64_host_stream(unsigned channel, const char *name);
extern int32_t d64_host_data(const char *name, uint8_t *out, uint32_t cap);
extern void d64_host_video(unsigned sprite, const char *video, const char *audio,
                           double time, double rate, unsigned serial, float gain);
extern int d64_host_video_frame(unsigned sprite, uint32_t *out, uint32_t cap,
                                unsigned *width, unsigned *height);
extern unsigned d64_host_video_revision(void);
#if DG_D10
extern void d64_host_flash_sound(uint32_t id,const char *asset,unsigned rate,unsigned frames,
    unsigned start,unsigned end,unsigned loops,double position,float gain,int paused,
    const dg_flash_envelope_t *envelopes,unsigned count);
static void flash_sound(void *ctx,uint32_t id,const dg_flash_audio_t *v) {
  (void)ctx;
  d64_host_flash_sound(id,v?v->sound->asset:NULL,v?v->sound->rate:0,v?v->sound->samples:0,
      v?v->start_sample:0,v?v->end_sample:0,v?v->loops:0,v?(double)v->position:0,v?v->gain:0,
      v?v->paused:0,v&&v->event&&(v->event->flags&8)?v->event->envelopes:NULL,
      v&&v->event&&(v->event->flags&8)?v->event->envelope_count:0);
}
#endif

static uint32_t asset(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  return d64_host_asset(name, out, cap);
}
static bool text(void *ctx, const char *utf8, int width, int height,
                 const wc_text_style_t *style, uint8_t *coverage) {
  (void)ctx;
  // Font 0 with size 0 asks for the builtin font.
  return d64_host_text(utf8, width, height, style ? (int)style->font : 0,
                       style ? (int)style->size : 0,
                       style ? (int)style->align : 0,
                       style ? style->ascent : 0,
                       style ? style->line_height : 0, coverage) != 0;
}
static int layout(void *ctx, const char *utf8, int width, int height,
                  const wc_text_style_t *style, int16_t *boxes, int max_chars) {
  (void)ctx;
  return d64_host_layout(utf8, width, height, style ? (int)style->font : 0,
                         style ? (int)style->size : 0, style ? (int)style->align : 0,
                         style ? style->ascent : 0, style ? style->line_height : 0, boxes,
                         max_chars);
}
static void sound(void *ctx, unsigned channel, const char *name, bool looping,
                  uint32_t loop_start, uint32_t loop_end) {
  (void)ctx;
  d64_host_sound(channel, name, looping, loop_start, loop_end);
}
static void sound_pause(void *ctx, unsigned channel, bool paused) {
  (void)ctx;
  d64_host_sound_pause(channel, paused);
}
static void sound_break_loop(void *ctx, unsigned channel) {
  (void)ctx;
  d64_host_sound_break_loop(channel);
}
static void gain(void *ctx, unsigned channel, float value) {
  (void)ctx;
  d64_host_gain(channel, value);
}
static void save_commit(void *ctx, bool ok) {
  (void)ctx;
  d64_host_save_commit(ok);
}
static void trace(void *ctx, const char *line) {
  (void)ctx;
  d64_host_trace(line);
}

static uint32_t stream(void *ctx, unsigned channel, const char *name) {
  (void)ctx;
  return d64_host_stream(channel, name);
}
static int32_t data(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  return d64_host_data(name, out, cap);
}
static void video(void *ctx, unsigned sprite, const char *video, const char *audio,
                  double time, double rate, unsigned serial, float gain) {
  (void)ctx;
  d64_host_video(sprite, video, audio, time, rate, serial, gain);
}
static bool video_frame(void *ctx, unsigned sprite, uint32_t *out, uint32_t cap,
                        unsigned *width, unsigned *height) {
  (void)ctx;
  return d64_host_video_frame(sprite, out, cap, width, height) != 0;
}
static unsigned video_revision(void *ctx) {
  (void)ctx;
  return d64_host_video_revision();
}

int main(void) {
  web_runtime_host((web_host_t){.asset = asset, .text = text, .layout = layout,
                                .sound = sound, .sound_pause = sound_pause,
                                .sound_break_loop = sound_break_loop, .gain = gain,
                                .save_commit = save_commit, .trace = trace,
                                .stream = stream, .data = data, .video = video,
                                .video_frame = video_frame,
                                .video_revision = video_revision,
#if DG_D10
                                .flash_sound = flash_sound,
#endif
                                });
  return 0;
}
