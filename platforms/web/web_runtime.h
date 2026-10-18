#ifndef DIRECTOR64_WEB_RUNTIME_H
#define DIRECTOR64_WEB_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
#include "compositor.h"
#include "flash_runtime.h"

// The console's 128 KiB FlashRAM; every adapter's archive is laid out in it.
#define WEB_FLASH_BYTES (128u * 1024u)
// Decoded images the compositor keeps, as RGBA8888. The console holds 3 MiB of
// 16-bit texels; a browser tab can afford every scene's working set. A
// smaller device's build sets its own.
#ifndef WEB_IMAGE_BUDGET
#define WEB_IMAGE_BUDGET (192u * 1024u * 1024u)
#endif

// The embedder's services: the page (site/app.js) or the Node probe harness.
typedef struct {
  // Converted asset bytes by cast name; returns the length, 0 when missing.
  uint32_t (*asset)(void *ctx, const char *name, uint8_t *out, uint32_t cap);
  // Coverage of one text box, width*height bytes, set in `style` (NULL: the
  // builtin font); false without a font.
  bool (*text)(void *ctx, const char *utf8, int width, int height,
               const wc_text_style_t *style, uint8_t *coverage);
  // Each character's box in the text box `text` set (wc_host_t.layout).
  int (*layout)(void *ctx, const char *utf8, int width, int height,
                const wc_text_style_t *style, int16_t *boxes, int max_chars);
  // Start (asset non-NULL) or stop a channel.
  void (*sound)(void *ctx, unsigned channel, const char *asset, bool looping,
                uint32_t loop_start, uint32_t loop_end);
  void (*sound_pause)(void *ctx, unsigned channel, bool paused);
  void (*sound_break_loop)(void *ctx, unsigned channel);
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
  // Digital video, per runtime-assigned player ID (platforms/n64/d5_video.c): the
  // member's video and sound assets ("" for none; NULL video closes the
  // player), its time in 600ths of a second, rate, a serial that changes on
  // every seek or loop, and the gain. Called each service step while the
  // sprite shows a video. IDs distinguish a child movie's channels from its
  // parent's channels and remain stable for that child instance's lifetime.
  void (*video)(void *ctx, unsigned sprite, const char *video, const char *audio,
                double time, double rate, unsigned serial, float gain);
  // The frame a sprite's player shows, RGBA8888; false while none decoded.
  bool (*video_frame)(void *ctx, unsigned sprite, uint32_t *out, uint32_t cap,
                      unsigned *width, unsigned *height);
  // Changes whenever a player has decoded a new frame.
  unsigned (*video_revision)(void *ctx);
#if DG_D10
  // Independent SWF event voice. NULL stops it; repeated IDs update gain
  // and pause state without restarting. The snapshot is borrowed this call.
  void (*flash_sound)(void *ctx, uint32_t id, const dg_flash_audio_t *);
#endif
  void *ctx;
} web_host_t;

void web_runtime_host(web_host_t);
// Release the running engine and package while host services/assets are alive.
// Idempotent; a new host and package can then be installed in the same process.
void d64_shutdown(void);
// Engine event slots available to a single-owner input producer.
unsigned d64_key_capacity(void);
#endif
