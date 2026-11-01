#ifndef DIRECTOR64_WEB_COMPOSITOR_H
#define DIRECTOR64_WEB_COMPOSITOR_H
// The software compositor the web and Android players share: the stage,
// drawn by the CPU into a 640x480 RGBA8888 framebuffer (800x600 with
// DIRECTOR64_NATIVE_STAGE) that the web page presents on a canvas and the
// Android player on its surface. Both hosts draw the same Director state from
// the same converted images through it, so they show the same game.
#include "director.h"
#if defined(DIRECTOR64_SOURCE_CURSOR_MASK) && DIRECTOR64_SOURCE_CURSOR_MASK
#include "cursor_resource.h"
#endif
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
  unsigned face; // bold1, italic2, underline4, strikeout8.
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
#if DG_MODERN
  // Initial styled text, as straight-alpha RGBA, with shared character layout.
  bool (*rich_text)(void *,const char *,int,int,const dg_text_format_t *,unsigned,uint32_t *);
  int (*rich_layout)(void *,const char *,int,int,const dg_text_format_t *,unsigned,int16_t *,int);
#endif
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
enum { WC_INVERSE_SCAN_RATIO = 4 };

typedef struct {
  uint64_t pixels, sampled_pixels;
  unsigned channel, member, frame;
  char movie[40];
} wc_inverse_work_t;

// Deterministic raster work, independent of host CPU speed. Counters are
// updated per row/draw, outside the arithmetic for individual pixels.
typedef struct {
  uint64_t frames, scene_frames, bitmap_calls, bitmap_pixels, inverse_pixels;
  uint64_t translated_pixels, translated_bounds_pixels;
  uint64_t peak_frame_pixels, peak_frame_inverse_pixels;
  uint64_t worst_inverse_pixels, worst_inverse_samples;
  unsigned worst_channel, worst_member, worst_frame;
  char worst_movie[40];
  // Largest rejected area among draws exceeding the scan-ratio threshold.
  // A larger, legitimate cropped draw must not hide an inefficient small one.
  wc_inverse_work_t wasteful_inverse;
  unsigned peak_frame_loads, peak_frame_evictions;
  unsigned peak_frame_pressure_evictions, peak_cache_pressure_run;
  size_t peak_image_bytes;
  uint64_t render_us, peak_render_us;
  unsigned peak_render_frame;
  char peak_render_movie[40];
} wc_work_t;

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
  uint32_t *rgba;
} wc_text_t;

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
#if defined(DIRECTOR64_SOURCE_CURSOR_MASK) && DIRECTOR64_SOURCE_CURSOR_MASK
  wc_resource_cursor_t resource_cursor;
#endif
  unsigned serial;
  // What the last composite showed; an unchanged stage is not redrawn.
  bool drawn;
  bool cache_disabled; // Validation: recomposite scenes and derived planes.
  bool film_clip_disabled; // Validation: retain the original full-wrapper scan.
  const dg_runtime_t *owner;
  const dg_movie_t *movie;
  unsigned revision;
  dg_cursor_t cursor;
  int pointer_x, pointer_y;
  bool pointer_shown;
  uint32_t background;
  // What else the composite shows that the stage revision does not track:
  // the host's decoded video frames, a D5 alert or source dialog.
  unsigned externals;
  // Counters for the page's diagnostics.
  unsigned loads, evictions, renders, scene_renders, plane_hits, plane_builds;
  unsigned prefix_hits, prefix_builds;
  wc_work_t work;
  uint64_t frame_pixels, frame_inverse_pixels;
  unsigned work_channel;
  unsigned pressure_evictions, cache_pressure_run;
  // The last composited scene without its cursors, and the snapshot of it a
  // script kept as a member's image (`member.image = (the stage).image`).
  uint32_t scene[WC_WIDTH * WC_HEIGHT];
  uint32_t *transition_previous, *transition_next;
  uint32_t transition_serial, transition_tick;
  int transition_l, transition_t, transition_r, transition_b;
  unsigned transition_cell_w, transition_cell_h, transition_bit_group;
  unsigned transition_grid_w, transition_grid_h;
  uint32_t transition_polynomial, transition_random_state;
  uint32_t transition_random_done, transition_random_total;
#if DG_D10
  uint32_t *film_scratch[8];
  unsigned film_depth;
  dg_film_transform_t film_transform[8];
  unsigned film_transform_count;
  wc_image_t stage_snapshot;
  uint32_t snapshot_member;
#endif
} wc_t;

#if defined(DIRECTOR64_SOURCE_CURSOR_MASK) && DIRECTOR64_SOURCE_CURSOR_MASK
// Read-only asset metadata for source cursor rendering and probe observations.
const wc_resource_cursor_t *wc_source_cursor(wc_t *,lv_runtime_t *,unsigned resource);
#endif

void wc_init(wc_t *, wc_host_t, size_t image_budget);
void wc_release(wc_t *);
// Keep immutable decoded assets; bypass scene/derived-plane reuse for an
// independent rendering comparison without changing playback or trails.
void wc_cache_enabled(wc_t *, bool enabled);
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
bool wc_member_resident(wc_t *,dg_runtime_t *,uint32_t member,bool load);
bool wc_flash_field_hit(wc_t *, dg_runtime_t *, struct dg_flash_instance *, float, float);
#endif
// Which character of a text member's `text`, set in a width x height box,
// lies at (x, y): 0-based in Unicode characters, -1 for none (the host's
// layout callback places them).
int wc_text_char_at(wc_t *, dg_runtime_t *, const dg_member_t *, const char *text, int width, int height, int x,
                    int y);
bool wc_text_position(wc_t *,dg_runtime_t *,const dg_member_t *,const char *text,
                        int width,int position,int *x,int *y);
#if DG_CAP_TEXT_BOX
int wc_text_height(wc_t *,dg_runtime_t *,const dg_member_t *,const char *text,int width);
#endif
bool wc_render(wc_t *, dg_runtime_t *, int pointer_x, int pointer_y,
               bool pointer_shown, unsigned externals);
// OS-native cursor role, or -1 when source artwork/software composition is needed.
// 0 arrow, 1 I-beam, 2/3 crosshair, 4 wait, 200 hidden.
int wc_native_cursor(dg_runtime_t *);
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
