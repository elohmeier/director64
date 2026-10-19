#ifndef DIRECTOR64_WEB_COMPOSITOR_H
#define DIRECTOR64_WEB_COMPOSITOR_H
// The software compositor the web and Android players share: the stage,
// drawn by the CPU into a 640x480 RGBA8888 framebuffer (800x600 with
// DIRECTOR64_NATIVE_STAGE) that the web page presents on a canvas and the
// Android player on its surface. Both hosts draw the same Director state from
// the same converted images through it, so they show the same game.
#include "director.h"
#include "film_transform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(DIRECTOR64_NATIVE_STAGE) && DIRECTOR64_NATIVE_STAGE
// The authored 800x600 stage, unscaled (ink.h).
enum { WC_WIDTH = 800, WC_HEIGHT = 600 };
#else
enum { WC_WIDTH = 640, WC_HEIGHT = 480 };
#endif

// Everything the compositor needs from its host. Assets are converted images
// named as the cast records them ("<hash>.fdi"); the host never hands out a
// path. `read` returns the asset's length (0 when missing) and copies at most
// `cap` bytes when `out` is non-NULL. `text` rasterizes one text box into a
// width*height coverage plane and returns false when the host has no font.

// How a text box is set: the member's authored style, or NULL for the
// builtin monospace font. font is the model's font number (0: none
// recovered); align 0 left, 1 center, 2 right.
typedef struct {
  unsigned font, size, align;
  int ascent, line_height;
} wc_text_style_t;

typedef struct {
  uint32_t (*read)(void *ctx, const char *name, uint8_t *out, uint32_t cap);
  bool (*text)(void *ctx, const char *utf8, int width, int height,
               const wc_text_style_t *style, uint8_t *coverage);
  // Where `text` set each character of the same box: x, y, width, height
  // per character (one per Unicode character of the UTF-8), at most
  // max_chars of them. Returns how many it placed, 0 without a layout.
  // Optional; text colour runs and hit tests need it.
  int (*layout)(void *ctx, const char *utf8, int width, int height,
                const wc_text_style_t *style, int16_t *boxes, int max_chars);
  void *ctx;
  // The digital video frame a sprite shows now (D5), as RGBA8888 in `out`
  // (at most `cap` pixels); returns false while none is decoded. Optional.
  bool (*video_frame)(void *ctx, const dg_runtime_t *owner, unsigned sprite, uint32_t *out, uint32_t cap,
                      unsigned *width, unsigned *height);
} wc_host_t;

// A decoded image: linear RGBA8888 (r in the low byte), the ink's coverage
// already applied to alpha.
typedef struct {
  char name[40];
  unsigned ink, width, height, depth;
  unsigned stride; // pixels per row, zero for tightly packed images
  // A cropped plane keeps the original sampling space. Offsets locate the
  // stored pixels inside it; zero source dimensions mean an uncropped image.
  unsigned source_width, source_height;
  int source_x, source_y;
  bool follow_alpha, force_opaque, rgb555, rich_text, palette_brush;
  bool coverage_scanned, binary_coverage;
#if DG_D10
  const dg_member_t *linked_member; // crop/center mapping for a nested scene
#endif
  uint32_t *pixels;
  const uint8_t *coverage; // optional independent Imaging Lingo matte
  size_t bytes;
  unsigned last;
  // The stored registration point: a trimmed film-loop pose places itself by
  // it (draw_sprite).
  int reg_x, reg_y;
  // The rectangle holding any coverage: draws skip what lies outside it.
  unsigned cover_l, cover_t, cover_r, cover_b;
} wc_image_t;

enum { WC_IMAGE_SLOTS = 512, WC_TEXT_SLOTS = 32, WC_CURSOR_SLOTS = 16 };

// A colour run of a text box: from character `offset` on, colour `rgb`
// (the compositor's pixel colour).
typedef struct {
  uint32_t offset, rgb;
} wc_text_run_t;

typedef struct {
  uint32_t key;
  int width, height;
  uint8_t *coverage;
  unsigned last;
  // Per pixel, which colour run set it (0: the box's own colour), built
  // when the text has runs.
  uint8_t *runs;
} wc_text_t;

// One cursor on the stage: where it points and the ink its glyph's black
// body takes (dg_cursor_ink; 1 keeps it black). Controllers each draw one,
// in their player's colour.
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
  uint32_t *target; // temporary film target; NULL selects the stage
  wc_image_t images[WC_IMAGE_SLOTS];
  struct wc_cached_plane *planes;
  size_t image_bytes, image_budget;
  wc_text_t texts[WC_TEXT_SLOTS];
  wc_cursor_t cursors[WC_CURSOR_SLOTS];
  unsigned serial;
  // What the last composite showed; an unchanged stage is not redrawn.
  bool drawn;
  const dg_runtime_t *owner;
  const dg_movie_t *movie;
  unsigned revision;
  dg_cursor_t cursor;
  wc_pointer_t pointers[WC_POINTERS];
  unsigned pointer_count;
  uint32_t background;
  // What else the composite shows that the stage revision does not track:
  // the host's decoded video frames, a D5 alert or source dialog.
  unsigned externals;
  // Counters for the page's diagnostics.
  unsigned loads, evictions, renders, scene_renders, plane_hits, plane_builds;
  unsigned prefix_hits, prefix_builds;
  // The last composited scene without its cursors, and the snapshot of it a
  // script kept as a member's image (`member.image = (the stage).image`).
  uint32_t scene[WC_WIDTH * WC_HEIGHT];
#if DG_D10
  uint32_t *film_scratch[8];
  unsigned film_depth;
  dg_film_transform_t film_transform[8];
  unsigned film_transform_count;
  wc_image_t stage_snapshot;
  uint32_t snapshot_member;
#endif
} wc_t;

void wc_init(wc_t *, wc_host_t, size_t image_budget);
void wc_release(wc_t *);
// Decodes (or finds) a member's image under an ink. NULL with the runtime
// failed on an invalid or missing asset.
// width and height 0 take the stored image's own size (trimmed poses).
const wc_image_t *wc_image(wc_t *, lv_runtime_t *, const char *asset,
                           unsigned width, unsigned height, unsigned ink);
// The platform hit test: matte ink answers from the image's coverage, every
// other ink from the rectangle the caller already checked.
bool wc_hit(wc_t *, lv_runtime_t *, const dg_member_t *, unsigned ink, int x,
            int y);
// Composites the stage and the pointer. Returns false when nothing changed
// since the last composite (the framebuffer still holds it).
#if DG_D10
// Keeps what the stage shows now (without cursors) as the member's image.
void wc_capture_stage(wc_t *, uint32_t member);
bool wc_read_image(wc_t *, dg_runtime_t *, uint32_t member, dg_pixel_image_t *);
bool wc_flash_field_hit(wc_t *, dg_runtime_t *, struct dg_flash_instance *, float, float);
#endif
// Which character of a text member's `text`, set in a width x height box,
// lies at (x, y): 0-based in Unicode characters, -1 for none (the host's
// layout callback places them).
int wc_text_char_at(wc_t *, dg_runtime_t *, const dg_member_t *, const char *text, int width, int height, int x,
                    int y);
bool wc_render(wc_t *, dg_runtime_t *, int pointer_x, int pointer_y,
               bool pointer_shown, unsigned externals);
// The same with up to WC_POINTERS cursors, drawn in order (the last on top).
bool wc_render_pointers(wc_t *, dg_runtime_t *, const wc_pointer_t *,
                        unsigned count, unsigned externals);
// A member's cursor bitmap, as the probe state line's cursor hash reads it
// (state.inc calls it as native_cursor_bitmap); fails the runtime on a bad
// asset.
bool wc_cursor_bitmap(wc_t *, lv_runtime_t *, const dg_member_t *m,
                      const dg_member_t *mask, dg_cursor_bitmap_t *out);
// Converts one stored image to RGBA8888, applying an ink. Exposed for the
// contract tests.
bool wc_decode(const uint8_t *data, uint32_t length, unsigned width,
               unsigned height, unsigned ink, uint32_t *out,
               bool *follow_alpha);
#endif
