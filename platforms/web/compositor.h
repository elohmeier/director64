#ifndef DIRECTOR64_WEB_COMPOSITOR_H
#define DIRECTOR64_WEB_COMPOSITOR_H
// The browser backend's software compositor: the stage the N64 draws with the
// RDP, drawn by the CPU into a 640x480 RGBA8888 framebuffer that the page
// presents on a canvas. It reads the same Director state and the same converted
// images as the console, so a difference between the two is a compositor
// difference, not a different game.
#include "director.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { WC_WIDTH = 640, WC_HEIGHT = 480 };

// Everything the compositor needs from its host. Assets are converted images
// named as the cast records them ("<hash>.fdi"); the host never hands out a
// path. `read` returns the asset's length (0 when missing) and copies at most
// `cap` bytes when `out` is non-NULL. `text` rasterizes one text box into a
// width*height coverage plane and returns false when the host has no font.

// How a text box is set: the member's authored style, or NULL for the
// console's builtin font. font is the model's font number (0: none
// recovered); align 0 left, 1 center, 2 right.
typedef struct {
  unsigned font, size, align;
  int ascent, line_height;
} wc_text_style_t;

typedef struct {
  uint32_t (*read)(void *ctx, const char *name, uint8_t *out, uint32_t cap);
  bool (*text)(void *ctx, const char *utf8, int width, int height,
               const wc_text_style_t *style, uint8_t *coverage);
  void *ctx;
  // The digital video frame a sprite shows now (D5), as RGBA8888 in `out`
  // (at most `cap` pixels); returns false while none is decoded. Optional.
  bool (*video_frame)(void *ctx, unsigned sprite, uint32_t *out, uint32_t cap,
                      unsigned *width, unsigned *height);
} wc_host_t;

// A decoded image: linear RGBA8888 (r in the low byte), the ink's coverage
// already applied to alpha, exactly as the console's cache holds it after its
// ink pass.
typedef struct {
  char name[40];
  unsigned ink, width, height;
  bool follow_alpha;
  uint32_t *pixels;
  size_t bytes;
  unsigned last;
} wc_image_t;

enum { WC_IMAGE_SLOTS = 512, WC_TEXT_SLOTS = 32, WC_CURSOR_SLOTS = 16 };

typedef struct {
  uint32_t key;
  int width, height;
  uint8_t *coverage;
  unsigned last;
} wc_text_t;

// One cursor on the stage: where it points and the ink its glyph's black
// body takes (dg_cursor_ink; 1 keeps it black). Controllers each draw one,
// in their player's colour, as the console does.
typedef struct {
  int x, y;
  uint32_t ink; // a 16-bit ink; the full word keeps the struct unpadded
} wc_pointer_t;
enum { WC_POINTERS = 4 };

typedef struct {
  dg_cursor_t cursor;
  dg_cursor_bitmap_t bitmap;
  char image[40], mask[40];
  unsigned last;
} wc_cursor_t;

typedef struct {
  wc_host_t host;
  uint32_t pixels[WC_WIDTH * WC_HEIGHT];
  wc_image_t images[WC_IMAGE_SLOTS];
  size_t image_bytes, image_budget;
  wc_text_t texts[WC_TEXT_SLOTS];
  wc_cursor_t cursors[WC_CURSOR_SLOTS];
  unsigned serial;
  // What the last composite showed; an unchanged stage is not redrawn.
  bool drawn;
  unsigned revision;
  dg_cursor_t cursor;
  wc_pointer_t pointers[WC_POINTERS];
  unsigned pointer_count;
  uint32_t background;
  // What else the composite shows that the stage revision does not track:
  // the host's decoded video frames, a D5 alert or source dialog.
  unsigned externals;
  // Counters for the page's diagnostics.
  unsigned loads, evictions, renders;
} wc_t;

void wc_init(wc_t *, wc_host_t, size_t image_budget);
void wc_release(wc_t *);
// Decodes (or finds) a member's image under an ink. NULL with the runtime
// failed on an invalid or missing asset, as the console fails.
const wc_image_t *wc_image(wc_t *, lv_runtime_t *, const char *asset,
                           unsigned width, unsigned height, unsigned ink);
// The platform hit test: matte ink answers from the image's coverage, every
// other ink from the rectangle the caller already checked.
bool wc_hit(wc_t *, lv_runtime_t *, const dg_member_t *, unsigned ink, int x,
            int y);
// Composites the stage and the pointer. Returns false when nothing changed
// since the last composite (the framebuffer still holds it).
bool wc_render(wc_t *, dg_runtime_t *, int pointer_x, int pointer_y,
               bool pointer_shown, unsigned externals);
// The same with up to WC_POINTERS cursors, drawn in order (the last on top).
bool wc_render_pointers(wc_t *, dg_runtime_t *, const wc_pointer_t *,
                        unsigned count, unsigned externals);
// A member's cursor bitmap, as the native probe's native_cursor_bitmap reads
// it (the probe state line's cursor hash); fails the runtime as it does.
bool wc_cursor_bitmap(wc_t *, lv_runtime_t *, const dg_member_t *m,
                      const dg_member_t *mask, dg_cursor_bitmap_t *out);
// Converts one stored image to RGBA8888, applying an ink as the console's
// loader does. Exposed for the contract tests.
bool wc_decode(const uint8_t *data, uint32_t length, unsigned width,
               unsigned height, unsigned ink, uint32_t *out,
               bool *follow_alpha);
#endif
