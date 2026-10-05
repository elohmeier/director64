#ifndef DIRECTOR64_WEB_RUNTIME_H
#define DIRECTOR64_WEB_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
#include "compositor.h"

// The console's 128 KiB FlashRAM; every adapter's archive is laid out in it.
#define WEB_FLASH_BYTES (128u * 1024u)
// Decoded images the compositor keeps, as RGBA8888. The console holds 3 MiB of
// 16-bit texels; a browser tab can afford every scene's working set.
#define WEB_IMAGE_BUDGET (192u * 1024u * 1024u)

// The embedder's services: the page (site/app.js) or the Node probe harness.
typedef struct {
  // Converted asset bytes by cast name; returns the length, 0 when missing.
  uint32_t (*asset)(void *ctx, const char *name, uint8_t *out, uint32_t cap);
  // Coverage of one text box, width*height bytes, set in `style` (NULL: the
  // builtin font); false without a font.
  bool (*text)(void *ctx, const char *utf8, int width, int height,
               const wc_text_style_t *style, uint8_t *coverage);
  // Start (asset non-NULL) or stop a channel.
  void (*sound)(void *ctx, unsigned channel, const char *asset, bool looping,
                uint32_t loop_start, uint32_t loop_end);
  void (*gain)(void *ctx, unsigned channel, float gain);
  // The flash image holds a new save generation (ok) or a failed commit.
  void (*save_commit)(void *ctx, bool ok);
  void (*trace)(void *ctx, const char *text);
  // Start a playFile stream by its virtual "folder/stem" name (NULL stops
  // the channel). Returns its length in milliseconds, 0 without such a
  // stream: the page answers the real length, the probe harness the native
  // probe's nominal second.
  uint32_t (*stream)(void *ctx, unsigned channel, const char *name);
  // A shipped read-only data file by its "folder/path" name; returns its
  // length, -1 when there is none.
  int32_t (*data)(void *ctx, const char *name, uint8_t *out, uint32_t cap);
  // D5 digital video, per sprite channel (platforms/n64/d5_video.c): the
  // member's video and sound assets ("" for none; NULL video closes the
  // player), its time in 600ths of a second, rate, a serial that changes on
  // every seek or loop, and the gain. Called each service step while the
  // sprite shows a video.
  void (*video)(void *ctx, unsigned sprite, const char *video, const char *audio,
                double time, double rate, unsigned serial, float gain);
  // The frame a sprite's player shows, RGBA8888; false while none decoded.
  bool (*video_frame)(void *ctx, unsigned sprite, uint32_t *out, uint32_t cap,
                      unsigned *width, unsigned *height);
  // Changes whenever a player has decoded a new frame.
  unsigned (*video_revision)(void *ctx);
  void *ctx;
} web_host_t;

void web_runtime_host(web_host_t);
#endif
