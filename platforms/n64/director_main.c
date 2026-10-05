#include "audio_bounds.h"
#include "bitmap_tiles.h"
#include "director.h"
#include "game.h"
#include "ink.h"
#include "pack.h"
#include "platform.h"
#include "pointer.h"
#include "shape.h"
#include "text_plain.h"
#ifdef DIRECTOR64_PRINTING
#include "printing.h"
#include "print_ui.h"
#endif
#if DG_CAP_TEXT_INPUT
#include "text_input.h"
#endif
#ifdef DIRECTOR64_WILLY
#include "willy_input.h"
#endif
#if DG_D8
#include "controls.h"
#endif
#if DG_D5
#include "d5_video.h"
#endif
#ifdef DIRECTOR64_FULL_REPLAY
#include "director_replay.h"
#endif
#include <dlfcn.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_CACHE_BYTES (3u * 1024u * 1024u)
#define IMAGE_CACHE_SLOTS 192
static const char *const global_names[] = {
#include "globals.inc"
};
static const char *const movie_names[] = {
#include "movie_names.inc"
};
// The cast files each movie opens, for the bundle's overlay budget and the
// D10 overlay lifetime.
#include "movie_casts.inc"
static lv_runtime_t *values;
static dg_runtime_t *director;
static pointer_control_t pointer;
static void *overlays[DG_FILES];
static unsigned overlay_count;
#if DG_D5
static unsigned overlay_ids[DG_FILES], window_overlay_base;
#elif DG_D10
// The controller window's movies, their cast archives and swapped cast
// targets stay resident across stage transitions; ids key the reuse cache.
static unsigned overlay_ids[DG_FILES];
#endif
static wav64_t voices[DG_SOUND_CHANNELS];
static bool voice_open[DG_SOUND_CHANNELS];
// A sound replaced or stopped while it is audible ramps to silence over a
// few milliseconds before the channel lets go of it: mixer_ch_stop cuts the
// waveform wherever it is, and a loud one cut far from zero is a click.
// The swap waits for the mixer to have played the ramp (the next audio
// service), so the replacement starts at most one buffer later; to the
// score the new sound is already the channel's.
enum { SOUND_FADE_SAMPLES = 128 };
typedef struct {
  // The gain last handed to each channel; negative until the loop sets one.
  float gain[DG_SOUND_CHANNELS];
  double from[DG_SOUND_CHANNELS];
  bool pending[DG_SOUND_CHANNELS];
  // The member to start, copied: its record and asset name live in the
  // movie overlay, which a scene change can unload before the swap.
  const dg_member_t *next[DG_SOUND_CHANNELS];
  dg_member_t member[DG_SOUND_CHANNELS];
  char asset[DG_SOUND_CHANNELS][64];
} sound_fades_t;
// Allocated once at boot rather than declared: the statics' placement in
// the data cache is tuned (platforms/n64/n64.ld), and a kilobyte inserted
// among them cost the station 15% of its render time.
static sound_fades_t *fades;
static bool audio_live, recording_block;
// Channels with a swap pending, so the audio service and the gain loop can
// skip the fade state entirely in the common case.
static uint16_t fading;
_Static_assert(DG_SOUND_CHANNELS <= 16, "fading is a 16-channel mask");
static rdpq_font_t *font;
// Render phase attribution: image cache traffic, glyph drawing (which the
// Flash overlay total also contains), and the draw-order sort. Every version
// reports these, so they must stay outside the styled-text guard below.
static uint64_t phase_image_us, phase_text_us, phase_flash_us, phase_order_us;
// Image cache traffic splits again into the hit scan and the miss path's
// stages, so a capture says which part of a load costs what.
static uint64_t image_scan_us, image_open_us, image_headroom_us,
    image_read_us, image_ink_us, image_mask_us;
static uint64_t heap_stats_us;
static unsigned heap_stats_calls;
// Scene entry splits into reaching the movie (drain, overlay dlopen, plane
// preload) and dg_enter's own setup, which runs the authored prepare/start
// handlers. Both land in a window's ticks_us.
static uint64_t enter_load_us, enter_script_us, enter_overlay_us;
static uint64_t enter_drain_us, enter_plane_us;
static unsigned enter_overlays;
// sys_get_heap_stats walks the whole malloc arena; count what that costs
// wherever a hot path asks for free bytes.
static void counted_heap_stats(heap_stats_t *stats) {
  uint64_t started = get_ticks_us();
  sys_get_heap_stats(stats);
  heap_stats_us += get_ticks_us() - started;
  heap_stats_calls++;
}
#if DG_MODERN
typedef struct {
  rdpq_font_t *font;
  unsigned last, color;
  uint8_t font_id, size;
  bool styled, drawn;
} cached_font_t;
// Four slots: a Flash-card scene draws its prompt overlays beside a field
// member and the toolbar text without reloading an atlas every frame.
static cached_font_t text_fonts[4];
enum { TEXT_FONT_BASE = 2, TEXT_FONT_SLOTS = 4 }; // 1: debug
static bool evict(void);
static bool evict_arena(void);
static unsigned font_serial;
static int source_state_id = -1;
static char source_state[64];

static void observe_source_state(void) {
  if (source_state_id < 0)
    return;
  lv_t value = values->globals[source_state_id];
  if (lv_type(value) != LV_STRING && lv_type(value) != LV_SYMBOL)
    return;
  const char *state = lv_cstr(values, value);
  if (strcmp(state, source_state)) {
    snprintf(source_state, sizeof(source_state), "%s", state);
    debugf("DIRECTOR64 NATIVE_SOURCE_STATE movie=%s name=%s "
           "value=%s tick=%lu\n", director->movie->code->name,
           global_names[source_state_id], source_state,
           (unsigned long)director->ticks);
  }
}

static void clear_text_fonts(void) {
  // Font atlases and style blocks can still be referenced by queued draws.
  rspq_wait();
  for (unsigned i = 0; i < TEXT_FONT_SLOTS; i++) {
    if (text_fonts[i].font) {
      rdpq_text_unregister_font(i + TEXT_FONT_BASE);
      rdpq_font_free(text_fonts[i].font);
      memset(&text_fonts[i], 0, sizeof(text_fonts[i]));
    }
  }
}

static unsigned text_font(unsigned id, unsigned size, color_t color) {
  unsigned slot = 0;
  for (; slot < TEXT_FONT_SLOTS; slot++)
    if (text_fonts[slot].font && text_fonts[slot].font_id == id &&
        text_fonts[slot].size == size)
      break;
  if (slot == TEXT_FONT_SLOTS) {
    slot = 0;
    for (unsigned i = 1; i < TEXT_FONT_SLOTS; i++)
      if (!text_fonts[i].font ||
          (text_fonts[slot].font && text_fonts[i].last < text_fonts[slot].last))
        slot = i;
    cached_font_t *entry = &text_fonts[slot];
    rspq_wait();
    if (entry->font) {
      rdpq_text_unregister_font(slot + TEXT_FONT_BASE);
      rdpq_font_free(entry->font);
    }
    memset(entry, 0, sizeof(*entry));
    char path[64];
    snprintf(path, sizeof(path), "rom:/fonts/f%u-%u.font64", id, size);
    FILE *file = fopen(path, "rb");
    if (!file) {
      lv_fail(values, "missing authored text font");
      return 0;
    }
    fclose(file);
#if DG_CAP_HEAP_SWEEPS
    // The SDK's font loader asserts on allocation failure. Release
    // completed images first: an exercise screen's cast overlays can leave
    // the atlas only kilobytes otherwise.
    for (;;) {
      heap_stats_t stats;
      sys_get_heap_stats(&stats);
      if (stats.free >= 192 * 1024 || !evict_arena())
        break;
    }
#endif
    heap_stats_t before, after;
    sys_get_heap_stats(&before);
    entry->font = rdpq_font_load(path);
    sys_get_heap_stats(&after);
    entry->font_id = id;
    entry->size = size;
    rdpq_text_register_font(slot + TEXT_FONT_BASE, entry->font);
    debugf("DIRECTOR64 NATIVE_FONT_LOAD movie=%s font=%u size=%u "
           "heap_bytes=%d free=%d\n", director->movie->code->name, id, size,
           before.free - after.free, after.free);
  }
  cached_font_t *entry = &text_fonts[slot];
  unsigned rgba = (unsigned)color.r << 24 | (unsigned)color.g << 16 |
                  (unsigned)color.b << 8 | color.a;
  if (!entry->styled || entry->color != rgba) {
    // The pinned SDK copies style values into each draw's queued commands.
    rdpq_font_style(entry->font, 1, &(rdpq_fontstyle_t){.color = color});
    entry->styled = true;
    entry->color = rgba;
  }
  entry->last = ++font_serial;
  return slot + TEXT_FONT_BASE;
}

// Director text at rest keeps the single-byte windows-1252 bytes that the
// authored charToNum arithmetic and the task databases produce, while
// converted member text arrives as UTF-8; rdpq consumes only UTF-8. A byte
// string that does not scan as UTF-8 transcodes byte-onto-codepoint here,
// which matches the recovered PFR fonts' character assignments.
static bool text_is_utf8(const char *text) {
  const unsigned char *s = (const unsigned char *)text;
  while (*s) {
    if (*s < 0x80) {
      s++;
      continue;
    }
    unsigned extra = *s >= 0xf0 ? 3 : *s >= 0xe0 ? 2 : *s >= 0xc0 ? 1 : 0;
    if (!extra)
      return false;
    for (unsigned i = 1; i <= extra; i++)
      if ((s[i] & 0xc0) != 0x80)
        return false;
    s += extra + 1;
  }
  return true;
}
static char text_utf8_scratch[4096];
static const char *text_as_utf8(const char *text) {
  if (text_is_utf8(text))
    return text;
  unsigned at = 0;
  for (const unsigned char *s = (const unsigned char *)text;
       *s && at + 3 < sizeof(text_utf8_scratch); s++) {
    if (*s < 0x80)
      text_utf8_scratch[at++] = (char)*s;
    else {
      text_utf8_scratch[at++] = (char)(0xC0 | (*s >> 6));
      text_utf8_scratch[at++] = (char)(0x80 | (*s & 63));
    }
  }
  text_utf8_scratch[at] = 0;
  return text_utf8_scratch;
}
static void draw_text_uncounted(const dg_text_style_t *style,
                                const char *override_font,
                                const char *text, color_t color,
                                int l, int t, int r, int b, uint32_t member,
                                int scroll);
static void draw_text(const dg_text_style_t *style, const char *override_font,
                      const char *text, color_t color,
                      int l, int t, int r, int b, uint32_t member, int scroll) {
  uint64_t started = get_ticks_us();
  draw_text_uncounted(style, override_font, text, color, l, t, r, b, member,
                      scroll);
  phase_text_us += get_ticks_us() - started;
}
static void draw_text_uncounted(const dg_text_style_t *style,
                                const char *override_font,
                                const char *text, color_t color,
                                int l, int t, int r, int b, uint32_t member,
                                int scroll) {
  text = text_as_utf8(text);
  unsigned id = style ? style->font_id : 0;
  if (override_font && *override_font &&
      (!style || strcmp(override_font, style->font_name))) {
    // The recovered corpus assigns only the two aliases of embedded Pettson.
    id = !strcmp(override_font, "Pettson") || !strcmp(override_font, "Pettson *")
             ? 1 : 0;
  }
  unsigned draw_size = style ? style->size : 12;
  int draw_ascent = style ? style->ascent : 12;
  int line_height = style ? style->line_height : 12;
#if DG_D10
  // Glyphs render at the stage prescale: the converted font set carries a
  // 4/5-scaled variant of every authored size, and the vertical metrics
  // scale with the already-prescaled sprite bounds.
  draw_size = (draw_size * 4 + 2) / 5;
  draw_ascent = (draw_ascent * 4 + 2) / 5;
  line_height = (line_height * 4 + 2) / 5;
#endif
  unsigned selected = id ? text_font(id, draw_size, color) : 1;
  if (!selected || !*text || r <= l || b <= t || r <= 0 || b <= 0 ||
      l >= 640 || t >= 480)
    return;
  if (selected == 1) {
    rdpq_font_style(font, 1, &(rdpq_fontstyle_t){.color = color});
  }
  if (line_height <= 0)
    line_height = (int)draw_size;
  int baseline = t + draw_ascent - scroll;
  rdpq_set_scissor(l < 0 ? 0 : l, t < 0 ? 0 : t,
                   r > 640 ? 640 : r, b > 480 ? 480 : b);
  while (*text && baseline - draw_ascent < b) {
    const char *end = text;
    while (*end && *end != '\r' && *end != '\n')
      end++;
    if (end != text) {
      rdpq_textparms_t params = {.style_id = 1, .width = r - l,
          .align = style ? style->align : ALIGN_LEFT, .wrap = WRAP_NONE,
          .disable_aa_fix = true}; // Display filtering/AA is disabled.
#if DG_D5
      params.wrap = WRAP_WORD;
#endif
      // Feed literal UTF-8 spans: Director text must never be interpreted as
      // libdragon's $font/^style markup. A zero height keeps baseline explicit.
      // Allocate enough for every byte, so the builder cannot relocate this
      // pointer. Check it before end(): the pinned SDK assumes a nonempty line.
      unsigned capacity = (unsigned)(end - text) + 1;
      rdpq_paragraph_t *line = malloc(sizeof(*line) +
          sizeof(rdpq_paragraph_char_t) * (capacity + 1));
      if (!line) {
        lv_fail(values, "text layout allocation");
        break;
      }
      *line = (rdpq_paragraph_t){.capacity = capacity,
          .flags = RDPQ_PARAGRAPH_FLAG_MALLOC};
      rdpq_paragraph_builder_begin(&params, selected, line);
      rdpq_paragraph_builder_span(text, end - text);
      if (line->nchars) {
        line = rdpq_paragraph_builder_end();
        rdpq_paragraph_render(line, l, baseline);
#if DG_D5
        baseline += (int)line->advance_y;
#endif
        if (selected >= TEXT_FONT_BASE &&
            !text_fonts[selected - TEXT_FONT_BASE].drawn) {
          debugf("DIRECTOR64 NATIVE_TEXT_DRAW movie=%s member=%lu font=%u "
                 "size=%u glyphs=%d\n", director->movie->code->name,
                 (unsigned long)member, id, style ? style->size : 12,
                 line->nchars);
          text_fonts[selected - TEXT_FONT_BASE].drawn = true;
        }
      }
      rdpq_paragraph_free(line);
    }
    baseline += line_height;
    text = end;
    if (*text == '\r') {
      text++;
      if (*text == '\n')
        text++;
    } else if (*text == '\n')
      text++;
  }
  rdpq_set_scissor(0, 0, 640, 480);
}
#endif
#if DG_D10
// Prompt text written into a converted Flash member's named edit fields
// renders as an overlay above the flattened frame. Wrapping runs in authored
// member space with the field's measured advances, so the rendered lines and
// the textHeight the scripts already read agree.
static char flash_overlay_text[2048];
static void draw_flash_overlays(unsigned channel, const dg_sprite_t *s,
                                const dg_member_t *m) {
  if (!m || !m->flash_field_count)
    return;
  for (unsigned f = 0; f < m->flash_field_count; f++) {
    const dg_flash_field_t *field = &m->flash_fields[f];
    // A field the scripts have addressed reads its live state from the
    // object bag; an untouched one shows its authored initial text.
    lv_t bag = {0};
    for (unsigned n = 0; n < director->flash_object_count; n++)
      if (director->flash_objects[n].sprite == channel &&
          dg_flash_field_find(m, director->flash_objects[n].name) == field) {
        bag = values->roots[DG_FLASH_ROOT + n];
        break;
      }
    const char *text = field->text;
    int fx = field->x, fy = field->y, fw = field->width, fh = field->height;
    unsigned size = field->style->size;
    color_t color = RGBA32(field->style->color >> 16,
                           (field->style->color >> 8) & 255,
                           field->style->color & 255, 255);
    if (lv_type(bag) == LV_PROPLIST) {
      if (!lv_truth(values, lv_get(values, NULL, "_visible", bag)))
        continue;
      lv_t text_value = lv_get(values, NULL, "text", bag);
      if (lv_type(text_value) != LV_STRING)
        continue;
      text = lv_cstr(values, text_value);
      lv_t v;
      if (lv_type((v = lv_get(values, NULL, "_x", bag))) == LV_NUMBER) fx = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_y", bag))) == LV_NUMBER) fy = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_width", bag))) == LV_NUMBER) fw = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_height", bag))) == LV_NUMBER) fh = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "size", bag))) == LV_NUMBER && lv_numeric(v) > 0)
        size = (unsigned)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "color", bag))) == LV_COLOR) {
        unsigned rgb = (unsigned)lv_id(v);
        color = RGBA32(rgb >> 16, (rgb >> 8) & 255, rgb & 255, 255);
      }
    }
    if (!*text)
      continue;
    if (size != field->style->size && field->style->font_id) {
      // An authored runtime size without a packed variant falls back to the
      // descriptor size, visibly traced.
      char path[64];
      snprintf(path, sizeof(path), "rom:/fonts/f%u-%u.font64",
               field->style->font_id, (size * 4 + 2) / 5);
      FILE *probe = fopen(path, "rb");
      if (probe)
        fclose(probe);
      else {
        debugf("DIRECTOR64 FLASH_FONT_VARIANT_MISSING font=%u size=%u\n",
               field->style->font_id, size);
        size = field->style->size;
      }
    }
    dg_flash_field_wrap(field, size, text, fw, flash_overlay_text,
                        sizeof(flash_overlay_text));
    color.a = (uint8_t)(dg_opacity(s) * 255 / 100);
    dg_text_style_t style = *field->style;
    if (size != style.size) {
      style.ascent = (int16_t)((int)style.ascent * (int)size /
                               (style.size ? (int)style.size : 1));
      style.size = (uint8_t)size;
    }
    style.line_height =
        (int16_t)(size + (unsigned)(field->leading < 0 ? 0 : field->leading));
    int al, at, ar, ab;
    dg_sprite_bounds(director, s, &al, &at, &ar, &ab);
    float sx = m->width ? (float)(ar - al) / m->width : 1.0f;
    float sy = m->height ? (float)(ab - at) / m->height : 1.0f;
    // The SWF renderer keeps a two-pixel gutter inside the field bounds.
    int l = al + (int)((fx + field->margin_left + field->indent) * sx) + 2;
    int t = at + (int)(fy * sy) + 2;
    int r = al + (int)((fx + fw - field->margin_right) * sx) - 2;
    int b = at + (int)((fy + fh) * sy);
    l = dg_prescale_coord(l); t = dg_prescale_coord(t);
    r = dg_prescale_coord(r); b = dg_prescale_coord(b);
    draw_text(&style, NULL, flash_overlay_text, color, l, t, r, b, m->id, 0);
  }
}
#endif
typedef struct {
  char name[32];
  void *data;
  unsigned bytes, last;
  uint32_t alpha_offset;
  uint8_t ink;
  bool follow_alpha;
  // IMAGE_RGBA16, IMAGE_RGBA32, IMAGE_CI4 or IMAGE_CI8 (an FDIC image, ink.h,
  // with colors_m1 + 1 palette entries; its index plane starts at
  // dg_fdic_index_offset of that). The record keeps its size: the statics
  // after the cache map onto cache sets the runtimes were placed around.
  uint8_t format, colors_m1;
  // The record is a pinned bundle row: its pixels live at the bottom of the
  // scene's block and are never freed or evicted on their own.
  bool bundled;
  // Offset of data within the block's dynamic area, plus one; 0 means the
  // pixels came from the malloc arena.
  uint32_t region_offset;
#if DG_CAP_DRAW_BLOCKS
  rspq_block_t *block;
  int block_rect[4];
  unsigned block_opacity, block_bytes, loaded_serial;
#endif
} cached_image_t;
enum { IMAGE_RGBA16, IMAGE_RGBA32, IMAGE_CI4, IMAGE_CI8 };
static inline unsigned image_ci_bits(const cached_image_t *t) {
  return t->format == IMAGE_CI4 ? 4 : t->format == IMAGE_CI8 ? 8 : 0;
}
static inline uint32_t image_index_offset(const cached_image_t *t) {
  return dg_fdic_index_offset(t->colors_m1 + 1u);
}
static cached_image_t cache[IMAGE_CACHE_SLOTS];
// One hashed key per slot, 0 where the slot is free. Held apart from the
// entries so a lookup walks 768 contiguous bytes of integers instead of
// calling strcmp against a hundred and ninety-two names: every sprite drawn
// asks for its image on every frame, and that scan alone cost 57 seconds of
// an hour of station play.
static uint32_t cache_key[IMAGE_CACHE_SLOTS];
// Cached bytes that came from the malloc arena: the overflow cache, for
// images the scene's bundle does not hold.
static unsigned cache_arena_bytes;
// The image pack (runtime/director/pack.h) holds every image once, and per
// scene the directory of what the scene draws, best first. At a scene entry
// the console keeps the directory's first rows (BUNDLE_ROWS at most), takes
// one block for as many of them as the drained heap allows after the
// scene's overlays and BUNDLE_HEADROOM, and decompresses each image into
// its slot the first time the scene draws it. The block goes back whole at
// the next entry, so image churn never fragments the arena: what a reserved
// region and its compactor used to guarantee for one port, the block gives
// every port, sized to the scene. Rows past the block, and images no
// directory lists, load through the overflow cache above from their pack
// offsets, evicted by least recent use as before.
// The block is one size for the whole session, so every scene's block
// fills the hole the last one left. The size is the old cache's: the
// region Mucklas reserved, or the arena ceiling elsewhere, less what the
// first scene's overlays and the headroom need if the console has less.
// BUNDLE_HEADROOM is what the block leaves the arena for what a scene
// allocates while it runs: font atlases, recorded draw commands, a save
// generation, audio streams. D10 keeps no block at all: its scenes hold
// megabytes of cast overlays across scenes and load more mid-scene, and
// the arena cache with its headroom sweeps is the storage that fits
// around them; its directories still tell the loader where every image
// is in the pack.
enum {
  BUNDLE_ROWS = 256,
#if DG_D8
  // Mucklas's region ran an hour-long soak on this headroom.
  BUNDLE_HEADROOM = 512u * 1024u,
  IMAGE_BLOCK_BYTES = 3456u * 1024u,
#else
  // The other ports allocate more while a scene runs (Willy's car screens
  // record dozens of part images and edit text); at half a megabyte the
  // 10.DXR capture bottomed out at 145 KB free.
  BUNDLE_HEADROOM = 768u * 1024u,
  IMAGE_BLOCK_BYTES = IMAGE_CACHE_BYTES,
#endif
  IMAGE_BLOCK_MIN_BYTES = 1024u * 1024u,
  IMAGE_BLOCK_STEP = 128u * 1024u,
};
static unsigned bundle_block_size; // decided at the first scene with rows, never grown
#define BUNDLE_UNPLACED UINT32_MAX
typedef struct {
  dg_pack_entry_t entry;
  uint32_t slot; // offset into the block, or BUNDLE_UNPLACED
} bundle_row_t;
static FILE *pack_file;
static dg_pack_header_t pack_header;
static uint8_t *pack_scenes; // the scene table, DG_PACK_SCENE_BYTES per movie
static bundle_row_t *bundle_rows;
static uint32_t *bundle_keys; // one per row, contiguous for the lookup scan
static cached_image_t *bundle_images; // one record per row
// Row index plus one by key, open addressing over twice the rows: every
// sprite drawn asks for its image every frame, and a scan of 256 keys per
// ask was 2% of the train scene's data misses.
enum { BUNDLE_HASH = 512 };
static uint16_t bundle_hash[BUNDLE_HASH];
static inline unsigned bundle_hash_slot(uint32_t key) {
  return (key * 2654435761u) >> 23;
}
static uint8_t *bundle_block;
static unsigned bundle_count, bundle_resident, bundle_bytes;
static uint64_t bundle_us;
static void bundle_release(void);
static void bundle_enter(unsigned id, unsigned largest);
// The block is also the image region: the pinned rows sit at its bottom,
// decoded in place, and the dynamic area above them is the overflow
// cache's storage, a coalesced hole list with a compactor, so a request
// that fits by bytes can always be made to fit by address. Cycling image
// blocks through the malloc arena instead is what wedged the train
// station: 814 KB failing for 688 consecutive frames with 2.4 MB free and
// no hole that size. What the arena still lends is bounded, because arena
// bytes spent on pixels are bytes a scene cannot spend on recorded draw
// commands, font atlases or a save generation; at 384 KB the station's
// saves began failing for want of two contiguous 64 KiB blocks.
enum { IMAGE_ARENA_BUDGET = 64u * 1024u };
static unsigned image_arena_budget(void);
static uint8_t *image_region; // the block, while a scene holds one
static unsigned image_region_bytes, image_region_base; // base: the pinned rows' end
// The in-flight load, which has its bytes but no cache entry yet.
static uint32_t image_region_pending_offset, image_region_pending_bytes;
static uint64_t image_region_us, image_region_compact_us;
static unsigned image_region_compactions, image_region_moved, image_region_windows;
#define IMAGE_REGION_ACTIVE (image_region != NULL)
static void image_region_release(uint32_t offset, uint32_t bytes);
// Only the loop's audio service tops up the AI, and it holds 160 ms. Work
// that can run longer than that in one piece — moving the image block,
// decompressing and inking a platform strip, a save commit — calls this
// between its pieces, or the AI runs dry and the console plays a gap with
// a click at each edge.
static void audio_pump(void);
#if DG_CAP_DRAW_BLOCKS
static bool release_bitmap_blocks(void);
#endif
static unsigned cache_bytes, render_serial;
// One sprite skipped on a fragmented allocation; the next render repacks.
static bool image_alloc_soft_fail;
#if DG_EXTENDED
static unsigned compact_serial;
#endif
static cached_image_t *image(const dg_member_t *, unsigned);
// Largest allocatable block, probed by trial: the heap reports total free
// bytes only, and a scene fails on the largest hole, not on the total.
static uint64_t largest_block_us;
static unsigned largest_block_calls;
static unsigned largest_block(void) {
  uint64_t started = get_ticks_us();
  unsigned result = 0;
  // Start at the free total: the bundle wants the whole coalesced arena,
  // and a probe that started lower would cap it there.
  heap_stats_t stats;
  sys_get_heap_stats(&stats);
  unsigned first = stats.free > 0 ? (unsigned)stats.free & ~(32u * 1024 - 1) : 0;
  for (unsigned bytes = first; bytes >= 32 * 1024; bytes -= 32 * 1024) {
    void *probe = memalign(32, bytes);
    if (probe) {
      free(probe);
      result = bytes;
      break;
    }
  }
  largest_block_us += get_ticks_us() - started;
  largest_block_calls++;
  return result;
}

#if DG_CAP_CACHE_STATS
static unsigned cache_loads, cache_evictions, cache_retries, cache_peak;
static unsigned bitmap_block_bytes;
enum { BITMAP_BLOCK_BUDGET = 192 * 1024 };
static unsigned cache_load_bytes, cache_load_peak;
static int cache_min_free = 8 * 1024 * 1024;
#endif
static bool frame_cached, cached_overlay;
static unsigned cached_revision, cached_externals;
#if DG_MODERN
static unsigned cached_background;
#endif
static dg_cursor_t cached_cursor;
// Every player's cursor and highlight folded into one key: any of them moving
// has to compose a new frame, and none of them alone is worth its own field.
static unsigned cached_pointers;
static volatile unsigned completed_serial;
#if DG_EXTENDED
static volatile unsigned completed_frame;
static unsigned reported_frame;
#endif
#if DG_D5 || DG_EXTENDED
static unsigned scene_first_serial;
static bool scene_ready;
#endif
typedef struct {
  surface_t *surface;
  unsigned serial;
  // One texture per player: each cursor keeps its own pixels through RDP
  // completion, so several tinted cursors can be in flight at once.
  _Alignas(16) uint16_t cursor_pixels[POINTER_PLAYERS][256];
#if DG_EXTENDED
  unsigned frame;
#endif
  volatile bool active;
} display_job_t;
static display_job_t jobs[2];
#if DG_EXTENDED
/* Puppet-transition reveal (wipe right, edges-in square). The most recently
 * queued framebuffer is the RDP source for the still-covered region, so each
 * composite chains from the last one; the reveal is monotonic and no spare
 * capture surface is needed. The score keeps playing underneath, an
 * approximation of the original blocking transition. */
static surface_t *previous_frame;
static unsigned transition_consumed, transition_started;
static bool transition_active;
#endif
static uint64_t tick_us, render_us, audio_us, pointer_us;
static unsigned render_calls;
// Game time permanently lost at the service-backlog clamp, in microseconds.
// Monotonic: the pacing analyzer diffs it across NATIVE_COST reports.
static uint64_t dropped_us;
static bool alpha_draw_reported;
static bool flash_read(void *ctx, size_t offset, void *out, size_t length) {
  (void)ctx;
  audio_pump();
  bool ok = flashram_read(out, offset, length) == (int)length;
  audio_pump();
  return ok;
}
static bool flash_write(void *ctx, size_t offset, const void *data,
                        size_t length) {
  (void)ctx;
  // Finish every queued RDP/RSP read before the synchronous FlashRAM operation.
  audio_pump();
  rspq_wait();
  bool ok = flashram_write(data, offset, length) == (int)length;
  audio_pump();
  return ok;
}
static bool read_file(void *ctx, const char *name, char *out, unsigned cap,
                      unsigned *length) {
  (void)ctx;
  return game_read_file(name, out, cap, length);
}
#if DG_D10
// Shipped read-only data (the exercise task databases) lives on the ROM
// filesystem under its authored folder tail. Not "read_data": the SDK's
// rsp.h already claims that name.
static bool read_rom_data(void *ctx, const char *name, char *out, unsigned cap,
                          unsigned *length) {
  (void)ctx;
  char path[128];
  snprintf(path, sizeof(path), "rom:/%s", name);
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  size_t read = fread(out, 1, cap, file);
  bool complete = feof(file) || (read < cap);
  fclose(file);
  *length = (unsigned)read;
  return complete;
}
#endif
static bool evict_arena(void);
static bool write_file(void *ctx, const char *name, const char *data,
                       unsigned length) {
  (void)ctx;
  // The archive commit stages a full save generation plus its readback in
  // heap; release completed images first so a crowded scene can still save.
  // It needs those bytes in two contiguous pieces, and a station holding
  // megabytes of scrolling art can have the total without the room, so the
  // sweep probes for a block rather than trusting the free figure.
  for (;;) {
    // A commit stages one generation and its verification readback at the
    // same time, so both have to be placeable — the free total alone says
    // nothing. Every adapter's generation is a 64 KiB FlashRAM page.
    enum { SAVE_GENERATION_BYTES = 64 * 1024 };
    void *staging = malloc(SAVE_GENERATION_BYTES);
    void *readback = malloc(SAVE_GENERATION_BYTES);
    free(staging);
    free(readback);
    if ((staging && readback) || !evict_arena())
      break;
  }
  bool ok = game_write_file(name, data, length);
  debugf("DIRECTOR64 NATIVE_SAVE_%s file=%s bytes=%u generation=%lu\n",
         ok ? "OK" : "ERROR", name, length,
         (unsigned long)game_save_generation());
  return ok;
}
static bool sound_busy(void *ctx, unsigned channel) {
  (void)ctx;
  if (fades && channel < DG_SOUND_CHANNELS && fades->pending[channel])
    return fades->next[channel] != NULL;
  return channel < DG_SOUND_CHANNELS && mixer_ch_playing(channel * 2);
}
#if DG_EXTENDED
// Where the mixer has reached in the sound on this channel, in milliseconds,
// so authored cue points land against the audio the player hears rather than
// against a frame count the console can fall behind.
static unsigned sound_position(void *ctx, unsigned channel) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || !voice_open[channel] || (fades && fades->pending[channel]))
    return 0;
  float rate = voices[channel].wave.frequency;
  if (rate <= 0.0f)
    return 0;
  double played = mixer_ch_get_pos(channel * 2);
  return played <= 0 ? 0 : (unsigned)(played * 1000.0 / rate);
}
#endif
static void sound_start(unsigned channel, const dg_member_t *member);
static void fade_to(unsigned channel, const dg_member_t *member) {
  fades->next[channel] = NULL;
  if (!member) return;
  fades->member[channel] = (dg_member_t){.id = member->id, .looping = member->looping,
                                       .loop_start = member->loop_start,
                                       .loop_end = member->loop_end,
                                       .asset = fades->asset[channel]};
  snprintf(fades->asset[channel], sizeof(fades->asset[channel]), "%s", member->asset);
  fades->next[channel] = &fades->member[channel];
}
static void sound(void *ctx, unsigned channel, const dg_member_t *member) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS) {
    lv_fail(values, "native audio channel");
    return;
  }
  if (fades && fades->pending[channel]) {
    fade_to(channel, member); // the latest request wins, as it would have
    return;
  }
  if (audio_live && fades && voice_open[channel] && mixer_ch_playing(channel * 2) &&
      fades->gain[channel] > 0.0f) {
    fades->pending[channel] = true;
    fading |= (uint16_t)(1u << channel);
    fade_to(channel, member);
    fades->from[channel] = mixer_ch_get_pos(channel * 2);
    mixer_ch_set_vol_ramp(channel * 2, 0.0f, 0.0f, SOUND_FADE_SAMPLES);
    return;
  }
  sound_start(channel, member);
}
// Finish a faded swap: now if `force`, else once the mixer has played past
// the ramp (or the sound ended by itself during it).
static void sound_settle(unsigned channel, bool force) {
  if (!fades || !fades->pending[channel]) return;
  if (!force && mixer_ch_playing(channel * 2)) {
    double pos = mixer_ch_get_pos(channel * 2);
    double need = (double)SOUND_FADE_SAMPLES * voices[channel].wave.frequency /
                  audio_get_frequency();
    if (pos >= fades->from[channel] && pos - fades->from[channel] < need + 1.0)
      return;
  }
  fades->pending[channel] = false;
  fading &= (uint16_t)~(1u << channel);
  sound_start(channel, fades->next[channel]);
  fades->next[channel] = NULL;
  float gain = fades->gain[channel] < 0.0f ? 0.7f : fades->gain[channel];
  mixer_ch_set_vol_ramp(channel * 2, gain, gain, 0);
}
static void sound_start(unsigned channel, const dg_member_t *member) {
  mixer_ch_stop(channel * 2);
  // Closing a wav64 decoder releases buffers which queued audio can reference.
  if (voice_open[channel]) {
    rspq_wait();
    wav64_close(&voices[channel]);
    voice_open[channel] = false;
  }
  if (!member)
    return;
  char path[80];
  snprintf(path, sizeof(path), "rom:/audio/%s", member->asset);
  FILE *file = fopen(path, "rb");
  if (!file) {
    lv_fail(values, "missing native audio file");
    return;
  }
  fclose(file);
  wav64_open(&voices[channel], path);
  voice_open[channel] = true;
  wav64_set_loop(&voices[channel], false);
  if (member->looping) {
    unsigned length, stop;
    if (voices[channel].wave.len <= 0 ||
        !audio_loop_bounds(member->loop_start, member->loop_end,
                           (unsigned)voices[channel].wave.len, &length,
                           &stop)) {
      lv_fail(values, "native audio loop is empty after codec alignment");
      return;
    }
    voices[channel].wave.loop_len = (int)length;
    voices[channel].wave.loop_end = (int)stop;
  }
  wav64_play(&voices[channel], channel * 2);
  debugf("DIRECTOR64 NATIVE_AUDIO channel=%u member=%lu tick=%lu\n",
         channel + 1, (unsigned long)member->id,
         (unsigned long)director->ticks);
}
#if DG_D10
static void play_file(void *ctx, unsigned channel, const char *name) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS)
    return;
  if (fades && fades->pending[channel]) {
    // A streamed file takes the channel over; the faded member never starts.
    fades->pending[channel] = false;
    fading &= (uint16_t)~(1u << channel);
    fades->next[channel] = NULL;
    float gain = fades->gain[channel] < 0.0f ? 0.7f : fades->gain[channel];
    mixer_ch_set_vol_ramp(channel * 2, gain, gain, 0);
  }
  mixer_ch_stop(channel * 2);
  if (voice_open[channel]) {
    rspq_wait();
    wav64_close(&voices[channel]);
    voice_open[channel] = false;
  }
  if (!name || !*name)
    return;
  char path[96];
  snprintf(path, sizeof(path), "rom:/%s.wav64", name);
  FILE *file = fopen(path, "rb");
  if (!file) {
    // The original playFile of a missing file plays nothing and moves on.
    debugf("DIRECTOR64 NATIVE_MISSING_STREAM name=%s\n", name);
    return;
  }
  fclose(file);
  wav64_open(&voices[channel], path);
  voice_open[channel] = true;
  wav64_set_loop(&voices[channel], false);
  wav64_play(&voices[channel], channel * 2);
  debugf("DIRECTOR64 NATIVE_STREAM channel=%u name=%s tick=%lu\n", channel + 1,
         name, (unsigned long)director->ticks);
}
#endif
static void trace(void *ctx, const char *text) {
  (void)ctx;
  debugf("DIRECTOR64 NATIVE_TRACE %s\n", text);
}
#if DG_EXTENDED
// The shared casts appear in every movie's cast list; their scene tables are
// generated const, so the overlays stay resident. That avoids re-expanding
// the multi-megabyte database DSO on every scene change and anchors it as
// one stable block at the bottom of the heap, keeping the region above it
// packable for the full-stage planes.
static struct { const char *name; void *handle; } persistent_overlays[] = {
    {"00_cxt", NULL},
    {"data_cxt", NULL},
    {"cddata_cxt", NULL},
    {"tempplug_cxt", NULL},
};
#endif
static const dg_movie_t *load_movie(unsigned id) {
  if (!id || id > sizeof(movie_names) / sizeof(*movie_names) ||
      overlay_count >= DG_FILES) {
    lv_fail(values, "movie overlay index");
    return NULL;
  }
  char path[80], symbol[64];
  snprintf(path, sizeof(path), "rom:/code/%s.dso", movie_names[id - 1]);
  snprintf(symbol, sizeof(symbol), "dg_%s", movie_names[id - 1]);
#if DG_D5 || DG_D10
  for(unsigned i=0;i<overlay_count;i++)
    if(overlay_ids[i]==id)return dlsym(overlays[i],symbol);
  overlay_ids[overlay_count]=id;
#endif
  void *handle = NULL, **persist = NULL;
#if DG_EXTENDED
  for (unsigned i = 0;
       i < sizeof(persistent_overlays) / sizeof(*persistent_overlays); i++)
    if (!strcmp(movie_names[id - 1], persistent_overlays[i].name)) {
      persist = &persistent_overlays[i].handle;
      handle = *persist;
    }
#endif
  if (!handle) {
    heap_stats_t overlay_stats;
#if DG_CAP_ALLOCATION_CHAIN
    // dlopen needs one contiguous block and cannot retry. Buy that room
    // here, where it is wanted, rather than making every image load
    // pre-pay a reserve for an overlay that may never be requested.
    for (;;) {
      sys_get_heap_stats(&overlay_stats);
      if (overlay_stats.free >= 768 * 1024 || !evict_arena()) break;
    }
#endif
    sys_get_heap_stats(&overlay_stats);
    // A load fails on the largest hole, not the total; keep both visible.
    // Probed before the load, as the trial allocation raises malloc's break
    // and the break never retreats — dlopen's link temporary lives above it.
    unsigned largest = largest_block();
    uint64_t dlopen_started = get_ticks_us();
    handle = dlopen(path, RTLD_LOCAL);
    uint64_t dlopen_us = get_ticks_us() - dlopen_started;
    enter_overlay_us += dlopen_us;
    enter_overlays++;
    debugf("DIRECTOR64 NATIVE_OVERLAY_LOAD path=%s free=%d largest=%u us=%llu\n",
           path, overlay_stats.free, largest, (unsigned long long)dlopen_us);
    if (!handle) {
      debugf("DIRECTOR64 NATIVE_LINK_ERROR %s\n", dlerror());
      lv_fail(values, "movie overlay load failed");
      return NULL;
    }
    {
      // Where the overlay landed, for the cache-miss profile's attribution:
      // its movie table is the last object of the unit, and the heap gave
      // up about the unit's size to hold it.
      const char *stem = strrchr(path, '/');
      stem = stem ? stem + 1 : path;
      char symbol[64];
      snprintf(symbol, sizeof symbol, "aot_%.*s", (int)strcspn(stem, "."), stem);
      heap_stats_t after;
      counted_heap_stats(&after);
      debugf("DIRECTOR64 NATIVE_OVERLAY_BASE symbol=%s at=%p bytes=%d\n", symbol,
             dlsym(handle, symbol), overlay_stats.free - after.free);
    }
    if (persist)
      *persist = handle;
    else
      overlays[overlay_count++] = handle;
  }
  const dg_movie_t *movie = dlsym(handle, symbol);
  if (!movie || movie->id != id) {
    lv_fail(values, "movie overlay export mismatch");
    return NULL;
  }
  return movie;
}
#if DG_EXTENDED
// A shared cast is resident for the rest of the session once any scene needs
// one, so the first scene that does pays the whole expansion inside its entry
// stall — 357 ms of Willy's, most of it the database cast. The boot screens
// leave two thirds of every frame unused, so open one per idle pass instead,
// while the heap is also at its least fragmented. Speculation must not be able
// to fail a run that would otherwise work: a load that does not fit is simply
// left for the entry path, which reports it as it always did.
static void preload_shared_cast(void) {
  static unsigned cursor;
  const unsigned total =
      sizeof(persistent_overlays) / sizeof(*persistent_overlays);
  while (cursor < total) {
    unsigned slot = cursor++;
    if (persistent_overlays[slot].handle)
      continue;
    unsigned id = 0;
    for (unsigned i = 0; i < sizeof(movie_names) / sizeof(*movie_names); i++)
      if (!strcmp(movie_names[i], persistent_overlays[slot].name)) {
        id = i + 1;
        break;
      }
    if (!id)
      continue; // A name no movie in the selected game supplies.
    char path[80];
    snprintf(path, sizeof(path), "rom:/code/%s.dso",
             persistent_overlays[slot].name);
    heap_stats_t stats;
    for (;;) {
      sys_get_heap_stats(&stats);
      if (stats.free >= 768 * 1024 || !evict_arena())
        break;
    }
    unsigned largest = largest_block();
    uint64_t started = get_ticks_us();
    void *handle = dlopen(path, RTLD_LOCAL);
    debugf("DIRECTOR64 NATIVE_OVERLAY_PRELOAD path=%s free=%d largest=%u "
           "us=%llu loaded=%d\n",
           path, stats.free, largest,
           (unsigned long long)(get_ticks_us() - started), handle != NULL);
    persistent_overlays[slot].handle = handle;
    return;
  }
}
#endif
#if DG_D10
// Overlays the focused-window model must keep across a stage transition:
// the controller window's movies, their cast archives, and any castLib
// fileName swap target.
static bool d10_resident_overlay(unsigned id) {
  const dg_movie_t *w[2] = {director->window_home, director->window_movie};
  for (unsigned n = 0; n < 2; n++) {
    if (!w[n]) continue;
    if (w[n]->id == id) return true;
    for (unsigned i = 0; i < w[n]->cast_count; i++)
      if (w[n]->casts[i].file == id) return true;
  }
  for (unsigned i = 0; i < director->cast_swap_count; i++)
    if (director->cast_swaps[i].target && director->cast_swaps[i].target->id == id)
      return true;
  return false;
}
static const dg_movie_t *find_movie(void *ctx, const char *stem, unsigned file) {
  (void)ctx;
  unsigned id = 0;
  if (!stem) {
    id = file;
  } else {
    char lowered[64];
    snprintf(lowered, sizeof(lowered), "%s", stem);
    for (char *p = lowered; *p; p++)
      if (*p >= 'A' && *p <= 'Z') *p += 32;
    size_t n = strlen(lowered);
    for (unsigned i = 0; i < sizeof(movie_names) / sizeof(*movie_names); i++)
      if (!strncmp(movie_names[i], lowered, n) && movie_names[i][n] == '_') {
        id = i + 1;
        break;
      }
  }
  if (!id || id > sizeof(movie_names) / sizeof(*movie_names)) return NULL;
  return load_movie(id);
}
#endif
// Bytes of a movie overlay in the ROM filesystem: what its load will take
// from the arena.
static unsigned dso_bytes(unsigned id) {
  char path[80];
  snprintf(path, sizeof(path), "rom:/code/%s.dso", movie_names[id - 1]);
  FILE *dso = fopen(path, "rb");
  if (!dso)
    return 0;
  fseek(dso, 0, SEEK_END);
  long bytes = ftell(dso);
  fclose(dso);
  return bytes > 0 ? (unsigned)bytes : 0;
}
#if DG_D10
// Overlay bytes a scene may hold for the next one. The spare set is small in
// practice — most of a scene's libraries are what the next scene needs, and
// those are kept outright — but the cap keeps a long session from ending up
// with every shared library resident.
enum {
  OVERLAY_SPARE_BYTES = 512 * 1024,
  // A scene needing less than this is passing through, not playing.
  OVERLAY_PASSTHROUGH_BYTES = 64 * 1024,
};
static bool entering_needs_overlay(unsigned entering, unsigned overlay) {
  if (overlay == entering)
    return true;
  for (unsigned i = movie_cast_first[entering - 1]; movie_cast_files[i]; i++)
    if (movie_cast_files[i] == overlay)
      return true;
  return false;
}
// Release one overlay the running scene does not use, so the image cache can
// reclaim that heap instead of evicting the frame's own working set. A spare
// is absent from the movie's cast list, so nothing resident can resolve a
// member through it; cast-swap targets and window casts answer
// d10_resident_overlay and are never taken. Picks the first rather than the
// largest: this runs under pressure and must not touch the filesystem.
static bool release_spare_overlay(void) {
  if (!director || !director->movie)
    return false;
  for (unsigned j = 0; j < overlay_count; j++) {
    if (d10_resident_overlay(overlay_ids[j]) ||
        entering_needs_overlay(director->movie->id, overlay_ids[j]))
      continue;
    debugf("DIRECTOR64 NATIVE_OVERLAY_SPARE_RELEASE movie=%s reason=image\n",
           movie_names[overlay_ids[j] - 1]);
    dlclose(overlays[j]);
    overlays[j] = overlays[--overlay_count];
    overlay_ids[j] = overlay_ids[overlay_count];
    malloc_trim(0);
    return true;
  }
  return false;
}
#endif
static unsigned movie_id(const char *name) {
  char stem[64];
  snprintf(stem, sizeof(stem), "%s", name);
  for (char *p = stem; *p; p++) {
    if (*p >= 'A' && *p <= 'Z')
      *p += 32;
    if (*p == '.')
      *p = '_';
  }
  if (!strchr(stem, '_'))
    strcat(stem, "_dxr");
  for (unsigned i = 0; i < sizeof(movie_names) / sizeof(*movie_names); i++)
    if (!strcmp(movie_names[i], stem))
      return i + 1;
  return 0;
}
static bool enter(const char *name, unsigned frame) {
  uint64_t enter_started = get_ticks_us();
  unsigned id = movie_id(name);
  if (!id) {
    debugf("DIRECTOR64 NATIVE_MISSING_MOVIE name=%s\n", name);
    lv_fail(values, "unresolved movie transition");
    return false;
  }
  dg_stop_sounds(director);
  // Entry frees decoders for the incoming scene's heap; it cannot wait for
  // a fade, and the scene cut hides the cut sound.
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++)
    sound_settle(ch, true);
#if DG_D5
  d5_video_close();
#endif
  rspq_wait();
#if DG_D10
  // A streamed speech file is not a channel member, so stopping the sound
  // channels leaves its decoder open. Its buffers would sit in the middle
  // of the heap while the entering movie's megabyte cast overlays look for
  // one contiguous block.
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++)
    if (voice_open[ch]) {
      mixer_ch_stop(ch * 2);
      wav64_close(&voices[ch]);
      voice_open[ch] = false;
    }
#endif
#if DG_D5 || DG_EXTENDED
  scene_ready=false;scene_first_serial=render_serial+1;
#if DG_EXTENDED
  completed_frame = reported_frame = 0;
#endif
#endif
#if DG_MODERN
  clear_text_fonts();
  source_state[0] = 0;
#endif
  for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++) {
#if DG_CAP_DRAW_BLOCKS
    if (cache[i].block) rspq_block_free(cache[i].block);
#endif
    // A block-backed entry's pixels go back with the block, not to malloc.
    if (cache[i].region_offset) cache[i].data = NULL;
    free(cache[i].data);
    memset(&cache[i], 0, sizeof(cache[i]));
    cache_key[i] = 0;
  }
  bundle_release();
  cache_bytes = cache_arena_bytes = 0;
  frame_cached = false;
#if DG_CAP_DRAW_BLOCKS
  bitmap_block_bytes = 0;
#endif
  alpha_draw_reported = false;
#if DG_D5
  if(director->window_open)window_overlay_base=overlay_count;
  else {
#endif
#if DG_D10
    {
      // Keep what the entering scene will ask for, and keep the libraries
      // other movies share while they fit the spare budget. Half of a play
      // session's overlay bytes were reloads of a library the previous scene
      // had loaded: the authored flow separates every pair of real scenes
      // with a tiny transition movie, and draining there released them.
      // Only a scene that needs almost nothing may carry spares. That is the
      // transition movie the authored flow puts between every pair of real
      // scenes, which is the whole point: the libraries survive it and the
      // next scene finds them loaded. Carrying a spare *through* a real scene
      // is a different trade and a losing one — the map screen paid more in
      // image traffic than the load it saved.
      unsigned entering_bytes = dso_bytes(id);
      for (unsigned i = movie_cast_first[id - 1]; movie_cast_files[i]; i++)
        entering_bytes += dso_bytes(movie_cast_files[i]);
      bool passing_through = entering_bytes < OVERLAY_PASSTHROUGH_BYTES;
      // Exactly one spare, the largest shared library. Overlays load
      // largest-first into a drained arena, so that one sits at the bottom
      // and the region above it stays coalesced for the next scene to pack
      // into. Keeping several instead punched holes through the middle: free
      // bytes were unchanged but the largest block fell by a third of a
      // megabyte, and the map screen's full-stage planes churned for it.
      unsigned spare = 0, spare_bytes = 0;
      if (passing_through)
        for (unsigned i = 0; i < overlay_count; i++) {
          unsigned overlay = overlay_ids[i];
          if (d10_resident_overlay(overlay) ||
              entering_needs_overlay(id, overlay) || !overlay ||
              overlay > sizeof(movie_shared_cast) ||
              !movie_shared_cast[overlay - 1])
            continue;
          unsigned bytes = dso_bytes(overlay);
          if (bytes > spare_bytes && bytes <= OVERLAY_SPARE_BYTES) {
            spare_bytes = bytes;
            spare = overlay;
          }
        }
      unsigned kept = 0;
      for (unsigned i = 0; i < overlay_count; i++) {
        unsigned overlay = overlay_ids[i];
        bool keep = d10_resident_overlay(overlay) ||
                    entering_needs_overlay(id, overlay) ||
                    (spare && overlay == spare);
        if (keep) {
          overlays[kept] = overlays[i];
          overlay_ids[kept++] = overlay;
        } else {
          dlclose(overlays[i]);
        }
      }
      overlay_count = kept;
    }
#else
    for (unsigned i = 0; i < overlay_count; i++) dlclose(overlays[i]);
    overlay_count = 0;
#endif
#if DG_D5
    window_overlay_base=0;director->window_cleanup=false;
  }
#endif
  // Return the drained arena's trailing free space to the system: dlopen's
  // link temporaries live in the top-of-RAM scratch arena, which can only
  // grow while malloc's break sits low. Without the trim, one scene whose
  // image churn pushes the break near the top (the castle map) starves
  // every later cast-overlay load of its temporary, permanently.
  malloc_trim(0);
  // Take the scene's block here and nowhere else: the old scene's overlays
  // are closed, the cache is empty and the trim has just returned the
  // tail, so this is the one moment in a scene's life when the arena is
  // coalesced, and the block is the same size every time, so the hole it
  // leaves is the hole the next one fills. The entering movie's overlays
  // then pack into what is left, which the first sizing keeps for them.
  // Taking it after they load instead measured a largest hole of 2 MB
  // against 3.7 MB free (the D8 overlays and their link temporaries
  // interleave), and sizing it per scene measured Deutsch's largest hole
  // falling from 4.6 to 1.5 MB in six scenes as the kept overlays fenced
  // each smaller block's hole off from the next larger one.
  {
    heap_stats_t drained;
    sys_get_heap_stats(&drained);
    unsigned largest = largest_block();
    debugf("DIRECTOR64 NATIVE_SCENE_DRAIN name=%s free=%d largest=%u\n", name,
           drained.free, largest);
    bundle_enter(id, largest);
  }
  enter_drain_us += get_ticks_us() - enter_started;
#if DG_D10
  const dg_movie_t *movie = NULL, *shared[DG_FILES - 1];
  unsigned count = 0;
  // Decompressing a cast overlay needs one contiguous block for its whole
  // image plus a link-time temporary. Authored cast order interleaves a
  // megabyte library with small ones, and even the entering movie's own
  // small overlay can take the bottom of the critical hole first, so claim
  // the free region truly largest-first, as the full-stage planes do. The
  // generated cast table supplies the dependency list, which used to cost a
  // throwaway load of the entering movie's own overlay just to read it back
  // — for a big stage movie that was a second decompression of its whole
  // image before the ordered pass could start.
  {
    unsigned files[DG_FILES + 1], sizes[DG_FILES + 1], pending = 0;
    unsigned probe_files[DG_FILES + 1];
    unsigned probe_count = 0;
    probe_files[probe_count++] = id;
    for (unsigned i = movie_cast_first[id - 1];
         movie_cast_files[i] && probe_count <= DG_FILES; i++) {
      unsigned file = movie_cast_files[i];
      bool seen = false;
      for (unsigned j = 0; j < probe_count; j++)
        seen |= probe_files[j] == file;
      if (seen || file > sizeof(movie_names) / sizeof(*movie_names))
        continue;
      probe_files[probe_count++] = file;
    }
    for (unsigned i = 0; i < probe_count; i++) {
      files[pending] = probe_files[i];
      sizes[pending++] = dso_bytes(probe_files[i]);
    }
    // A retained spare costs contiguous heap, and dlopen cannot retry: it
    // asserts. Give the room back before the ordered pass whenever the
    // largest hole cannot serve the biggest overlay still missing, releasing
    // the largest spare first because one release usually settles it. The
    // resident image decompresses to more than its packed bytes, hence the
    // margin. With no spare left this is exactly the old drain-everything
    // behavior, so a scene that truly needs the whole heap still gets it.
    for (;;) {
      unsigned biggest = 0;
      for (unsigned i = 0; i < pending; i++) {
        bool loaded = false;
        for (unsigned j = 0; j < overlay_count; j++)
          loaded |= overlay_ids[j] == files[i];
        if (!loaded && sizes[i] > biggest)
          biggest = sizes[i];
      }
      if (!biggest || largest_block() >= biggest + biggest / 2)
        break;
      unsigned worst = overlay_count, worst_bytes = 0;
      for (unsigned j = 0; j < overlay_count; j++) {
        if (d10_resident_overlay(overlay_ids[j]) ||
            entering_needs_overlay(id, overlay_ids[j]))
          continue;
        unsigned bytes = dso_bytes(overlay_ids[j]);
        if (bytes >= worst_bytes) {
          worst_bytes = bytes;
          worst = j;
        }
      }
      if (worst == overlay_count)
        break;
      debugf("DIRECTOR64 NATIVE_OVERLAY_SPARE_RELEASE movie=%s bytes=%u\n",
             movie_names[overlay_ids[worst] - 1], worst_bytes);
      dlclose(overlays[worst]);
      overlays[worst] = overlays[--overlay_count];
      overlay_ids[worst] = overlay_ids[overlay_count];
      malloc_trim(0);
    }
    while (pending) {
      unsigned best = 0;
      for (unsigned j = 1; j < pending; j++)
        if (sizes[j] > sizes[best])
          best = j;
      const dg_movie_t *loaded = load_movie(files[best]);
      if (!loaded)
        return false;
      if (files[best] == id)
        movie = loaded;
      files[best] = files[--pending];
      sizes[best] = sizes[pending];
    }
    if (!movie) {
      movie = load_movie(id);
      if (!movie)
        return false;
    }
  }
#else
  const dg_movie_t *movie = load_movie(id), *shared[DG_FILES - 1];
  unsigned count = 0;
  if (!movie)
    return false;
#endif
  for (unsigned i = 0; i < movie->cast_count; i++)
    if (movie->casts[i].file != id) {
      bool added = false;
      for (unsigned j = 0; j < count; j++)
        if (shared[j]->id == movie->casts[i].file)
          added = true;
      if (!added) {
        const dg_movie_t *s = load_movie(movie->casts[i].file);
        if (!s)
          return false;
        shared[count++] = s;
      }
    }
#if DG_D10
  // Pin the entering scene's opening full-stage planes right after the cast
  // overlays: the overlays claim the largest contiguous blocks first, and
  // the planes take theirs before per-frame allocations splinter the rest.
  uint64_t plane_started = get_ticks_us();
  for (unsigned f = 0; f < movie->frame_count && f < 2 && !values->failed; f++)
    for (unsigned n = 0; n < movie->frames[f].count; n++) {
      const dg_delta_t *delta = &movie->deltas[movie->frames[f].first + n];
      if (delta->channel < 6) continue;
      uint32_t member_id = delta->value.member;
      if (member_id >> 20 != movie->id) continue;
      for (unsigned k = 0; k < movie->member_count; k++) {
        const dg_member_t *m = &movie->members[k];
        if (m->id != member_id) continue;
        if (m->type == 1 && (unsigned)m->width * m->height >= 100000) {
          unsigned plane_ink = delta->value.ink;
          if (plane_ink == 0 || plane_ink == 8 || plane_ink == 9 ||
              plane_ink == 32 || plane_ink == 36 || plane_ink == 39)
            image(m, plane_ink);
        }
        break;
      }
    }
  enter_plane_us += get_ticks_us() - plane_started;
  if (values->failed)
    return false;
#endif
  uint64_t dg_started = get_ticks_us();
  enter_load_us += dg_started - enter_started;
  bool entered = dg_enter(director, movie, frame ? frame : 1, shared, count);
  enter_script_us += get_ticks_us() - dg_started;
  if (!entered)
    return false;
#if DG_MODERN
  // Observe a movie's conventional <stem>_state global when present. This is
  // read-only telemetry; the game continues to own every state transition.
  char state_name[64];
  unsigned state_length = 0;
  const char *movie_name = movie->code->name;
  while (movie_name[state_length] && movie_name[state_length] != '.' &&
         state_length < sizeof(state_name) - 7) {
    char c = movie_name[state_length];
    state_name[state_length++] = c >= 'A' && c <= 'Z' ? c + 32 : c;
  }
  strcpy(state_name + state_length, "_state");
  source_state_id = lv_global_id(values, state_name);
#endif
  heap_stats_t stats;
  sys_get_heap_stats(&stats);
  debugf("DIRECTOR64 NATIVE_SCENE name=%s frame=%u tick=%lu free=%d "
         "largest=%u\n", movie->code->name, frame,
         (unsigned long)director->ticks, stats.free, largest_block());
  return true;
}
#if DG_CAP_DRAW_BLOCKS
static bool release_bitmap_blocks(void) {
  if (!bitmap_block_bytes) return false;
  // rspq_wait also waits for queued RDP work in the pinned SDK.
  rspq_wait();
  for (unsigned i=0;i<IMAGE_CACHE_SLOTS;i++) if (cache[i].block) {
    rspq_block_free(cache[i].block);cache[i].block=NULL;cache[i].block_bytes=0;
  }
  for (unsigned i=0;i<bundle_count;i++) if (bundle_images[i].block) {
    rspq_block_free(bundle_images[i].block);bundle_images[i].block=NULL;bundle_images[i].block_bytes=0;
  }
  bitmap_block_bytes=0;
  return true;
}
#endif
#if DG_D10
// The corpus pairs every masked image with a "<name>_mask" member directly
// after it. Mask ink over any other neighbour copies, as the original does
// with an unsuitable mask member (the class panel's full-stage base).
static bool mask_pair(const dg_member_t *member, const dg_member_t *mask) {
  size_t length = strlen(member->name);
  return !strncmp(mask->name, member->name, length) &&
         !strcmp(mask->name + length, "_mask");
}
#endif
// Renders whose images count as the scene's live working set. A sweep that
// takes an image the next render draws again buys its headroom with a full
// reload, so the set a running scene cycles through must outlast one frame:
// the measured exercise screens redraw six images every frame and four more
// every fourth frame.
enum { IMAGE_LIVE_RENDERS = 8 };
// Reclaim the least recently used completed image. `live_only` false is the
// hard sweep every allocation path needs; true keeps the live working set and
// releases only what the scene has stopped drawing.
static void evict_entry(cached_image_t *entry) {
  cache_key[entry - cache] = 0;
  cache_bytes -= entry->bytes;
#if DG_CAP_CACHE_STATS
  cache_evictions++;
  if (entry->block) {
    rspq_block_free(entry->block);
    bitmap_block_bytes -= entry->block_bytes;
  }
#endif
  if (entry->region_offset) {
    image_region_release(entry->region_offset - 1, (entry->bytes + 31u) & ~31u);
  } else {
    cache_arena_bytes -= entry->bytes;
    free(entry->data);
  }
  memset(entry, 0, sizeof(*entry));
}
// Which storage an eviction must reclaim from. Freeing an arena image does
// not open room in the block, or the reverse, so a sweep that is making
// space names the class it needs; ANY is for pressure on the heap at large.
typedef enum { IMAGE_ANY, IMAGE_REGION, IMAGE_ARENA } image_storage_t;
static bool evict_lru_of(bool keep_live, image_storage_t storage) {
  cached_image_t *oldest = NULL;
  for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++) {
    if (storage == IMAGE_REGION && !cache[i].region_offset) continue;
    if (storage == IMAGE_ARENA && cache[i].region_offset) continue;
    if (cache[i].data && cache[i].last <= completed_serial &&
        !(keep_live && cache[i].last + IMAGE_LIVE_RENDERS > render_serial) &&
        (!oldest || cache[i].last < oldest->last))
      oldest = &cache[i];
  }
  if (!oldest)
    return false;
  evict_entry(oldest);
  return true;
}
static bool evict_lru(bool keep_live) {
  return evict_lru_of(keep_live, IMAGE_ANY);
}
// Reclaim malloc-arena bytes. Every sweep that is buying room for something
// that is not an image — a font atlas, an overlay, a save generation — wants
// this one: evicting a block-backed image returns nothing to the arena, so
// a plain LRU sweep would empty the cache and still not free a byte.
static bool evict_arena(void) {
  if (image_region) return evict_lru_of(false, IMAGE_ARENA);
  return evict_lru_of(false, IMAGE_ANY);
}
// Free extents of the block's dynamic area, ordered by offset and always
// coalesced. Every dynamic block belongs to a cache entry, so the entries
// are the allocation table and this list is its complement; both stay
// O(holes) to maintain, which matters because a frame under pressure
// allocates and evicts dozens of times (a full scan of the entries per
// attempt measured 319 ms per 300 ticks, 6% of the service budget).
typedef struct { uint32_t offset, bytes; } image_hole_t;
static image_hole_t image_holes[IMAGE_CACHE_SLOTS + 1];
static unsigned image_hole_count;
static void image_region_reset(void) {
  image_hole_count = image_region_bytes > image_region_base ? 1 : 0;
  image_holes[0] = (image_hole_t){image_region_base, image_region_bytes - image_region_base};
}
static void image_region_release(uint32_t offset, uint32_t bytes) {
  unsigned i = 0;
  while (i < image_hole_count && image_holes[i].offset < offset) i++;
  bool after = i && image_holes[i - 1].offset + image_holes[i - 1].bytes == offset;
  bool before = i < image_hole_count && offset + bytes == image_holes[i].offset;
  if (after && before) {
    image_holes[i - 1].bytes += bytes + image_holes[i].bytes;
    memmove(&image_holes[i], &image_holes[i + 1],
            (image_hole_count - i - 1) * sizeof(*image_holes));
    image_hole_count--;
  } else if (after) {
    image_holes[i - 1].bytes += bytes;
  } else if (before) {
    image_holes[i].offset = offset;
    image_holes[i].bytes += bytes;
  } else {
    // One hole per allocated block at worst, and the area holds at most
    // IMAGE_CACHE_SLOTS blocks, so the table cannot overflow.
    memmove(&image_holes[i + 1], &image_holes[i],
            (image_hole_count - i) * sizeof(*image_holes));
    image_holes[i] = (image_hole_t){offset, bytes};
    image_hole_count++;
  }
}
static uint32_t image_region_free_bytes(void) {
  uint32_t total = 0;
  for (unsigned i = 0; i < image_hole_count; i++) total += image_holes[i].bytes;
  return total;
}
// The block's cached images in address order.
static unsigned image_region_sorted(cached_image_t **live) {
  unsigned count = 0;
  for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++)
    if (cache[i].data && cache[i].region_offset) {
      unsigned at = count++;
      while (at && live[at - 1]->region_offset > cache[i].region_offset) {
        live[at] = live[at - 1];
        at--;
      }
      live[at] = &cache[i];
    }
  return count;
}
// Slide the dynamic blocks that start inside [from, to) down onto `from`,
// then rebuild the holes as the complement of the blocks. Unlike the malloc
// arena the area's blocks are movable — a cache entry's `data` is the only
// reference to its pixels — so a request that fits by bytes can always be
// made to fit by address. Without this the train station wedged: 1.9 MB
// live and 1.2 MB free, but no 814 KB hole, so the pavement was skipped for
// hundreds of consecutive renders.
static void image_region_slide(uint32_t from, uint32_t to) {
  uint64_t started = get_ticks_us();
  // The RDP must not be reading a block while it moves.
  rspq_wait();
  cached_image_t *live[IMAGE_CACHE_SLOTS];
  unsigned count = image_region_sorted(live);
  uint32_t next = from;
  for (unsigned i = 0; i < count; i++) {
    uint32_t offset = live[i]->region_offset - 1;
    uint32_t size = (live[i]->bytes + 31u) & ~31u;
    if (offset < from || offset >= to) continue;
    if (offset != next) {
#if DG_CAP_DRAW_BLOCKS
      // A recorded command block holds its texture's address, so only the
      // images that actually move lose their recording. Dropping every
      // recording instead cost 340 ms of render time per five seconds in
      // the train capture — more than the compaction it paid for.
      if (live[i]->block) {
        rspq_block_free(live[i]->block);
        bitmap_block_bytes -= live[i]->block_bytes;
        live[i]->block = NULL;
        live[i]->block_bytes = 0;
      }
#endif
      // Blocks only ever move down, so copying in ascending pieces is
      // safe, and the audio is served between them.
      for (uint32_t done = 0; done < live[i]->bytes;) {
        uint32_t piece = live[i]->bytes - done < 65536 ? live[i]->bytes - done : 65536;
        memmove(image_region + next + done, image_region + offset + done, piece);
        done += piece;
        audio_pump();
      }
      image_region_moved += live[i]->bytes;
      live[i]->region_offset = next + 1;
      live[i]->data = image_region + next;
      data_cache_hit_writeback(image_region + next, size);
    }
    next += size;
  }
  image_hole_count = 0;
  uint32_t at = image_region_base;
  for (unsigned i = 0; i <= count; i++) {
    uint32_t block = i < count ? live[i]->region_offset - 1 : image_region_bytes;
    if (block > at) image_holes[image_hole_count++] = (image_hole_t){at, block - at};
    if (i < count) at = block + ((live[i]->bytes + 31u) & ~31u);
  }
  image_region_compactions++;
  image_region_compact_us += get_ticks_us() - started;
}
static bool image_region_compact(void) {
  if (!image_region || image_hole_count <= 1) return false;
  image_region_slide(image_region_base, image_region_bytes);
  return true;
}
// Make one hole of `bytes` from the cheapest run of the block, in address
// order. The station fills its block with the strips it scrolls past and
// leaves most of them stale; asked for a few kilobytes by address, a whole
// compaction used to move 2-3 MB (200-330 ms, three times a minute, each
// long enough to starve the audio) while 0.4-1.8 MB of images that had not
// been drawn for hundreds of frames sat in the way.
//
// Without `move` the run may hold only holes and stale images, which are
// dropped: an image dropped here reloads if it is drawn again, tens of
// milliseconds against a compaction's hundreds, and images that went stale
// only recently weigh four times their bytes, so the strip the train just
// left goes last. With `move` the run may also hold images still being
// drawn; they slide down inside the run, costing the bytes they move,
// while stale ones in it are dropped at a quarter of theirs (or their
// full bytes if recently drawn).
// A span of the area in address order: a hole by its index, or (with the
// high bit) an image by its place in the sorted list.
enum { SPAN_IS_IMAGE = 0x8000 };
static inline cached_image_t *span_image(cached_image_t **live, uint16_t code) {
  return code & SPAN_IS_IMAGE ? live[code & (SPAN_IS_IMAGE - 1)] : NULL;
}
static inline image_hole_t span_hole(uint16_t code) {
  return code < IMAGE_CACHE_SLOTS + 1 ? image_holes[code] : (image_hole_t){0, 0};
}
static bool image_region_make_hole(unsigned bytes, bool move) {
  if (!image_region) return false;
  bytes = (bytes + 31u) & ~31u;
  cached_image_t *live[IMAGE_CACHE_SLOTS];
  unsigned count = image_region_sorted(live);
  // The area in address order as one list of span codes, so the walk needs
  // a few hundred bytes of stack rather than a table among the statics.
  uint16_t spans[2 * IMAGE_CACHE_SLOTS + 2];
  unsigned n = 0, h = 0, l = 0;
  while (h < image_hole_count || l < count) {
    if (l == count || (h < image_hole_count &&
                       image_holes[h].offset < live[l]->region_offset - 1))
      spans[n++] = (uint16_t)h++;
    else
      spans[n++] = (uint16_t)(SPAN_IS_IMAGE | l++);
  }
  // What a span covers, what it frees, what it costs, and whether it ends
  // the run: an image still being drawn when nothing may move.
#define SPAN_IMAGE(i) span_image(live, spans[i])
#define SPAN_START(i) (SPAN_IMAGE(i) ? SPAN_IMAGE(i)->region_offset - 1 : span_hole(spans[i]).offset)
#define SPAN_END(i) (SPAN_IMAGE(i) ? SPAN_IMAGE(i)->region_offset - 1 + ((SPAN_IMAGE(i)->bytes + 31u) & ~31u) \
                               : span_hole(spans[i]).offset + span_hole(spans[i]).bytes)
  // Stale: drawn by no frame still in flight and by none of the last few.
#define SPAN_STALE(e) ((e)->last <= completed_serial && (e)->last + IMAGE_LIVE_RENDERS <= render_serial)
  unsigned lo = 0, best_lo = 0, best_hi = 0;
  uint64_t cost = 0, freed = 0, best = UINT64_MAX;
  uint32_t lo_free = 0, lo_cost = 0;
  for (unsigned hi = 0; hi < n; hi++) {
    cached_image_t *e = SPAN_IMAGE(hi);
    uint32_t size = SPAN_END(hi) - SPAN_START(hi);
    bool stale = !e || SPAN_STALE(e);
    bool recent = e && render_serial - e->last < 60;
    uint32_t span_free = stale ? size : 0;
    uint32_t span_cost = !e ? 0 : !stale ? size
                       : move ? (recent ? size : size / 4) : (recent ? size * 4 : size);
    bool barrier = !stale && !move;
    if (barrier || (hi > lo && SPAN_START(hi) != SPAN_END(hi - 1))) {
      lo = barrier ? hi + 1 : hi;
      cost = freed = 0;
      if (barrier) continue;
    }
    cost += span_cost;
    freed += span_free;
    for (;;) {
      // What the run's first span contributes, recomputed as it advances.
      cached_image_t *f = SPAN_IMAGE(lo);
      uint32_t fsize = SPAN_END(lo) - SPAN_START(lo);
      bool fstale = !f || SPAN_STALE(f);
      bool frecent = f && render_serial - f->last < 60;
      lo_free = fstale ? fsize : 0;
      lo_cost = !f ? 0 : !fstale ? fsize
              : move ? (frecent ? fsize : fsize / 4) : (frecent ? fsize * 4 : fsize);
      if (lo >= hi || freed - lo_free < bytes) break;
      cost -= lo_cost;
      freed -= lo_free;
      lo++;
    }
    if (freed >= bytes && cost < best) {
      best = cost;
      best_lo = lo;
      best_hi = hi;
    }
  }
  if (best == UINT64_MAX || !best) return false;
  uint32_t from = SPAN_START(best_lo), to = SPAN_END(best_hi);
  bool moves = false;
  for (unsigned i = best_lo; i <= best_hi; i++) {
    cached_image_t *e = SPAN_IMAGE(i);
    if (!e) continue;
    if (SPAN_STALE(e)) evict_entry(e);
    else moves = true;
  }
#undef SPAN_IMAGE
#undef SPAN_START
#undef SPAN_END
#undef SPAN_STALE
  if (moves) image_region_slide(from, to);
  else image_region_windows++;
  return true;
}
static uint8_t *image_region_alloc(unsigned bytes) {
  if (!image_region) return NULL;
  bytes = (bytes + 31u) & ~31u;
  if (!bytes || bytes > image_region_bytes - image_region_base) return NULL;
  uint64_t started = get_ticks_us();
  uint8_t *result = NULL;
  for (unsigned i = 0; i < image_hole_count; i++)
    if (image_holes[i].bytes >= bytes) {
      image_region_pending_offset = image_holes[i].offset;
      image_region_pending_bytes = bytes;
      image_holes[i].offset += bytes;
      image_holes[i].bytes -= bytes;
      if (!image_holes[i].bytes) {
        memmove(&image_holes[i], &image_holes[i + 1],
                (image_hole_count - i - 1) * sizeof(*image_holes));
        image_hole_count--;
      }
      result = image_region + image_region_pending_offset;
      break;
    }
  image_region_us += get_ticks_us() - started;
  return result;
}
// Release a loading buffer that never reached the cache: a pinned slot
// simply stays empty, a dynamic block goes back to the holes, an arena
// buffer to the arena.
static void image_loading_failed(bool bundled, uint8_t *data) {
  if (bundled) return;
  if (image_region && data >= image_region && data < image_region + image_region_bytes) {
    image_region_release(image_region_pending_offset, image_region_pending_bytes);
    image_region_pending_bytes = 0;
    return;
  }
  free(data);
}
// Open the pack at boot and keep the scene table resident: eight bytes per
// movie, the offset and length of its directory.
static void pack_open(void) {
  pack_file = fopen("rom:/images.pack", "rb");
  uint8_t header[DG_PACK_HEADER_BYTES];
  if (!pack_file || fread(header, 1, sizeof(header), pack_file) != sizeof(header) ||
      !dg_pack_header_parse(header, &pack_header)) {
    debugf("DIRECTOR64 NATIVE_PACK unreadable\n");
    if (pack_file) fclose(pack_file);
    pack_file = NULL;
    return;
  }
  size_t table = (size_t)pack_header.scene_count * DG_PACK_SCENE_BYTES;
  pack_scenes = malloc(table ? table : 1);
  if (!pack_scenes || fseek(pack_file, pack_header.scene_table_offset, SEEK_SET) ||
      fread(pack_scenes, 1, table, pack_file) != table) {
    debugf("DIRECTOR64 NATIVE_PACK unreadable scene table\n");
    free(pack_scenes);
    pack_scenes = NULL;
    fclose(pack_file);
    pack_file = NULL;
    return;
  }
  debugf("DIRECTOR64 NATIVE_PACK images=%lu scenes=%lu\n",
         (unsigned long)pack_header.index_count, (unsigned long)pack_header.scene_count);
}
// The index: every image once, sorted by its authored key, searched in
// place with one small read per probe. This is the path for an image no
// directory lists, so its cost is paid rarely.
static bool pack_index_find(uint32_t key, dg_pack_entry_t *entry) {
  if (!pack_file) return false;
  uint32_t low = 0, high = pack_header.index_count;
  uint8_t raw[DG_PACK_ENTRY_BYTES];
  while (low < high) {
    uint32_t middle = low + (high - low) / 2;
    if (fseek(pack_file, pack_header.index_offset + middle * DG_PACK_ENTRY_BYTES, SEEK_SET) ||
        fread(raw, 1, sizeof(raw), pack_file) != sizeof(raw))
      return false;
    dg_pack_entry_parse(raw, entry);
    if (entry->key == key) return true;
    if (entry->key < key)
      low = middle + 1;
    else
      high = middle;
  }
  return false;
}
static int bundle_find(uint32_t key) {
  for (unsigned slot = bundle_hash_slot(key);; slot = (slot + 1) & (BUNDLE_HASH - 1)) {
    unsigned row = bundle_hash[slot];
    if (!row) return -1;
    if (bundle_keys[row - 1] == key) return (int)row - 1;
  }
}
static void bundle_release(void) {
#if DG_CAP_DRAW_BLOCKS
  for (unsigned i = 0; i < bundle_count; i++)
    if (bundle_images[i].block) rspq_block_free(bundle_images[i].block);
#endif
  free(bundle_block);
  free(bundle_rows);
  free(bundle_keys);
  free(bundle_images);
  image_region = NULL;
  image_region_bytes = image_region_base = 0;
  image_region_pending_bytes = 0;
  image_hole_count = 0;
  bundle_block = NULL;
  bundle_rows = NULL;
  bundle_keys = NULL;
  bundle_images = NULL;
  bundle_count = bundle_resident = bundle_bytes = 0;
  memset(bundle_hash, 0, sizeof(bundle_hash));
}
// Read the entering scene's directory and take the block. `largest` is the
// largest block the drained arena offers; the first time, the scene's
// overlays and the headroom come off it to size the block for the session.
// A scene whose whole directory fits gets it all pinned, with the rest of
// the block for the images no directory lists; a scene that draws more
// than the block holds pins the rows the stage draws at least half the
// time and leaves the rest to the overflow cache, which the station's
// pavement strips and the house's rooms cycle through.
enum { BUNDLE_PIN_SHARE = 500 };
static void bundle_enter(unsigned id, unsigned largest) {
  uint64_t started = get_ticks_us();
  if (!pack_file || !id || id > pack_header.scene_count) return;
  const uint8_t *scene = pack_scenes + (id - 1) * DG_PACK_SCENE_BYTES;
  uint32_t first = dg_pack_be32(scene), count = dg_pack_be32(scene + 4);
  if (count > BUNDLE_ROWS) count = BUNDLE_ROWS;
  uint8_t *raw = count ? malloc((size_t)count * DG_PACK_ENTRY_BYTES) : NULL;
  bundle_rows = count ? malloc(count * sizeof(*bundle_rows)) : NULL;
  bundle_keys = count ? malloc(count * sizeof(*bundle_keys)) : NULL;
  bundle_images = count ? calloc(count, sizeof(*bundle_images)) : NULL;
  if (count && (!raw || !bundle_rows || !bundle_keys || !bundle_images ||
                fseek(pack_file, pack_header.entries_offset + first * DG_PACK_ENTRY_BYTES,
                      SEEK_SET) ||
                fread(raw, DG_PACK_ENTRY_BYTES, count, pack_file) != count)) {
    free(raw);
    bundle_release();
    debugf("DIRECTOR64 NATIVE_BUNDLE movie=%u unread\n", id);
    return;
  }
  unsigned total = 0;
  for (unsigned i = 0; i < count; i++) {
    dg_pack_entry_parse(raw + i * DG_PACK_ENTRY_BYTES, &bundle_rows[i].entry);
    bundle_keys[i] = bundle_rows[i].entry.key;
    bundle_rows[i].slot = BUNDLE_UNPLACED;
    total += (bundle_rows[i].entry.allocation + 31u) & ~31u;
  }
  free(raw);
  bundle_count = count;
  for (unsigned i = 0; i < count; i++) {
    unsigned slot = bundle_hash_slot(bundle_keys[i]);
    while (bundle_hash[slot]) slot = (slot + 1) & (BUNDLE_HASH - 1);
    bundle_hash[slot] = (uint16_t)(i + 1);
  }
  unsigned bytes = 0, pinned = 0, resident = 0;
#if !DG_D10
  // A scene without a directory keeps the arena cache it always had: the
  // boot movies preload shared casts behind their entry, and a block taken
  // for nothing starved Willy's of the megabyte they need.
  if (count && is_memory_expanded()) {
    if (!bundle_block_size) {
      unsigned reserve = BUNDLE_HEADROOM + dso_bytes(id);
      for (unsigned i = movie_cast_first[id - 1]; movie_cast_files[i]; i++)
        reserve += dso_bytes(movie_cast_files[i]);
      unsigned spare = largest > reserve ? (largest - reserve) & ~31u : 0;
      bundle_block_size = spare < IMAGE_BLOCK_BYTES ? spare : IMAGE_BLOCK_BYTES;
    }
    // Take less than the session's size rather than nothing, and keep the
    // smaller size: what a scene can spare is what its overlays left.
    for (bytes = bundle_block_size; bytes >= IMAGE_BLOCK_MIN_BYTES; bytes -= IMAGE_BLOCK_STEP)
      if ((bundle_block = memalign(32, bytes))) break;
    if (!bundle_block) bytes = 0;
    bundle_block_size = bytes;
  }
#endif
  bool fits = total <= bytes;
  for (unsigned i = 0; i < count && bytes; i++) {
    unsigned need = (bundle_rows[i].entry.allocation + 31u) & ~31u;
    if (!need || pinned + need > bytes) continue;
    if (!fits && bundle_rows[i].entry.share < BUNDLE_PIN_SHARE) continue;
    bundle_rows[i].slot = pinned;
    pinned += need;
    resident++;
  }
  bundle_bytes = bytes;
  bundle_resident = resident;
  image_region = bundle_block;
  image_region_bytes = bytes;
  image_region_base = pinned;
  image_region_reset();
  bundle_us += get_ticks_us() - started;
  debugf("DIRECTOR64 NATIVE_BUNDLE movie=%u rows=%u resident=%u pinned=%u bytes=%u "
         "total=%u largest=%u at=%p\n",
         id, (unsigned)count, resident, pinned, bytes, total, largest, (void *)bundle_block);
}
static bool evict(void) { return evict_lru(false); }
// Make the cache ceiling hold for one more image. Failing means the frame's
// own live set already fills the budget, which is a reason to skip one
// sprite for one render — the original skipped updates under memory
// pressure too — and never a reason to stop the movie.
// What the arena may hold of images: with a block, only the bounded
// overflow; without one (a scene with no directory), the cache the arena
// always gave. Lending the arena more beside a block measured 24 KB of
// free memory on the Deutsch showcase, against the block's own headroom.
static unsigned image_arena_budget(void) {
  return image_region ? IMAGE_ARENA_BUDGET : IMAGE_CACHE_BYTES;
}
static unsigned image_budget(void) {
  if (image_region) return image_region_bytes - image_region_base + image_arena_budget();
  return IMAGE_CACHE_BYTES;
}
static bool image_room(unsigned stored, const char *asset) {
  bool drained = false;
  while (cache_bytes + stored > image_budget())
    if (!evict()) {
      if (!drained) {
        // Two display jobs are in flight, so the previous frame still pins
        // every image it drew — including the one this frame is scrolling
        // away from. A station pavement is 860 KB against a 3 MB budget, so
        // the outgoing strip alone decides whether the incoming one fits.
        // Waiting for that frame costs at most one frame; skipping the
        // sprite costs a visible gap and a reload on the next render, which
        // is what the train's missing platform was.
        rspq_wait();
        drained = true;
        continue;
      }
      unsigned live = 0, live_bytes = 0, largest = 0;
      for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++)
        if (cache[i].data) {
          live++;
          live_bytes += cache[i].bytes;
          if (cache[i].bytes > largest) largest = cache[i].bytes;
        }
      debugf("DIRECTOR64 NATIVE_IMAGE_BUDGET asset=%s bytes=%u live=%u "
             "live_bytes=%u largest=%u serial=%u completed=%u\n",
             asset, stored, live, live_bytes, largest, render_serial,
             completed_serial);
      image_alloc_soft_fail = true;
      return false;
    }
  return true;
}
static cached_image_t *image_uncounted(const dg_member_t *member, unsigned ink);
// Whether an image is decoded and resident, in the bundle or the overflow
// cache, without loading it.
static inline bool image_cached(const char *asset, unsigned ink) {
  uint32_t key = dg_image_key(asset, ink);
  int row = bundle_find(key);
  if (row >= 0 && bundle_images[row].data) return true;
  for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++)
    if (cache_key[i] == key && cache[i].data && cache[i].ink == ink &&
        !strcmp(cache[i].name, asset))
      return true;
  return false;
}
static cached_image_t *image(const dg_member_t *member, unsigned ink) {
  uint64_t started = get_ticks_us();
  cached_image_t *result = image_uncounted(member, ink);
  phase_image_us += get_ticks_us() - started;
  return result;
}
static cached_image_t *image_uncounted(const dg_member_t *member, unsigned ink) {
  // Legacy copy ink is opaque; D8 FollowAlpha retains its authored coverage.
  uint64_t scan_started = get_ticks_us();
  uint32_t key = dg_image_key(member->asset, ink);
  // The scene's bundle answers first: a decoded slot is a hit, an empty one
  // decodes below, a row past the block loads through the overflow cache
  // from the offset the row already knows.
  int row = bundle_find(key);
  cached_image_t *slot = NULL;
  uint8_t *data = NULL;
  bool bundled = false, have_entry = false, regional = false;
  dg_pack_entry_t entry;
  if (row >= 0) {
    if (bundle_images[row].data) {
      bundle_images[row].last = render_serial;
      image_scan_us += get_ticks_us() - scan_started;
      return &bundle_images[row];
    }
    entry = bundle_rows[row].entry;
    have_entry = true;
    if (bundle_rows[row].slot != BUNDLE_UNPLACED) {
      slot = &bundle_images[row];
      data = bundle_block + bundle_rows[row].slot;
      bundled = true;
    }
  }
  if (!bundled)
    for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++)
      if (cache_key[i] == key && cache[i].data && cache[i].ink == ink &&
          !strcmp(cache[i].name, member->asset)) {
        cache[i].last = render_serial;
        image_scan_us += get_ticks_us() - scan_started;
        return &cache[i];
      }
  image_scan_us += get_ticks_us() - scan_started;
  bool rgba32 = false, alpha_plane = false;
#if DG_MODERN
  size_t name_length = strlen(member->asset);
  rgba32 = name_length >= 4 && !strcmp(member->asset + name_length - 4, ".fd2");
  alpha_plane = name_length >= 4 && !strcmp(member->asset + name_length - 4, ".fda");
#endif
  unsigned stride = rgba32 ? 4u : 2u;
  // A .fdi asset is FDI1 or, when the packer indexed its colours, FDIC,
  // which is smaller than the extent below; the header decides after the
  // load, so the size checks before it are only ceilings.
  const bool maybe_indexed = !rgba32 && !alpha_plane;
  uint32_t alpha_offset = 0;
  unsigned width = member->width, height = member->height;
#if DG_D10
  // Converted assets are prescaled to the 640x480 stage; the member keeps
  // its authored dimensions for every layout and script answer.
  dg_prescale_dims(member->width, member->height, &width, &height);
#endif
  // Images past the texture limit are stored tile-packed by the converter, so
  // the stored extent is the padded one and the console never rearranges a
  // plane it loads.
  uint64_t extent = alpha_plane
      ? dg_fdia_size(width, height, &alpha_offset)
      : 32u + bitmap_plane_pixels(width, height) * stride;
  uint64_t stored = extent;
  if (!width || !height || !extent || extent > IMAGE_CACHE_BYTES ||
      stored > IMAGE_CACHE_BYTES) {
    lv_fail(values, "invalid native image dimensions");
    return NULL;
  }
  unsigned expected = (unsigned)extent;
  uint64_t open_started = get_ticks_us();
  if (!have_entry &&
      !pack_index_find(dg_image_key(member->asset, DG_PACK_INK_AUTHORED), &entry)) {
    image_open_us += get_ticks_us() - open_started;
    debugf("DIRECTOR64 NATIVE_IMAGE_OPEN member=%lu asset=%s\n",
           (unsigned long)member->id, member->asset);
    lv_fail(values, "missing native image file");
    return NULL;
  }
  image_open_us += get_ticks_us() - open_started;
  // The pack recorded the decompressor's allocation (its margin included);
  // the size checks here are ceilings, as above.
  int length = (int)entry.compressed, allocation = (int)entry.allocation;
  if (allocation < (maybe_indexed ? 32 : (int)expected) ||
      allocation > (int)IMAGE_CACHE_BYTES) {
    lv_fail(values, "invalid native image allocation");
    return NULL;
  }
  if (bundled) {
    // The slot is exactly the decompressor's allocation.
    stored = (unsigned)allocation;
  } else {
    if (!image_room((unsigned)stored, member->asset)) return NULL;
    for (unsigned i = 0; i < IMAGE_CACHE_SLOTS; i++)
      if (!cache[i].data) {
        slot = &cache[i];
        break;
      }
    if (!slot) {
      if (!evict()) {
        lv_fail(values, "image cache handles exhausted");
        return NULL;
      }
      // Stay below the phase-counting wrapper: the retry is one lookup.
      return image_uncounted(member, ink);
    }
    // Reserve edge padding in the loading buffer so wide images can be tiled
    // in place, without a second large allocation during the train transition.
    if (!maybe_indexed && (uint64_t)allocation < stored)
      allocation = (int)stored;
    stored = (unsigned)allocation;
    if (!image_room((unsigned)stored, member->asset)) return NULL;
#if DG_CAP_ALLOCATION_CHAIN
    // The image ceiling alone cannot account for overlays, fonts and recorded
    // commands. Release completed LRU images before those allocations crowd
    // the heap, retaining headroom for decompression, audio and scene input
    // work. The extended D6 car screens draw dozens of part images in one
    // handler; images touched by in-flight frames are unevictable there, so
    // headroom must be reclaimed before the loading burst pins them. D10
    // scenes load megabyte cast overlays between renders and place two
    // full-stage planes per frame; a cache grown to its ceiling leaves too
    // little for the next dlopen to find one contiguous block.
#if DG_D10
    const int reserve = 512 * 1024, floor = 320 * 1024;
#else
    const int reserve = 256 * 1024, floor = 160 * 1024;
#endif
    // Reclaim toward the reserve from what the scene stopped drawing, and
    // only take the live working set once the heap is genuinely short. This
    // sweep runs on every overflow load: buying the full reserve out of the
    // images the next render needs is a treadmill, not headroom — the
    // measured exercise screen reloaded its whole ten-image set every frame
    // while two thirds of the cache budget stayed unused. A scene whose live
    // set really does not fit still degrades to plain LRU, because there is
    // then nothing else left to give.
    // A load the block will take is not arena bytes at all, so there is no
    // headroom to buy for it.
    if (!IMAGE_REGION_ACTIVE ||
        (unsigned)allocation > image_region_bytes - image_region_base) {
      uint64_t headroom_started = get_ticks_us();
      for (;;) {
        heap_stats_t stats;
        counted_heap_stats(&stats);
        if (stats.free >= allocation + reserve) break;
#if DG_D10
        // A spare overlay goes before any image. Its whole value was letting
        // the entering scene skip a load, and that has already happened by
        // the time this runs; a stale image in a scene that cycles its art
        // is needed again within a second. Holding a spare through the map
        // screen cost more than the load it saved.
        if (release_spare_overlay()) continue;
#endif
        if (evict_lru(true)) continue;
        if (stats.free >= allocation + floor || !evict_lru(false)) break;
      }
      image_headroom_us += get_ticks_us() - headroom_started;
    }
#endif
    bool drained = false, compacted = false, windowed = false, slid = false;
    for (;;) {
      // The block first, for every image: it is the storage whose blocks
      // can be moved, so pressure there is recoverable, while the same
      // churn in the arena is what leaves a scene unable to place a plane
      // it has the free bytes for.
      if (image_region && (unsigned)allocation <= image_region_bytes - image_region_base) {
        if ((data = image_region_alloc((unsigned)allocation))) {
          regional = true;
          break;
        }
        // Stale images in the way are cheaper to drop than to move; failing
        // a run of them, move only the images inside the cheapest run.
        if (!windowed && image_region_make_hole((unsigned)allocation, false)) {
          windowed = true;
          continue;
        }
        if (!slid && image_region_make_hole((unsigned)allocation, true)) {
          slid = true;
          continue;
        }
        // Room exists but not in one piece: move the blocks rather than
        // drop images the frame is about to draw again.
        if (!compacted &&
            image_region_free_bytes() >= (((unsigned)allocation + 31u) & ~31u) &&
            image_region_compact()) {
          compacted = true;
          continue;
        }
      }
      // Overflow: a scene whose working set is larger than the block still
      // gets the arena, bounded so it cannot crowd out overlays.
      bool arena_allowed = cache_arena_bytes + stored <= image_arena_budget();
      if (arena_allowed) data = memalign(32, (size_t)allocation);
      if (data) break;
#if DG_CAP_CACHE_STATS
      // The counter the capture gates read as heap pressure: a full block
      // is ordinary cache turnover and is reported as evictions instead.
      if (arena_allowed) cache_retries++;
      // Recordings live in the arena, so they can only buy an arena
      // allocation — and with a block there is no reason to spend them:
      // the pixels this is short of are block pixels. Dropping them here
      // anyway left the train station re-emitting draw commands for every
      // sprite, a third more render time per frame.
      if (!IMAGE_REGION_ACTIVE && release_bitmap_blocks()) continue;
#endif
      // An eviction rearranges the area, so the compaction that did not
      // help before this one may help after it.
      compacted = false;
      if (!evict()) {
        if (!drained) {
          // Images referenced by in-flight frames are unevictable and can
          // fragment the heap around a large allocation (the login preview is
          // a full 640x480 plane). Drain queued RDP work once so completed
          // frames release their images, then retry the eviction sweep.
          rspq_wait();
          drained = true;
          continue;
        }
        heap_stats_t stats;
        sys_get_heap_stats(&stats);
        // Nothing evictable is left: what remains is this frame's own live
        // set. Skip this sprite for one frame and let the next render's
        // pre-pass repack the whole set, as the original skipped updates
        // under memory pressure rather than stopping the movie. Not a *FAIL*
        // marker: the capture validation treats those as fatal.
        // The trial-allocation largest-hole probe is too expensive here: this
        // path can run every frame under pressure. The malloc arena's
        // fragmented-bytes figure is free and trends the same way.
        debugf("DIRECTOR64 NATIVE_IMAGE_REPACK asset=%s bytes=%u free=%d "
               "cache=%u serial=%u completed=%u frag=%d\n", member->asset,
               allocation, stats.free, cache_bytes, render_serial,
               completed_serial, stats.fragmented);
        image_alloc_soft_fail = true;
        return NULL;
      }
    }
  }
  audio_pump();
  uint64_t read_started = get_ticks_us();
  bool loaded = !fseek(pack_file, entry.offset, SEEK_SET) &&
                asset_loadf_into(pack_file, &length, data, &allocation);
  image_read_us += get_ticks_us() - read_started;
  audio_pump();
  unsigned ci_bits = 0, colors = 0;
  uint32_t index_offset = 0;
  bool indexed = loaded && maybe_indexed && length >= 32 && !memcmp(data, "FDIC", 4);
  bool valid = loaded && (indexed
      ? dg_fdic_valid(data, (uint32_t)length, width, height, &ci_bits, &colors,
                      &index_offset)
      : length == (int)expected &&
        !memcmp(data, alpha_plane ? "FDIA" : rgba32 ? "FDI2" : "FDI1", 4) &&
        *(uint32_t *)(data + 4) == expected &&
        *(uint16_t *)(data + 8) == width &&
        *(uint16_t *)(data + 10) == height &&
        (!alpha_plane || dg_fdia_valid(data, (uint32_t)length,
                                      width, height, &alpha_offset)));
  if (!valid) {
    image_loading_failed(bundled, data);
    lv_fail(values, "invalid native image");
    return NULL;
  }
  uint64_t ink_started = get_ticks_us();
  // Ink converts the stored plane, padding included: a padded pixel is zero,
  // which every ink leaves alone, and walking the plane as stored keeps this
  // a single sequential pass.
  const unsigned plane_pixels = (unsigned)bitmap_plane_pixels(width, height);
  bool follow_alpha = false;
#if DG_MODERN
  follow_alpha = dg_fdi_follow_alpha(data);
#endif
  // Every ink but matte (36) and opaque copy (0/32 over an image that does
  // not carry its own coverage) leaves the pixels exactly as authored. Walking
  // a megabyte plane to write back what it already holds is pure loss, so the
  // decision is hoisted out of the loop and the identity cases skip it.
  bool ink_rewrites = ink == 36 || ((ink == 0 || ink == 32) && !follow_alpha);
  if (alpha_plane) {
    // The alpha pass only ever clears matte-ink white; coverage is authored.
    if (ink == 36) {
      const uint16_t *pixels = (const uint16_t *)(data + 32);
      uint8_t *alpha = data + alpha_offset;
      for (unsigned i = 0; i < plane_pixels; i++) {
        alpha[i] = dg_bitmap_alpha16(pixels[i], alpha[i], ink);
        if (!(i & 0xffff)) audio_pump();
      }
    }
  } else if (indexed) {
    // One palette entry per colour: the rewrite the pixels got happens once
    // per colour instead of once per pixel.
    uint16_t *tlut = (uint16_t *)(data + 32);
    if (ink_rewrites)
      for (unsigned i = 0; i < colors; i++)
        tlut[i] = dg_bitmap_ink16(tlut[i], ink, follow_alpha);
  } else if (!ink_rewrites) {
    // Identity for this ink: nothing to convert.
  } else if (rgba32) {
    uint32_t *pixels = (uint32_t *)(data + 32);
    for (unsigned i = 0; i < plane_pixels; i++) {
      pixels[i] = dg_bitmap_ink32(pixels[i], ink, follow_alpha);
      if (!(i & 0xffff)) audio_pump();
    }
  } else {
    uint16_t *pixels = (uint16_t *)(data + 32);
    for (unsigned i = 0; i < plane_pixels; i++) {
      pixels[i] = dg_bitmap_ink16(pixels[i], ink, follow_alpha);
      if (!(i & 0xffff)) audio_pump();
    }
  }
  image_ink_us += get_ticks_us() - ink_started;
#if DG_D10
  uint64_t mask_started = get_ticks_us();
  // The packer bakes the mask into the coverage of every image the scenes
  // draw with mask ink (image_pack.apply_mask), and says so in the entry;
  // the per-pixel walk below is the fallback for an image it could not.
  if (ink == 9 && !(entry.flags & DG_PACK_MASK_BAKED)) {
    // Director mask ink: the cast member after the sprite's member is its
    // mask. Dark mask pixels keep the image; light ones become transparent.
    // The corpus pairs every masked image with a matching "<name>_mask".
    const dg_member_t *mask_member = dg_member(director, member->id + 1);
    bool masked = false;
    if (mask_member && mask_member->type == 1 && !rgba32 && !indexed &&
        mask_member->asset[0] && mask_pair(member, mask_member)) {
      // Authored masks can differ by a pixel from their image; sample with
      // clamped coordinates over the overlap, exactly like the original.
      unsigned mask_width = mask_member->width, mask_height = mask_member->height;
      dg_prescale_dims(mask_member->width, mask_member->height, &mask_width,
                       &mask_height);
      dg_pack_entry_t mask_entry;
      if (pack_index_find(dg_image_key(mask_member->asset, DG_PACK_INK_AUTHORED),
                          &mask_entry)) {
        int mask_length = (int)mask_entry.compressed;
        int mask_allocation = (int)mask_entry.allocation;
        uint8_t *mask_data = NULL;
        if (mask_allocation > 32)
          while (!(mask_data = memalign(32, mask_allocation)) && evict()) {}
        if (mask_data) {
          bool mask_loaded =
              !fseek(pack_file, mask_entry.offset, SEEK_SET) &&
              asset_loadf_into(pack_file, &mask_length, mask_data,
                               &mask_allocation) &&
              !memcmp(mask_data, "FDI1", 4) &&
              *(uint16_t *)(mask_data + 8) == mask_width &&
              *(uint16_t *)(mask_data + 10) == mask_height &&
              mask_width && mask_height;
          if (mask_loaded) {
            // Two sequential streams and a read-modify-write per pixel: this
            // walk runs at the console's memory bandwidth, and lifting the
            // clamp, the row products and the plane test out of it measured
            // 1.89 s against 1.84 s. Only removing it can make it cheaper.
            const uint16_t *mask_pixels = (const uint16_t *)(mask_data + 32);
            for (unsigned y = 0; y < height; y++) {
              unsigned mask_y = y < mask_height ? y : mask_height - 1;
              for (unsigned x = 0; x < width; x++) {
                unsigned mask_x = x < mask_width ? x : mask_width - 1;
                uint16_t p = mask_pixels[mask_y * mask_width + mask_x];
                unsigned luminance = ((p >> 11) & 31) * 77 +
                                     ((p >> 6) & 31) * 151 +
                                     ((p >> 1) & 31) * 28;
                unsigned i = y * width + x;
                if (alpha_plane) {
                  if (luminance >= 3968) (data + alpha_offset)[i] = 0;
                } else {
                  uint16_t *pixels = (uint16_t *)(data + 32);
                  pixels[i] = luminance >= 3968 ? pixels[i] & 65534u
                                                : pixels[i] | 1u;
                }
              }
            }
            masked = true;
          }
          free(mask_data);
        }
      }
    }
    if (!masked)
      debugf("DIRECTOR64 NATIVE_MASK_MEMBER_MISSING member=%lu\n",
             (unsigned long)member->id);
  }
  image_mask_us += get_ticks_us() - mask_started;
#endif
  slot->follow_alpha = follow_alpha;
  slot->format = rgba32 ? IMAGE_RGBA32 : ci_bits == 4 ? IMAGE_CI4 : ci_bits == 8 ? IMAGE_CI8 : IMAGE_RGBA16;
  slot->alpha_offset = alpha_offset;
  slot->colors_m1 = (uint8_t)(colors ? colors - 1 : 0);
  data_cache_hit_writeback(data, (size_t)stored);
  snprintf(slot->name, sizeof(slot->name), "%s", member->asset);
  slot->bundled = bundled;
  slot->data = data;
  slot->bytes = (unsigned)stored;
  slot->last = render_serial;
  slot->ink = ink;
  if (!bundled) {
    cache_key[slot - cache] = key;
    cache_bytes += slot->bytes;
    if (regional) {
      slot->region_offset = image_region_pending_offset + 1;
      image_region_pending_bytes = 0;
    } else {
      cache_arena_bytes += slot->bytes;
    }
  }
#if DG_CAP_CACHE_STATS
  slot->loaded_serial=render_serial;
  cache_loads++;
  cache_load_bytes += slot->bytes;
  if (slot->bytes > cache_load_peak) cache_load_peak = slot->bytes;
  if (cache_bytes > cache_peak) cache_peak = cache_bytes;
  heap_stats_t stats;
  counted_heap_stats(&stats);
  if (stats.free < cache_min_free) cache_min_free = stats.free;
#endif
  return slot;
}
typedef struct {
  dg_cursor_t cursor;
  dg_cursor_bitmap_t bitmap;
  char image[32], mask[32];
  int16_t reg_x, reg_y;
  uint16_t width, height, mask_width, mask_height;
  unsigned last;
} cached_cursor_t;
static cached_cursor_t cursor_cache[16];
static unsigned cursor_serial;

static const dg_member_t *cursor_member(uint32_t id) {
  const dg_member_t *m = dg_member(director, id);
#if DG_D5
  if (!m && director->suspended_stage)
    m = dg_member(director->suspended_stage, id);
#endif
  return m;
}
static bool cursor_plane(const dg_member_t *m, uint16_t pixels[256]) {
  cached_image_t *texture = image(m, 0);
  if (!texture) return false;
  memset(pixels, 0, 256 * sizeof(*pixels));
  for (unsigned y = 0; y < 16 && y < m->height; y++)
    for (unsigned x = 0; x < 16 && x < m->width; x++) {
      unsigned i = bitmap_needs_tiles(m->width, m->height) ? bitmap_tile_index(m->width, x, y) : y * m->width + x;
      const uint8_t *data = (const uint8_t *)texture->data + 32;
      if (image_ci_bits(texture)) {
        pixels[y * 16 + x] = dg_fdic_texel((const uint8_t *)texture->data, image_ci_bits(texture),
                                           image_index_offset(texture), m->width, m->height, x, y);
      } else if (texture->format == IMAGE_RGBA32) {
        unsigned rgb = ((const uint32_t *)data)[i] >> 8;
        if (rgb && rgb != 0xffffff) {
          lv_fail(values, "unsupported color cursor bitmap");
          return false;
        }
        pixels[y * 16 + x] = rgb ? 0xffff : 1;
      } else pixels[y * 16 + x] = ((const uint16_t *)data)[i];
    }
  return true;
}
static const dg_cursor_bitmap_t *cursor_bitmap(dg_cursor_t cursor) {
  static dg_cursor_bitmap_t builtin;
  if (!cursor.image) {
    if (!dg_cursor_builtin(&builtin, cursor.resource)) {
      lv_fail(values, "unsupported cursor resource");
      return NULL;
    }
    return &builtin;
  }
  const dg_member_t *m = cursor_member(cursor.image);
  const dg_member_t *mask = cursor.mask ? cursor_member(cursor.mask) : NULL;
  if (!m || !m->asset || (cursor.mask && (!mask || !mask->asset))) {
    lv_fail(values, "missing cursor bitmap member");
    return NULL;
  }
  const char *mask_asset = mask ? mask->asset : "";
  unsigned mw = mask ? mask->width : 0, mh = mask ? mask->height : 0;
  cached_cursor_t *slot = &cursor_cache[0];
  for (unsigned i = 0; i < 16; i++) {
    cached_cursor_t *c = &cursor_cache[i];
    if (c->last && dg_cursor_equal(c->cursor, cursor) &&
        !strcmp(c->image, m->asset) && !strcmp(c->mask, mask_asset) &&
        c->reg_x == m->reg_x && c->reg_y == m->reg_y &&
        c->width == m->width && c->height == m->height &&
        c->mask_width == mw && c->mask_height == mh) {
      c->last = ++cursor_serial;
      return &c->bitmap;
    }
    if (c->last < slot->last) slot = c;
  }
  uint16_t pixels[256], coverage[256];
  if (!cursor_plane(m, pixels) || (mask && !cursor_plane(mask, coverage))) return NULL;
  dg_cursor_bitmap_t bitmap;
  if (!dg_cursor_compose(&bitmap, pixels, m->width, m->height,
      mask ? coverage : NULL, mw, mh, m->reg_x, m->reg_y)) {
    lv_fail(values, "unsupported color cursor bitmap");
    return NULL;
  }
  *slot = (cached_cursor_t){.cursor=cursor, .bitmap=bitmap, .reg_x=m->reg_x,
      .reg_y=m->reg_y, .width=m->width, .height=m->height,
      .mask_width=mw, .mask_height=mh, .last=++cursor_serial};
  snprintf(slot->image, sizeof(slot->image), "%s", m->asset);
  snprintf(slot->mask, sizeof(slot->mask), "%s", mask_asset);
  debugf("DIRECTOR64 NATIVE_CURSOR_LOAD image=%lu mask=%lu hotspot=%u,%u hash=%lu\n",
         (unsigned long)cursor.image, (unsigned long)cursor.mask, bitmap.hot_x, bitmap.hot_y,
         (unsigned long)dg_cursor_bitmap_hash(&bitmap));
  return &slot->bitmap;
}
static void draw_cursor(display_job_t *job, unsigned player, dg_cursor_t cursor,
                        int x, int y) {
  if (!cursor.image && cursor.resource == 200) return;
  const dg_cursor_bitmap_t *bitmap = cursor_bitmap(cursor);
  if (!bitmap) return;
  // Each framebuffer job owns its pixels through RDP completion. The small
  // CPU bitmap cache can be replaced without touching an in-flight texture.
  // Director only ever resolves one cursor, so every player wears the shape
  // the engine is showing and their own colour tells them apart.
  uint16_t *pixels = job->cursor_pixels[player];
  pointer_color_t color = pointer_player_color(player);
  dg_cursor_pixels_ink(bitmap, pixels, dg_cursor_ink(color.r, color.g, color.b));
  data_cache_hit_writeback(pixels, sizeof(job->cursor_pixels[player]));
  surface_t surface = surface_make_linear(pixels, FMT_RGBA16, 16, 16);
  rdpq_set_mode_standard();
  rdpq_mode_filter(FILTER_POINT);
  rdpq_mode_alphacompare(1);
  rdpq_tex_blit(&surface, x - bitmap->hot_x, y - bitmap->hot_y, NULL);
}
static void display_done(void *arg) {
  display_job_t *job = arg;
  completed_serial = job->serial;
#if DG_EXTENDED
  completed_frame = job->frame;
#endif
  display_show(job->surface);
  job->active = false;
}
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx;
  /* Director only uses the bitmap matte for matte ink. Background-transparent
   * sprites still receive input throughout their rectangular bounds. */
  if (ink != 8)
    return true;
#if DG_D10
  // A script-driven poll must not pull a full-stage plane into the heap
  // mid-frame: the render pre-pass places those with compaction. An uncached
  // plane-sized matte answers throughout its bounds until it is drawn.
  if ((unsigned)m->width * m->height >= 100000) {
    if (!image_cached(m->asset, ink))
      return true;
  }
#endif
  cached_image_t *texture = image(m, ink);
  if (!texture || x < 0 || y < 0 || x >= m->width || y >= m->height)
    return false;
  unsigned tex_width = m->width, tex_height = m->height;
#if DG_D10
  // Map the authored-space pixel onto the prescaled texture.
  dg_prescale_dims(m->width, m->height, &tex_width, &tex_height);
  x = (int)((unsigned)x * tex_width / m->width);
  y = (int)((unsigned)y * tex_height / m->height);
  if ((unsigned)x >= tex_width) x = (int)tex_width - 1;
  if ((unsigned)y >= tex_height) y = (int)tex_height - 1;
#endif
  unsigned pixel = bitmap_needs_tiles(tex_width, tex_height) ? bitmap_tile_index(tex_width, x, y)
                                  : (unsigned)y * tex_width + x;
  if (texture->alpha_offset) {
    const uint8_t *alpha = (const uint8_t *)texture->data + texture->alpha_offset;
    return alpha[pixel] != 0;
  }
  if (image_ci_bits(texture))
    return (dg_fdic_texel((const uint8_t *)texture->data, image_ci_bits(texture),
                          image_index_offset(texture), tex_width, tex_height,
                          (unsigned)x, (unsigned)y) & 1) != 0;
  if (texture->format == IMAGE_RGBA32) {
    const uint8_t *pixels = (const uint8_t *)texture->data + 32;
    return pixels[pixel * 4 + 3] != 0;
  }
  const uint16_t *pixels = (const uint16_t *)((uint8_t *)texture->data + 32);
  return (pixels[pixel] & 1) != 0;
}
static color_t indexed_color(unsigned index) {
  uint32_t rgb = director->movie->palette[index & 255];
  return RGBA32(rgb >> 16, (rgb >> 8) & 255, rgb & 255, 255);
}
#if DG_MODERN
static color_t sprite_color(const dg_sprite_t *s, bool back) {
  uint32_t rgb = back ? s->value.back_rgb : s->value.fore_rgb;
  if (!(s->value.flags & (back ? 32 : 16)))
    return indexed_color(back ? s->value.back : s->value.fore);
  return RGBA32(rgb >> 16, (rgb >> 8) & 255, rgb & 255, 255);
}
static void dark_bitmap(surface_t *fb, const surface_t *texture,
                        const surface_t *alpha, const cached_image_t *image,
                        const dg_sprite_t *s, int l, int t, int r, int b,
                        unsigned opacity) {
  float q[8];
  dg_sprite_quad(director, s, q);
#if DG_D10
  // The quad is authored 800x600 geometry; the framebuffer walk below runs
  // in already-prescaled screen coordinates.
  for (unsigned i = 0; i < 8; i++)
    q[i] *= 0.8f;
#endif
  if (l < 0)
    l = 0;
  if (t < 0)
    t = 0;
  if (r > fb->width)
    r = fb->width;
  if (b > fb->height)
    b = fb->height;
  // Darken needs the already composited destination. The installed SDK owns
  // uncached display surfaces; detach_wait completes RDP writes before CPU
  // access, then attach resumes later sprites on the same framebuffer.
  rdpq_detach_wait();
  for (int y = t; y < b; y++) {
    uint16_t *destination =
        (uint16_t *)((uint8_t *)fb->buffer + y * fb->stride);
    for (int x = l; x < r; x++) {
      float u, v;
      if (!dg_quad_uv(q, x + 0.5f, y + 0.5f, &u, &v))
        continue;
      if (s->flip_h)
        u = 1 - u;
      if (s->flip_v)
        v = 1 - v;
      unsigned sx = u * texture->width, sy = v * texture->height;
      if (sx >= texture->width)
        sx = texture->width - 1;
      if (sy >= texture->height)
        sy = texture->height - 1;
      unsigned pixel = bitmap_needs_tiles(texture->width, texture->height)
          ? bitmap_tile_index(texture->width, sx, sy) : sy * texture->width + sx;
      const void *source = texture->buffer;
      destination[x] = alpha
          ? dg_dark_alpha16(((const uint16_t *)source)[pixel],
                            ((const uint8_t *)alpha->buffer)[pixel],
                            destination[x], opacity)
          : image_ci_bits(image)
          ? dg_dark_rgba16(dg_fdic_texel((const uint8_t *)image->data, image_ci_bits(image),
                                         image_index_offset(image), texture->width,
                                         texture->height, sx, sy),
                           destination[x], opacity)
          : surface_get_format(texture) == FMT_RGBA32
          ? dg_dark_rgba32(((const uint32_t *)source)[pixel], destination[x], opacity)
          : dg_dark_rgba16(((const uint16_t *)source)[pixel], destination[x], opacity);
    }
  }
  rdpq_attach(fb, NULL);
}
#endif
static inline __attribute__((always_inline)) void tiled_bitmap_impl(
    const surface_t *surface, const surface_t *alpha, const dg_sprite_t *s,
    int l, int t, int r, int b, bool packed) {
#if DG_MODERN
  bool transformed = s->value.rotation || s->value.skew || s->flip_h || s->flip_v ||
                     s->custom_quad;
#else
  bool transformed = false;
#endif
  float q[8];
  if (transformed) {
    dg_sprite_quad(director, s, q);
#if DG_D10
    // The quad is authored 800x600 geometry; the rectangle path receives its
    // bounds already prescaled, but these vertices rasterize directly.
    for (unsigned i = 0; i < 8; i++)
      q[i] *= 0.8f;
#endif
  }
  unsigned first_x = 0, first_y = 0, end_x = surface->width, end_y = surface->height;
  if (!transformed) {
    bitmap_visible_tiles(surface->width, l, r, 640, &first_x, &end_x);
    bitmap_visible_tiles(surface->height, t, b, 480, &first_y, &end_y);
  }
  // RGBA16+I8 planes consume 3072 bytes per32x32 pair. Upload in ascending
  // tile order: the SDK may use tile+1 as temporary state during each upload.
  // Linear subuploads retain source UVs; packed tiles use local UVs. All pixel
  // data remains cache-owned until the queued display fence completes.
  for (int y = first_y; y < (int)end_y; y += 32) {
    int bottom = y + 32 < surface->height ? y + 32 : surface->height;
    for (int x = first_x; x < (int)end_x; x += 32) {
      int right = x + 32 < surface->width ? x + 32 : surface->width;
      int u0 = x, v0 = y, u1 = right, v1 = bottom;
      surface_t color_tile, alpha_tile;
      const surface_t *color_source = surface, *alpha_source = alpha;
      if (packed) {
        unsigned pixel = bitmap_tile_index(surface->width, x, y);
        tex_format_t format = surface_get_format(surface);
        color_tile = surface_make_linear(
            (uint8_t *)surface->buffer + TEX_FORMAT_PIX2BYTES(format, pixel),
            format, 32, 32);
        color_source = &color_tile;
        if (alpha) {
          alpha_tile = surface_make_linear((uint8_t *)alpha->buffer + pixel,
                                           FMT_I8, 32, 32);
          alpha_source = &alpha_tile;
        }
        u0 = v0 = 0;
        u1 = right - x;
        v1 = bottom - y;
      }
      if (alpha)
        rdpq_tex_multi_begin();
      rdpq_tex_upload_sub(TILE0, color_source, NULL, u0, v0, u1, v1);
      if (alpha) {
        rdpq_tex_upload_sub(TILE1, alpha_source, NULL, u0, v0, u1, v1);
        rdpq_tex_multi_end();
      }
      if (!transformed) {
        float sx = (float)(r - l) / surface->width;
        float sy = (float)(b - t) / surface->height;
        rdpq_texture_rectangle_scaled(TILE0, l + x * sx, t + y * sy,
                                      l + right * sx, t + bottom * sy,
                                      u0, v0, u1, v1);
        continue;
      }
      float vertices[4][5];
      int px[4] = {x, right, right, x}, py[4] = {y, y, bottom, bottom};
      for (unsigned i = 0; i < 4; i++) {
        float u = (float)px[i] / surface->width,
              v = (float)py[i] / surface->height;
#if DG_MODERN
        if (s->flip_h)
          u = 1 - u;
        if (s->flip_v)
          v = 1 - v;
#endif
        vertices[i][0] = (1 - v) * ((1 - u) * q[0] + u * q[2]) +
                         v * ((1 - u) * q[6] + u * q[4]);
        vertices[i][1] = (1 - v) * ((1 - u) * q[1] + u * q[3]) +
                         v * ((1 - u) * q[7] + u * q[5]);
        vertices[i][2] = px[i] - (packed ? x : 0);
        vertices[i][3] = py[i] - (packed ? y : 0);
        vertices[i][4] = 1;
      }
      rdpq_triangle(&TRIFMT_TEX, vertices[0], vertices[1], vertices[2]);
      rdpq_triangle(&TRIFMT_TEX, vertices[0], vertices[2], vertices[3]);
    }
  }
}
static void tiled_bitmap(const surface_t *surface, const surface_t *alpha,
                          const dg_sprite_t *s, int l, int t, int r, int b) {
  // Specialize the loops: ordinary images keep the original fast subupload
  // path, without packed-coordinate branches and surface setup per tile.
  if (bitmap_needs_tiles(surface->width, surface->height))
    tiled_bitmap_impl(surface, alpha, s, l, t, r, b, true);
  else
    tiled_bitmap_impl(surface, alpha, s, l, t, r, b, false);
}
static void draw_scene(surface_t *fb, dg_runtime_t *scene, int offset_x, int offset_y) {
  (void)fb;
  dg_runtime_t *previous=director;director=scene;
#if DG_MODERN
  uint32_t background = director->stage_color;
  rdpq_set_mode_fill(
      RGBA32(background >> 16, (background >> 8) & 255, background & 255, 255));
#else
  rdpq_set_mode_fill(RGBA32(255, 255, 255, 255));
#endif
  rdpq_fill_rectangle(0, 0, 640, 480);
  const uint16_t *order;
  uint64_t order_started = get_ticks_us();
  unsigned count = dg_draw_order(director, &order);
  phase_order_us += get_ticks_us() - order_started;
  unsigned first_draw = 0;
#if DG_EXTENDED
  // The screens stack several full-stage planes (login background, workshop
  // photo backdrop); everything beneath an opaque full-stage copy-ink plane
  // is invisible, and skipping it keeps those planes out of the image heap.
  for (unsigned draw = count; draw-- > 1;) {
    unsigned i = order[draw];
    const dg_sprite_t *s = &director->sprites[i];
    const dg_member_t *m = dg_member(director, s->value.member);
    if (!m || m->type != 1 || !s->visible || !s->value.type)
      continue;
    unsigned cover_ink = s->value.ink;
#if DG_D10
    // Unpaired mask ink copies; the class panel's opaque full-stage base is
    // authored that way and must release the login plane beneath it.
    if (cover_ink == 9) {
      const dg_member_t *mask_member = dg_member(director, m->id + 1);
      if (!(mask_member && mask_member->type == 1 && mask_member->asset[0] &&
            mask_pair(m, mask_member)))
        cover_ink = 0;
    }
#endif
    if (cover_ink != 0 || dg_opacity(s) != 100)
      continue;
    if (s->value.rotation || s->value.skew || s->flip_h || s->flip_v ||
        s->custom_quad)
      continue;
    int l, t, r, b;
    dg_sprite_bounds(director, s, &l, &t, &r, &b);
#if DG_D10
    l = dg_prescale_coord(l); t = dg_prescale_coord(t);
    r = dg_prescale_coord(r); b = dg_prescale_coord(b);
#endif
    if (l <= 0 && t <= 0 && r >= 640 && b >= 480) {
      first_draw = draw;
      break;
    }
  }
  // Every image loaded while this frame is queued stays pinned, so loading in
  // draw order can fragment the heap until a full-stage plane (the login car
  // preview) no longer finds a contiguous block. Load the frame's missing
  // bitmaps largest-first while the free region is still coalesced.
  {
    unsigned pending[DG_SPRITES], pixels[DG_SPRITES], pending_count = 0;
    for (unsigned draw = first_draw; draw < count; draw++) {
      unsigned i = order[draw];
      const dg_sprite_t *s = &director->sprites[i];
      const dg_member_t *m = dg_member(director, s->value.member);
      if (!m || !s->visible || !s->value.type || !dg_opacity(s))
        continue;
      if (m->type != 1 && !(m->type == 2 && m->film_count))
        continue;
      if (!m->width || !m->height)
        continue;
      int l, t, r, b;
      dg_sprite_bounds(director, s, &l, &t, &r, &b);
#if DG_D10
      l = dg_prescale_coord(l); t = dg_prescale_coord(t);
      r = dg_prescale_coord(r); b = dg_prescale_coord(b);
#endif
      if (r <= 0 || b <= 0 || l >= 640 || t >= 480 || r <= l || b <= t)
        continue;
      pending[pending_count] = i;
      // Order by stored bytes, not authored pixels: an alpha plane stores
      // half again as much as an opaque image of the same size and must
      // claim the coalesced region first.
      const char *weight_asset = m->asset;
      if (m->type == 2)
        weight_asset = m->film_assets[dg_film_pose(m, s)];
      size_t weight_length = weight_asset ? strlen(weight_asset) : 0;
      unsigned weight_stride = 2;
      if (weight_length >= 4 && !strcmp(weight_asset + weight_length - 4, ".fda"))
        weight_stride = 3;
      else if (weight_length >= 4 &&
               !strcmp(weight_asset + weight_length - 4, ".fd2"))
        weight_stride = 4;
      pixels[pending_count] = (unsigned)m->width * m->height * weight_stride;
      pending_count++;
    }
    // A large plane that is not cached yet must not fight the frame's other
    // pinned images for leftover holes; drain queued frames and evict
    // everything first so they pack back-to-back into one coalesced region
    // (the login screen keeps two 640x480 planes alive at once). Compacting
    // costs a whole-scene reload, so do it only when the heap cannot serve
    // the largest missing image as it stands: the map's cycling film poses
    // are uncached on every pose change but ordinarily fit.
    unsigned needed = 0;
    for (unsigned j = 0; j < pending_count; j++) {
      if (pixels[j] < 200000 || pixels[j] <= needed)
        continue;
      const dg_sprite_t *s = &director->sprites[pending[j]];
      const dg_member_t *m = dg_member(director, s->value.member);
      const char *asset = m->asset;
      if (m->type == 2)
        asset = m->film_assets[dg_film_pose(m, s)];
      unsigned plane_ink = s->value.ink;
      if (!image_cached(asset, plane_ink))
        needed = pixels[j];
    }
    bool compact = false;
    if (needed) {
      void *probe = memalign(32, needed);
      compact = !probe;
      free(probe);
    }
    // A fragmented allocation skipped a sprite last render; repack once so
    // the frame's whole set reloads coalesced. Never two renders in a row:
    // a genuinely over-budget scene must degrade to skipped sprites, not
    // relive the compaction storm.
    if (image_alloc_soft_fail) {
      image_alloc_soft_fail = false;
      if (compact_serial + 1 < render_serial)
        compact = true;
    }
    if (compact) {
      // Also release the lazily built font atlases: they were allocated
      // mid-scene and can split the free region so that two planes and an
      // atlas no longer tile. They rebuild after the planes are placed.
      // Draining queued frames first releases their pinned images so the
      // sweep really empties the heap before the coalesced reload.
      rspq_wait();
      clear_text_fonts();
      while (evict()) {}
      compact_serial = render_serial;
    }
    while (pending_count && !values->failed) {
      unsigned best = 0;
      for (unsigned j = 1; j < pending_count; j++)
        if (pixels[j] > pixels[best])
          best = j;
      const dg_sprite_t *s = &director->sprites[pending[best]];
      const dg_member_t *m = dg_member(director, s->value.member);
      dg_member_t film;
      if (m->type == 2) {
        film = *m;
        film.asset = m->film_assets[dg_film_pose(m, s)];
        film.type = 1;
        m = &film;
      }
      unsigned ink = s->value.ink;
      if (ink == 0 || ink == 8 || ink == 32 || ink == 36 || ink == 39
#if DG_D10
          // Mask ink loads with the next member's mask baked into its alpha.
          || ink == 9
#endif
      )
        image(m, ink);
      pending[best] = pending[--pending_count];
      pixels[best] = pixels[pending_count];
    }
  }
#endif
  for (unsigned draw = first_draw; draw < count && !values->failed; draw++) {
    unsigned i = order[draw];
    for (unsigned pass = 0; pass <= director->trail_count; pass++) {
      if (pass < director->trail_count && director->trails[pass].channel != i)
        continue;
      const dg_sprite_t *s = pass < director->trail_count
                                 ? &director->trails[pass].sprite
                                 : &director->sprites[i];
      dg_sprite_t shifted;
      if(offset_x || offset_y) {
        shifted=*s;shifted.value.x+=offset_x;shifted.value.y+=offset_y;s=&shifted;
      }
      const dg_member_t *m = dg_member(director, s->value.member);
      // Palettes, sounds and script casts have no visual widget in Director.
      if (m && (m->type == 4 || m->type == 6 || m->type == 11 || m->type == 14))
        continue;
      if (m && (m->type == 1 || m->type == 2) && (!m->width || !m->height))
        continue;
      dg_member_t film;
      if (m && m->type == 2 && m->film_count) {
        film = *m;
        unsigned frame = dg_film_pose(m, s);
        film.asset = m->film_assets[frame];
        film.type = 1;
        m = &film;
      }
      unsigned opacity = dg_opacity(s);
      if (!s->visible || !s->value.type || !opacity)
        continue;
      int l, t, r, b;
      dg_sprite_bounds(director, s, &l, &t, &r, &b);
#if DG_D10
      // Authored 800x600 geometry rasterizes onto the 640x480 screen.
      l = dg_prescale_coord(l); t = dg_prescale_coord(t);
      r = dg_prescale_coord(r); b = dg_prescale_coord(b);
#endif
      if (r <= 0 || b <= 0 || l >= 640 || t >= 480 || r <= l || b <= t)
        continue;
#if DG_D5
      if (m && m->type == 10) {
        d5_video_draw(i, l, t, r, b);
        continue;
      }
#endif
      if (m && m->type == 1) {
        unsigned ink = s->value.ink;
        if (ink != 0 && ink != 8 && ink != 36
#if DG_MODERN
            && ink != 32 && ink != 39
#endif
#if DG_D10
            // Mask ink loads with the next member's mask baked into alpha.
            && ink != 9
#endif
        ) {
          lv_fail(values, "unsupported native bitmap ink");
          break;
        }
#if DG_EXTENDED
        // A full-stage plane appearing mid-frame would allocate while this
        // frame pins every earlier load and can fail on fragmentation; show
        // it one frame later, after the pre-pass placed it with compaction.
        if (pass == director->trail_count &&
            (unsigned)m->width * m->height >= 100000) {
          if (!image_cached(m->asset, ink))
            continue;
        }
#endif
        cached_image_t *texture = image(m, ink);
        if (!texture) {
          if (values->failed)
            break;
          continue; // skipped this frame; the repacked next render draws it
        }
        unsigned tex_width = m->width, tex_height = m->height;
#if DG_D10
        dg_prescale_dims(m->width, m->height, &tex_width, &tex_height);
#endif
        surface_t surface = surface_make_linear(
            (uint8_t *)texture->data + (image_ci_bits(texture) ? image_index_offset(texture) : 32),
            texture->format == IMAGE_CI4 ? FMT_CI4 : texture->format == IMAGE_CI8 ? FMT_CI8
            : texture->format == IMAGE_RGBA32 ? FMT_RGBA32 : FMT_RGBA16,
            tex_width, tex_height);
#if DG_MODERN
        surface_t alpha_surface = {0};
        surface_t *alpha = NULL;
        if (texture->alpha_offset) {
          alpha_surface = surface_make_linear(
              (uint8_t *)texture->data + texture->alpha_offset, FMT_I8,
              tex_width, tex_height);
          alpha = &alpha_surface;
        }
        if (ink == 39) {
          dark_bitmap(fb, &surface, alpha, texture, s, l, t, r, b, opacity);
#if DG_D10
          goto flash_overlay;
#else
          continue;
#endif
        }
#endif
#if DG_CAP_DRAW_BLOCKS
        // A recording holds the sprite's quad; only an unrotated, unflipped
        // sprite drawn as its rectangle can replay one. Plain D6 sprites have
        // no other geometry.
#if DG_MODERN
        bool block_geometry = !s->value.rotation && !s->value.skew &&
            !s->flip_h && !s->flip_v && !s->custom_quad;
#else
        bool block_geometry = true;
#endif
        if (block_geometry && texture->block && texture->block_opacity == opacity &&
            texture->block_rect[0] == l && texture->block_rect[1] == t &&
            texture->block_rect[2] == r && texture->block_rect[3] == b) {
          rspq_block_run(texture->block);
#if DG_D10
          goto flash_overlay;
#else
          continue;
#endif
        }
        // Cache repeated bitmap command setup, alongside its fenced pixels.
        // Moving/reused geometry takes the ordinary draw path. Never record
        // CPU Darken work or retain pointers to an unloadable movie overlay.
        heap_stats_t block_before = {0};
        bool record_block = false;
        if (block_geometry && !texture->block && texture->loaded_serial < completed_serial &&
            m->width * m->height >= 16384) {
          unsigned estimate = ((m->width+31)/32)*((m->height+31)/32)*256 + 4096;
          sys_get_heap_stats(&block_before);
          // The heap this must leave alone. Without a block the arena also
          // carries megabytes of image pixels and an image load is the
          // allocation most likely to fail behind this one; with a block
          // the pixels are elsewhere and what remains is small and steady,
          // so holding back a quarter megabyte only buys redrawing. At the
          // larger reserve the train station recorded 62 KB of draw
          // commands instead of 101 KB and paid for it in render time.
#if DG_D8
          unsigned reserve = IMAGE_REGION_ACTIVE ? 128 * 1024 : 256 * 1024;
#else
          unsigned reserve = 256 * 1024;
#endif
          record_block = estimate <= BITMAP_BLOCK_BUDGET-bitmap_block_bytes &&
                         block_before.free > (int)(estimate*2 + reserve);
        }
        if (record_block) {
          rspq_block_begin();
          recording_block = true;
        }
#endif
        rdpq_set_mode_standard();
        if (image_ci_bits(texture)) {
          // The palette rides in the upper half of TMEM; the index tiles the
          // draw uploads below fit in the lower half, as every 32x32 tile
          // does at eight bits per pixel.
          rdpq_mode_tlut(TLUT_RGBA16);
          rdpq_tex_upload_tlut((uint16_t *)((uint8_t *)texture->data + 32), 0,
                               texture->colors_m1 + 1);
        } else {
          rdpq_mode_tlut(TLUT_NONE);
        }
        if (ink || texture->follow_alpha)
          rdpq_mode_alphacompare(1);
        if (texture->follow_alpha)
          rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        if (opacity < 100) {
          rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
          rdpq_set_prim_color(RGBA32(255, 255, 255, opacity * 255 / 100));
          rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        }
#if DG_MODERN
        if (alpha) {
          // The RDP shifts alpha compare in two-cycle mode; source-over already
          // leaves the destination intact for zero coverage.
          rdpq_mode_alphacompare(0);
          // Read TEX0 only in cycle0, avoiding the RDP's pipelined TEX0_BUG in
          // cycle1. I8 replicates its intensity into alpha. At100% opacity the
          // alpha plane passes through without a lossy multiplication by255.
          rdpq_mode_combiner(opacity < 100
              ? RDPQ_COMBINER2((0, 0, 0, TEX0), (TEX1, 0, PRIM, 0),
                               (0, 0, 0, COMBINED), (0, 0, 0, COMBINED))
              : RDPQ_COMBINER2((0, 0, 0, TEX0), (0, 0, 0, TEX1),
                               (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)));
          tiled_bitmap(&surface, alpha, s, l, t, r, b);
          if (!alpha_draw_reported) {
            debugf("DIRECTOR64 NATIVE_ALPHA_DRAW movie=%s format=FDIA "
                   "rgb_bits=15 alpha_bits=8 tile_bytes=3072 member=%lu\n",
                   director->movie->code->name, (unsigned long)m->id);
            alpha_draw_reported = true;
          }
        } else if (s->value.rotation || s->value.skew || s->flip_h || s->flip_v ||
            s->custom_quad)
          tiled_bitmap(&surface, NULL, s, l, t, r, b);
        else
#endif
        if (bitmap_needs_tiles(tex_width, tex_height))
          tiled_bitmap(&surface, NULL, s, l, t, r, b);
        else
          rdpq_tex_blit(
              &surface, l, t,
              &(rdpq_blitparms_t){.scale_x = (float)(r - l) / surface.width,
                                  .scale_y = (float)(b - t) / surface.height});
#if DG_CAP_DRAW_BLOCKS
        if (record_block) {
          rspq_block_t *block = rspq_block_end();
          recording_block = false;
          heap_stats_t after;sys_get_heap_stats(&after);
          unsigned bytes = block_before.free > after.free ? block_before.free-after.free : 0;
          rspq_block_run(block);
          if (bytes <= BITMAP_BLOCK_BUDGET-bitmap_block_bytes) {
            texture->block=block;texture->block_bytes=bytes;bitmap_block_bytes+=bytes;
            texture->block_rect[0]=l;texture->block_rect[1]=t;
            texture->block_rect[2]=r;texture->block_rect[3]=b;
            texture->block_opacity=opacity;
          } else {
            rspq_wait();rspq_block_free(block);
          }
        }
#endif
      } else if (m && (m->type == 3 || m->type == 7)) {
        const char *text = m->text;
        unsigned color = s->value.fore;
#if DG_MODERN
        const dg_text_style_t *text_style = m->text_style;
        const char *override_font = NULL;
        int text_scroll = 0;
        color_t text_color = text_style
            ? RGBA32(text_style->color >> 16, (text_style->color >> 8) & 255,
                     text_style->color & 255, 255)
            : sprite_color(s, false);
#endif
        for (unsigned j = 0; j < director->field_count; j++)
          if (director->field_members[j] == m->id) {
            text = lv_cstr(values, values->roots[DG_FIELD_TEXT_ROOT + j]);
#if DG_MODERN
            override_font = director->field_fonts[j];
            text_scroll = director->field_scroll[j];
            if (director->field_text_changed[j] && m->text_insert_style) {
              text_style = m->text_insert_style;
              text_color = RGBA32(text_style->color >> 16,
                  (text_style->color >> 8) & 255, text_style->color & 255, 255);
            }
#endif
            if (lv_type(values->roots[DG_FIELD_COLOR_ROOT + j]) != LV_VOID) {
              color =
                  lv_integer(values, values->roots[DG_FIELD_COLOR_ROOT + j]);
#if DG_MODERN
              text_color =
                  lv_type(values->roots[DG_FIELD_COLOR_ROOT + j]) == LV_COLOR || color > 255
                      ? RGBA32(color >> 16, (color >> 8) & 255, color & 255, 255)
                      : indexed_color(color);
#endif
            }
          }
#if DG_MODERN
        text_color.a = dg_opacity(s) * 255 / 100;
        draw_text(text_style, override_font, text, text_color, l, t, r, b, m->id, text_scroll);
#else
        rdpq_font_style(font, 1,
                        &(rdpq_fontstyle_t){.color = indexed_color(color)});
        // Doubled markup characters: field text is the game's, not markup.
        static char plain[4096];
        rdpq_text_print(
            &(rdpq_textparms_t){.style_id = 1, .width = r - l, .height = b - t},
            1, l, t + 12, text_plain(text, plain, sizeof(plain)));
#endif
      } else if ((m && m->type == 8) || s->value.type < 16) {
        unsigned thickness = s->value.thickness & 15;
        unsigned line = thickness ? thickness - 1 : 0;
        if (m && !m->filled && !line)
          continue; // Invisible Director hit rectangle.
#if DG_MODERN
        color_t foreground = sprite_color(s, false);
        color_t background = sprite_color(s, true);
        // Background-transparent shapes with matching colors are invisible
        // hit areas. Willy's save/load sheet uses one above the car preview.
        if (s->value.ink == 36 && foreground.r == background.r &&
            foreground.g == background.g && foreground.b == background.b)
          continue;
        rdpq_set_mode_fill(foreground);
#else
        rdpq_set_mode_fill(indexed_color(s->value.fore));
#endif
        unsigned kind = m ? m->shape : 1;
        bool filled = !m || m->filled;
        if (kind == 1 && filled)
          rdpq_fill_rectangle(l, t, r, b);
        else {
          for (int y = t < 0 ? 0 : t; y < b && y < 480; y++) {
            int spans[4];
            unsigned count =
                dg_shape_spans(kind, r - l, b - t, y - t, line, filled,
                               m && m->line_direction == 6, spans);
            for (unsigned j = 0; j < count; j++)
              if (spans[j * 2] < spans[j * 2 + 1])
                rdpq_fill_rectangle(l + spans[j * 2], y, l + spans[j * 2 + 1],
                                    y + 1);
          }
        }
      } else if (m) {
#if DG_D10
        // Policy-pinned deferred Xtra members (the Flash intro timelines)
        // stay data-only: their sprites draw nothing, visibly traced, per
        // each pin's recorded disposition. Other types still fail closed.
        if (m->type == 15) {
          debugf("DIRECTOR64 NATIVE_RENDER_DEFERRED member=%lu sprite=%u\n",
                 (unsigned long)m->id, i);
          continue;
        }
#endif
        debugf("DIRECTOR64 NATIVE_RENDER_UNSUPPORTED member=%lu type=%u "
               "sprite=%u\n",
               (unsigned long)m->id, m->type, i);
        lv_fail(values, "unsupported visible cast type");
      }
#if DG_D10
    flash_overlay:
      // Prompt overlays draw above their card in this sprite's own order,
      // so a later cover sprite still hides them.
      uint64_t flash_started = get_ticks_us();
      draw_flash_overlays(i, s, m);
      phase_flash_us += get_ticks_us() - flash_started;
#endif
    }
  }
  director=previous;
}
// Everything outside the score that the composite depends on. The stage's own
// state is `visual_revision`; these are the platform's own overlays and the
// one decoder that repaints a sprite without touching its record.
static unsigned render_externals(void) {
  unsigned key = 0;
#if DG_D5
  key = key * 31 + d5_video_revision();
  key = key * 31 + (director->ticks < director->alert_until);
  key = key * 31 + director->native_dialog;
#endif
#ifdef DIRECTOR64_PRINTING
  key = key * 31 + game_print_id();
#endif
#ifdef DIRECTOR64_WILLY
  key = key * 31 + (*director->notice != 0);
#endif
  return key;
}
// Where a player's cursor lands on the 640x480 screen, or false when nobody is
// using that port. The driver's position comes from the engine, which owns
// the authored stage space; the others point in screen space already.
static bool pointer_screen(unsigned player, int *out_x, int *out_y) {
  const pointer_player_t *q = &pointer.players[player];
  bool driving = player == pointer.active;
  if (!pointer_visible(&pointer, player)) return false;
  int x = q->input.x / INPUT_ONE, y = q->input.y / INPUT_ONE;
  if (driving) {
    x = director->mouse_x;
    y = director->mouse_y;
#if DG_D10
    // The runtime holds the pointer in authored 800x600 space; the cursor
    // sprite renders back on the 640x480 screen.
    x = dg_prescale_coord(x);
    y = dg_prescale_coord(y);
#endif
  }
#if DG_D5
  if (director->native_dialog) { x += 112; y += 100; }
#endif
  *out_x = x;
  *out_y = y;
  return true;
}
static unsigned pointer_key(void) {
  unsigned key = pointer.active + 1;
  for (unsigned i = 0; i < POINTER_PLAYERS; i++) {
    int x = 0, y = 0;
    bool drawn = pointer_screen(i, &x, &y);
    key = key * 31 + drawn;
    if (!drawn) continue;
    key = key * 31 + (unsigned)x;
    key = key * 31 + (unsigned)y;
  }
  return key;
}
static bool render(void) {
  dg_cursor_t cursor = dg_cursor_current(director);
  dg_update_stage(director);
  unsigned externals = render_externals();
#if DG_CAP_TEXT_INPUT
  bool overlay = text_input_active();
#else
  bool overlay = false;
#endif
  unsigned pointers = pointer_key();
  if (frame_cached && !overlay && !cached_overlay &&
#if DG_EXTENDED
      !transition_active && director->transition_serial == transition_consumed &&
#endif
      cached_revision == director->visual_revision &&
      cached_externals == externals &&
#if DG_MODERN
      cached_background == director->stage_color &&
#endif
      dg_cursor_equal(cached_cursor, cursor) && cached_pointers == pointers)
    return false;
  surface_t *fb = display_try_get();
  if (!fb) {
    rspq_flush();
    return false;
  }
  display_job_t *job = !jobs[0].active   ? &jobs[0]
                       : !jobs[1].active ? &jobs[1]
                                         : NULL;
  if (!job) {
    lv_fail(values, "display job ownership");
    return false;
  }
  job->active = true;
  job->surface = fb;
  job->serial = ++render_serial;
  frame_cached = true;
  cached_overlay = overlay;
  cached_pointers = pointers;
  cached_revision = director->visual_revision;
  cached_externals = externals;
#if DG_MODERN
  cached_background = director->stage_color;
#endif
  cached_cursor = cursor;
#if DG_EXTENDED
  job->frame = director->frame;
#endif
  rdpq_attach(fb, NULL);
#if DG_D5
  if(director->native_dialog && director->suspended_stage) {
    draw_scene(fb,director->suspended_stage,0,0);
    rdpq_set_scissor(112,100,528,340);
    draw_scene(fb,director,112,100);
    rdpq_set_scissor(0,0,640,480);
  } else
#endif
  draw_scene(fb,director,0,0);
#if DG_EXTENDED
  if (director->transition_serial != transition_consumed) {
    transition_consumed = director->transition_serial;
    // A reveal needs a previous frame to reveal from; otherwise keep the cut.
    transition_active = previous_frame &&
        (director->transition_type == 1 || director->transition_type == 9 || director->transition_type == 10);
    transition_started = director->ticks;
  }
  if (transition_active) {
    unsigned elapsed = director->ticks - transition_started;
    unsigned duration = director->transition_duration ? director->transition_duration : 15;
    unsigned chunk = director->transition_chunk ? director->transition_chunk : 1;
    int w = fb->width, h = fb->height;
    int cl = 0, ct = 0, cr = w, cb = h;  // region still showing the old frame
    if (elapsed >= duration) {
      transition_active = false;
    } else if (director->transition_type == 1) {  // wipe right
      cl = (int)((int64_t)w * elapsed / duration) / (int)chunk * (int)chunk;
    } else {  // edges-in square
      cl = (int)((int64_t)(w / 2) * elapsed / duration) / (int)chunk * (int)chunk;
      ct = (int)((int64_t)(h / 2) * elapsed / duration) / (int)chunk * (int)chunk;
      cr = w - cl;
      cb = h - ct;
    }
    if (transition_active && director->transition_type == 9) {
      // Center-out reveals the complement of the edges-in rectangle.
      int l = w / 2 - cl, t = h / 2 - ct, r = w - l, b = h - t;
      int regions[4][4] = {{0,0,w,t},{0,b,w,h},{0,t,l,b},{r,t,w,b}};
      rdpq_set_mode_standard();
      rdpq_mode_filter(FILTER_POINT);
      for (unsigned i = 0; i < 4; i++) {
        int *q = regions[i];
        if (q[2] > q[0] && q[3] > q[1])
          rdpq_tex_blit(previous_frame, q[0], q[1], &(rdpq_blitparms_t){
            .s0=q[0], .t0=q[1], .width=q[2]-q[0], .height=q[3]-q[1]});
      }
    } else
    if (transition_active && cl < cr && ct < cb) {
      // Unscaled same-format blit; standard mode avoids copy-mode source
      // alignment constraints at chunk-quantized offsets.
      rdpq_set_mode_standard();
      rdpq_mode_filter(FILTER_POINT);
      rdpq_tex_blit(previous_frame, cl, ct,
                    &(rdpq_blitparms_t){.s0 = cl, .t0 = ct,
                                        .width = cr - cl, .height = cb - ct});
    } else
      transition_active = false;
  }
#endif
#if DG_D5
  if(director->ticks<director->alert_until) {
    rdpq_set_mode_fill(RGBA32(255,255,255,255));
    rdpq_fill_rectangle(48,180,592,290);
    static char plain[1024];
    rdpq_text_print(&(rdpq_textparms_t){.style_id=1,.width=510,.height=80},
                    1,64,212,text_plain(director->alert_text,plain,sizeof(plain)));
  }
#endif
  // The player holding the mouse draws last, so a crowded screen never hides
  // the cursor the engine is actually following.
  for (unsigned n = 0; n < POINTER_PLAYERS; n++) {
    unsigned player = (pointer.active + 1 + n) % POINTER_PLAYERS;
    int x, y;
    if (pointer_screen(player, &x, &y))
      draw_cursor(job, player, cursor, x, y);
  }
#ifdef DIRECTOR64_PRINTING
  if (game_print_document()) {
    unsigned title_font = text_font(1, 24, RGBA32(0,0,0,255));
    unsigned help_font = text_font(1, 18, RGBA32(0,0,0,255));
    if (title_font && help_font) print_ui_draw(title_font, help_font);
  }
#endif
#if DG_CAP_TEXT_INPUT
  if (text_input_active()) {
#if DG_CAP_KEYBOARD
    // LO uses size 15. Keep one additional atlas for both keyboard labels.
    unsigned keyboard_font = text_font(1, 20, RGBA32(255,255,255,255));
    unsigned help_font = keyboard_font;
#else
    unsigned keyboard_font = text_font(1, 24, RGBA32(255,255,255,255));
    unsigned help_font = text_font(1, 18, RGBA32(255,255,255,255));
#endif
    if (keyboard_font && help_font) text_input_draw(keyboard_font, help_font);
  }
#endif
#ifdef DIRECTOR64_WILLY
  if (*director->notice) {
    // Reuse the certificate's 24px atlas: a third size would evict and reload
    // its two source fonts on every paused frame.
    unsigned font = text_font(1, 24, RGBA32(255,255,255,255));
    rdpq_sync_pipe();
    rdpq_set_mode_fill(RGBA32(32,42,52,255));
    rdpq_fill_rectangle(70,170,570,320);
    rdpq_set_mode_standard();
    rdpq_mode_alphacompare(1);
    if (font) {
      static char plain[1024];
      rdpq_text_print(&(rdpq_textparms_t){.style_id=1},font,90,205,
                      text_plain(director->notice,plain,sizeof(plain)));
      rdpq_text_print(&(rdpq_textparms_t){.style_id=1},font,90,290,"A oder B: Zurueck zur Urkunde");
    }
  }
#endif
#if DG_EXTENDED
  previous_frame = fb;
#endif
  rdpq_detach_cb(display_done, job);
  rspq_flush();
  return true;
}
static void service_audio(uint64_t now_us) {
  // Four 40 ms buffers are queued at 22 kHz and the mixer fills up to three
  // ahead, so pumping it every few milliseconds keeps it fed; pumping it on
  // every pass of the loop cost the buffer bookkeeping's cache lines each
  // time (the emulator's miss profile put a sixth of all data and code
  // misses in that polling).
  static uint64_t served_us;
  if (now_us - served_us < 4000) return;
  served_us = now_us;
  /* While a modal overlay pauses the source clock, preserve mixer/decoder
   * positions. Already queued audio drains normally; new buffers are silent. */
  bool paused = false;
#ifdef DIRECTOR64_PRINTING
  paused = paused || game_print_paused();
#endif
#if DG_EXTENDED
  paused = paused || text_input_active();
#endif
  if (!paused) {
    for (unsigned ch = 0; fading && ch < DG_SOUND_CHANNELS; ch++)
      sound_settle(ch, false);
    mixer_try_play();
  } else if (audio_can_write())
    audio_write_silence();
}
static void audio_pump(void) {
  // The mixer queues its RSP work at high priority, which a block being
  // recorded cannot take.
  if (audio_live && !recording_block) service_audio(get_ticks_us());
}
static void notice(const char *heading, const char *message) {
  // Keep errors visible until a console reset, without touching the journal.
  // In particular, corrupted or full storage is never silently reinitialized.
  rdpq_font_style(font, 1,
                  &(rdpq_fontstyle_t){.color = RGBA32(255, 255, 255, 255)});
  for (;;) {
    surface_t *fb = display_get();
    rdpq_attach(fb, NULL);
    rdpq_set_mode_fill(RGBA32(24, 32, 40, 255));
    rdpq_fill_rectangle(0, 0, 640, 480);
    static char plain[2048];
    rdpq_text_print(&(rdpq_textparms_t){.style_id = 1}, 1, 48, 160,
                    text_plain(heading, plain, sizeof(plain)));
    rdpq_text_print(
        &(rdpq_textparms_t){.style_id = 1, .width = 544, .height = 160}, 1, 48,
        200, text_plain(message, plain, sizeof(plain)));
    rdpq_text_print(&(rdpq_textparms_t){.style_id = 1}, 1, 48, 380,
                    "Reset the console to restart.");
    rdpq_detach_show();
  }
}
int main(void) {
  if (!platform_init())
    return 1;
  pack_open();
  // Both runtimes start at a fixed offset into the data cache's 8 KiB period,
  // so which cache set each of their fields lands in is decided by the
  // struct layout alone and not by whatever malloc placed before them. The
  // interpreter's hot header (the first lines of lv_runtime_t) is placed a
  // quarter period in: the main binary's hot statics and the top of the C
  // stack own the sets around the period boundary (the linker script,
  // n64.ld, holds the statics there), and the emulator's miss profile showed
  // the header ping-ponging with them when it sat at zero.
  values = (lv_runtime_t *)((char *)memalign(8192, sizeof(*values) + 8192) + 2048);
  director = (dg_runtime_t *)((char *)memalign(8192, sizeof(*director) + 8192) + 4096);
  // Where the two runtime structures and the binary's static sections sit,
  // for the emulator's cache-miss profile to attribute data misses.
  extern char __data_start[], __bss_start[], __bss_end[];
  debugf("DIRECTOR64 NATIVE_LAYOUT values=%p values_size=%u director=%p director_size=%u"
         " data=%p bss=%p heap=%p\n",
         (void *)values, (unsigned)sizeof(*values), (void *)director,
         (unsigned)sizeof(*director), (void *)__data_start, (void *)__bss_start,
         (void *)__bss_end);
  if (!values || !director)
    return 1;
  audio_init(22050, 4);
#if DG_D5
  mixer_init(D5_MIXER_CHANNELS);
  d5_video_init();
  for (unsigned ch = DG_SOUND_CHANNELS * 2; ch < D5_MIXER_CHANNELS; ch++)
    mixer_ch_set_limits(ch, 16, 44100, 0);
#else
  mixer_init(DG_SOUND_CHANNELS * 2);
#endif
#if DG_MODERN
  // The recovered Mucklas sounds are 16-bit mono at up to 44.1 kHz. Playback
  // keeps the source frequency even when the output device runs at 22.05 kHz.
  // Reserve both halves of each logical voice for libdragon's stereo pairing.
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS * 2; ch++)
    mixer_ch_set_limits(ch, 16, 44100, 0);
#endif
  wav64_init_compression(2);
  fades = calloc(1, sizeof(*fades));
  if (!fades) {
    lv_fail(values, "sound fade state");
    goto failed;
  }
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
    mixer_ch_set_vol(ch * 2, 0.7f, 0.7f);
    fades->gain[ch] = -1.0f;
  }
  audio_live = true;
#if DG_D10
  // Allocate every mixer channel's sample ring while the heap is empty.
  // The rings otherwise appear at each channel's first play — the castle
  // map's narration was the first use of the speech channels, and rings
  // allocated at the top of a full arena pin malloc's break there, capping
  // the top-of-RAM scratch region every later cast-overlay load needs for
  // its link temporary.
  {
    FILE *warm = fopen("rom:/sfx/silence.wav64", "rb");
    if (warm) {
      fclose(warm);
      wav64_t silence;
      wav64_open(&silence, "rom:/sfx/silence.wav64");
      for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
        wav64_play(&silence, ch * 2);
        mixer_ch_stop(ch * 2);
      }
      rspq_wait();
      wav64_close(&silence);
    }
  }
#endif
  font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
  rdpq_font_style(font, 1, &(rdpq_fontstyle_t){.color = RGBA32(0, 0, 0, 255)});
  rdpq_text_register_font(1, font);
  dg_init(director, values,
          (dg_platform_t){.nth_file = game_nth_file,
                          .long_date = game_long_date,
                          .read_file = read_file,
                          .write_file = write_file,
                          .sound = sound,
                          .sound_busy = sound_busy,
#if DG_EXTENDED
                          .sound_position = sound_position,
#endif
                          .trace = trace,
                          .hit = hit,
#if DG_D10
                          .play_file = play_file,
                          .find_movie = find_movie,
                          .read_data = read_rom_data,
#endif
                          },
          NULL, global_names, sizeof(global_names) / sizeof(*global_names),
          0x46494e44);
  values->clock_us = get_ticks_us;
#ifdef DIRECTOR64_PRINTING
  game_print_init(values);
#endif
  flashram_info_t flash;
  if (!flashram_init(NULL, &flash) || flash.total_size != 128 * 1024) {
    lv_fail(values, "FlashRAM unavailable; existing saves were not changed");
    goto failed;
  }
  game_save_load((save_backend_t){flash_read, flash_write, NULL});
  debugf("DIRECTOR64 NATIVE_SAVE_LOADED status=%u generation=%lu migrated=%u\n",
         game_save_status(), (unsigned long)game_save_generation(),
         game_save_migrated());
  if (game_save_status() != SAVE_VALID && game_save_status() != SAVE_BLANK) {
    lv_fail(values, "Save data unreadable; existing saves were not changed");
    goto failed;
  }
  const char *initial_movie = DIRECTOR64_ENTRY_MOVIE;
#ifdef DIRECTOR64_FULL_REPLAY
  initial_movie = director_replay_init(values);
#endif
  if (!enter(initial_movie, 1))
    goto failed;
  pointer_init(&pointer);
  unsigned long paused_steps = 0;
  uint64_t last = get_ticks_us(), accumulator = 0;
  bool ready = false;
  while (!values->failed && !director->quit) {
#if DG_D5
    if(director->window_cleanup && !values->depth) {
      d5_video_close();
      while(overlay_count>window_overlay_base)dlclose(overlays[--overlay_count]);
      director->window_cleanup=false;
    }
#endif
    uint64_t now = get_ticks_us();
    unsigned steps;
    if (!dg_clock_advance(&accumulator, now - last, &steps)) {
      lv_fail(values, "service clock overflow");
      break;
    }
    last = now;
    if (accumulator >= 6000000) {
      debugf("DIRECTOR64 NATIVE_TIMING_OVERRUN us=%llu\n",
             (unsigned long long)(accumulator / 60));
    }
    // A movie load or a sustained overload leaves owed service time. The
    // original never repaid loading time, and bursting minutes of backlog at
    // four ticks per frame would rush animation and audio cues far from the
    // displayed scene; beyond a short catch-up window the game simply runs
    // late. Twelve ticks ride out one slow frame without losing cadence.
    if (accumulator > 12000000) {
      dropped_us += (accumulator - 12000000) / 60;
      accumulator = 12000000;
    }
    for (unsigned step = 0; step < steps && !values->failed; step++) {
#if DG_D10
      // Focused-window boot: a pending controller-window movie enters the
      // single context; its code stays resident through dg_window_loaded.
      if (director->window_movie_name[0] && !director->window_movie &&
          !director->next_movie[0] && !values->depth)
        snprintf(director->next_movie, sizeof(director->next_movie), "%.27s.DXR",
                 director->window_movie_name);
#endif
      if (
#ifdef DIRECTOR64_PRINTING
          !game_print_paused() &&
#endif
          dg_transition_ready(director)) {
        char movie[32];
        snprintf(movie, sizeof(movie), "%s", director->next_movie);
        if (!enter(movie, director->next_movie_frame))
          break;
#if DG_D10
        dg_window_loaded(director);
#endif
      }
      input_sample_t sample = platform_poll_input();
#ifdef DIRECTOR64_FULL_REPLAY
      director_replay_sample(&sample, director);
#endif
      // Cursor motion and mouse ownership count against the guest budget.
      uint32_t started = TICKS_READ();
#if DG_D8
      if (!text_input_active())
        mucklas_pad_keys(director,&sample);
#endif
#ifdef DIRECTOR64_WILLY
      bool was_notice = *director->notice != 0;
#endif
      bool run;
#if defined(DIRECTOR64_PRINTING) || DG_EXTENDED || DG_D7_UP
      // A dialogue, keyboard or notice owns the whole screen, so it answers to
      // the player who opened it rather than to every controller at once.
      pointer_player_t *driver = pointer_active(&pointer);
      input_sample_t owner = input_sample_for(&sample, pointer.active);
#endif
#ifdef DIRECTOR64_PRINTING
      bool was_printing = game_print_paused();
      unsigned print_before = game_print_id();
      if (game_print_input(&owner)) {
        driver->input.held = owner.buttons;
        driver->input.pressed = driver->input.released = 0;
        run = false;
      } else
#endif
      {
#if DG_CAP_TEXT_INPUT
      bool was_typing = text_input_active();
      if(was_typing) {
        text_input_update(director,&owner);
#if DG_CAP_KEYBOARD
        driver->input.held=0;
        if (!text_input_active()) {
          director->await_release=true;
          driver->input.held=owner.buttons;
        }
#else
        driver->input.held=owner.buttons;
#endif
        driver->input.pressed=driver->input.released=0;
#if DG_CAP_KEYBOARD
        run=true;
#else
        run=false;
#endif
      } else {
#ifdef DIRECTOR64_WILLY
        if (!willy_input(director, &owner)) {
          driver->input.held = owner.buttons;
          driver->input.pressed = driver->input.released = 0;
          run = false;
        } else
#endif
        {
        pointer_update(&pointer,director,&sample);
        run=true;
        driver = pointer_active(&pointer);
        if (driver->input.pressed & INPUT_A) {
          int hit_x = driver->input.x / INPUT_ONE;
          int hit_y = driver->input.y / INPUT_ONE;
#if DG_D10
          // The editable-field hit test works in the authored 800x600 space.
          hit_x = hit_x * 5 / 4;
          hit_y = hit_y * 5 / 4;
#endif
          // Seed the keyboard with the buttons of whoever just clicked, so the
          // press that opened it is not read again as a key.
          if (text_input_open(director, dg_mouse_hit(director, hit_x, hit_y),
                              driver->input.held))
            run = false;
        }
        }
      }
#if DG_EXTENDED
      if (was_typing != text_input_active()) {
        accumulator = 0;
        steps = step + 1;
      }
#endif
#else
      pointer_update(&pointer, director, &sample);
      run = true;
#endif
      }
      pointer_us += TICKS_TO_US((uint64_t)TICKS_SINCE(started));
      // Service steps the loop consumed without ticking the score, because the
      // port was deliberately holding it: a modal notice, the controller
      // keyboard, a Willy input mode. Guest wall time passes for these and the
      // score's clock does not, so a window containing them reads exactly like
      // one the console was too slow for unless they are counted.
      if (!run)
        paused_steps++;
      if (run) {
        // Director has one mouse; the player holding it is the one it follows.
        const pointer_player_t *mouse = pointer_driver(&pointer);
        int mouse_x = mouse->input.x / INPUT_ONE;
        int mouse_y = mouse->input.y / INPUT_ONE;
#if DG_D10
        // The physical 640x480 pointer maps into the authored 800x600 stage.
        mouse_x = mouse_x * 5 / 4;
        mouse_y = mouse_y * 5 / 4;
#endif
        dg_tick(director, mouse_x, mouse_y, !!(mouse->input.held & INPUT_A),
                DG_SERVICE_BUDGET);
      }
#ifdef DIRECTOR64_WILLY
      if (was_notice != (*director->notice != 0)) {
        accumulator = 0;
        steps = step + 1;
      }
#endif
#ifdef DIRECTOR64_PRINTING
      if (print_before != game_print_id()) {
        const print_document_t *doc = game_print_document();
        debugf("DIRECTOR64 PRINT_%s id=%u tick=%lu url=%s\n",
               doc ? "OPEN" : "CLOSE", doc ? game_print_id() : print_before,
               (unsigned long)director->ticks, doc ? doc->url : "");
      }
      if (was_printing != game_print_paused()) {
        accumulator = 0;
        steps = step + 1;
      }
#endif
      tick_us += TICKS_TO_US((uint64_t)TICKS_SINCE(started));
#if DG_MODERN
      observe_source_state();
#endif
      if (run && director->ticks % 300 == 0) {
        heap_stats_t stats;
        sys_get_heap_stats(&stats);
        // object_count is maintained for every profile; reporting it only
        // above D6 left the handle-growth check blind on the three ports
        // that have no emergency collector to survive exhausting them.
        unsigned live_objects = values->object_count;
        debugf("DIRECTOR64 NATIVE_TICK movie=%s frame=%u tick=%lu free=%d "
               "cache=%u heap=%lu depth=%u objects=%u high=%lu frag=%d "
               "paused=%lu\n",
               director->movie->code->name, director->frame,
               (unsigned long)director->ticks, stats.free, cache_bytes,
               (unsigned long)values->heap_used, values->depth, live_objects,
               (unsigned long)values->heap_high_water, stats.fragmented,
               paused_steps);
        debugf("DIRECTOR64 NATIVE_COST ticks_us=%llu render_us=%llu "
               "audio_us=%llu renders=%u steps=%lu gc=%lu egc=%lu "
               "gc_us=%llu dropped_us=%llu wall=%llu image_us=%llu text_us=%llu "
               "flash_us=%llu order_us=%llu\n",
               (unsigned long long)tick_us, (unsigned long long)render_us,
               (unsigned long long)audio_us, render_calls,
               (unsigned long)values->steps,
               (unsigned long)values->collect_passes,
               (unsigned long)values->emergency_passes,
               (unsigned long long)values->collect_us,
               (unsigned long long)dropped_us,
               (unsigned long long)get_ticks_us(),
               (unsigned long long)phase_image_us,
               (unsigned long long)phase_text_us,
               (unsigned long long)phase_flash_us,
               (unsigned long long)phase_order_us);
        debugf("DIRECTOR64 NATIVE_IMAGE_COST scan_us=%llu open_us=%llu "
               "headroom_us=%llu read_us=%llu ink_us=%llu mask_us=%llu "
               "heap_us=%llu heap_calls=%u\n",
               (unsigned long long)image_scan_us,
               (unsigned long long)image_open_us,
               (unsigned long long)image_headroom_us,
               (unsigned long long)image_read_us,
               (unsigned long long)image_ink_us,
               (unsigned long long)image_mask_us,
               (unsigned long long)heap_stats_us, heap_stats_calls);
        debugf("DIRECTOR64 NATIVE_ENTER_COST load_us=%llu script_us=%llu "
               "overlay_us=%llu overlays=%u probe_us=%llu probes=%u "
               "drain_us=%llu plane_us=%llu\n",
               (unsigned long long)enter_load_us,
               (unsigned long long)enter_script_us,
               (unsigned long long)enter_overlay_us, enter_overlays,
               (unsigned long long)largest_block_us, largest_block_calls,
               (unsigned long long)enter_drain_us,
               (unsigned long long)enter_plane_us);
        largest_block_us = 0;
        largest_block_calls = 0;
        image_scan_us = image_open_us = image_headroom_us = 0;
        image_read_us = image_ink_us = heap_stats_us = 0;
        image_mask_us = 0;
        heap_stats_calls = 0;
        enter_load_us = enter_script_us = enter_overlay_us = 0;
        enter_drain_us = enter_plane_us = 0;
        enter_overlays = 0;
        debugf("DIRECTOR64 NATIVE_POINTER_COST us=%llu\n", (unsigned long long)pointer_us);
        pointer_us = 0;
        // Real-time playback positions anchor A/V drift measurement against
        // the virtual tick the report carries.
        for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++)
          if (mixer_ch_playing(ch * 2)) {
            waveform_t *wave = mixer_ch_playing_waveform(ch * 2);
            debugf("DIRECTOR64 NATIVE_AUDIO_POS channel=%u pos=%llu rate=%lu\n",
                   ch, (unsigned long long)mixer_ch_get_pos(ch * 2),
                   (unsigned long)(wave ? wave->frequency : 0));
          }
#if DG_CAP_CACHE_STATS
        if (stats.free < cache_min_free) cache_min_free = stats.free;
        debugf("DIRECTOR64 NATIVE_CACHE loads=%u load_bytes=%u evictions=%u "
               "allocation_retries=%u peak_bytes=%u largest_load=%u min_free=%d "
               "bitmap_block_bytes=%u bundle_rows=%u bundle_resident=%u "
               "bundle_bytes=%u bundle_us=%llu region_us=%llu region_compactions=%u "
               "compact_us=%llu compact_bytes=%u region_windows=%u\n",
               cache_loads, cache_load_bytes, cache_evictions, cache_retries,
               cache_peak, cache_load_peak, cache_min_free, bitmap_block_bytes,
               bundle_count, bundle_resident, bundle_bytes,
               (unsigned long long)bundle_us, (unsigned long long)image_region_us,
               image_region_compactions, (unsigned long long)image_region_compact_us,
               image_region_moved, image_region_windows);
        cache_loads = cache_load_bytes = cache_evictions = cache_retries = 0;
        bundle_us = image_region_us = 0;
        image_region_compactions = 0;
        image_region_compact_us = 0;
        image_region_moved = image_region_windows = 0;
        cache_peak = cache_bytes;
        cache_load_peak = 0;
        cache_min_free = stats.free;
#endif
        tick_us = render_us = audio_us = 0;
        phase_image_us = phase_text_us = phase_flash_us = phase_order_us = 0;
        render_calls = 0;
      }
    }
    float volume = director->sound_level <= 0   ? 0.0f
                   : director->sound_level >= 7 ? 1.0f
                                                : director->sound_level / 7.0f;
    // A channel's gain is handed to the mixer only when it changes. This
    // loop runs many times per service tick, and the emulator's cache-miss
    // profile put a seventh of all data misses in mixer_ch_set_vol: the
    // mixer's channel records alias the interpreter's hot lines in the
    // direct-mapped cache, so each pass refetched them for nothing.
    static float served_gain[DG_SOUND_CHANNELS] = {-1.0f};
    for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
      float gain = volume;
#if DG_MODERN
      gain *= director->channel_volume[ch] / 255.0f;
#endif
      // A fading channel keeps its ramp; the gain applies once it settles.
      if (gain != served_gain[ch] && !(fading & (1u << ch))) {
        served_gain[ch] = gain;
        fades->gain[ch] = gain;
        mixer_ch_set_vol(ch * 2, gain, gain);
      }
    }
    uint32_t started = TICKS_READ();
#if DG_D5
    // Presentation times advance in d5_media_tick, so between ticks this can
    // only rewalk every channel to decide nothing. The audio pump below still
    // runs every pass.
    static float served_volume = -1.0f;
    if (steps || volume != served_volume) {
      served_volume = volume;
      d5_video_update(director, volume);
    }
#endif
    service_audio(now);
    audio_us += TICKS_TO_US((uint64_t)TICKS_SINCE(started));
    started = TICKS_READ();
    // A cached scene cannot change between service ticks. Retry a pending
    // visual update if a display surface was busy, without rescanning an
    // unchanged stage on every idle iteration of the audio-service loop.
    bool rendered = (steps || !frame_cached || director->stage_count ||
                     cached_revision != director->visual_revision ||
                     cached_externals != render_externals()) &&
                    render();
    render_us += TICKS_TO_US((uint64_t)TICKS_SINCE(started));
    if (rendered) {
      render_calls++;
    }
    // A stage that has not changed for a second collects the garbage its
    // scene left behind: the pause lands where nothing moves, rather than
    // in the middle of play when allocation reaches a trigger, and live
    // memory is what the pacing census reports afterwards. Only between
    // service steps, with no handler running, and once per batch of
    // allocations.
    static uint64_t still_since_us;
    if (rendered || !still_since_us) {
      still_since_us = now;
    } else if (now - still_since_us >= 1000000 && !values->depth &&
               values->allocations_since_gc >= 256) {
      lv_collect(values);
      still_since_us = now;
    }
    if (completed_serial && !ready) {
      debugf("DIRECTOR64 NATIVE_RENDER_READY\n");
      ready = true;
    }
#if DG_D5 || DG_EXTENDED
    if(completed_serial>=scene_first_serial &&
#if DG_EXTENDED
       completed_frame && reported_frame != completed_frame
#else
       !scene_ready
#endif
    ) {
      debugf("DIRECTOR64 SCENE_RENDERED name=%s frame=%u\n",director->movie->code->name,
#if DG_EXTENDED
             completed_frame);
      reported_frame = completed_frame;
#else
             director->frame);
#endif
      scene_ready=true;
    }
#endif
    service_audio(get_ticks_us());
#if DG_EXTENDED
    // Nothing is owed this pass, so spend it on a shared cast the first scene
    // that wants one would otherwise expand inside its entry stall.
    if (!steps && ready && !director->next_movie[0] && !values->depth)
      preload_shared_cast();
#endif
    // A pass that owed no service tick and drew nothing has nothing to do
    // until the next tick is due. Spinning through the loop meanwhile cost
    // more than it looked: each pass touched a few lines of loop state that
    // alias the interpreter's working set in the direct-mapped cache, so
    // hundreds of idle passes per tick evicted it before every tick (half
    // of all data misses in the emulator's profile). Waiting on the counter
    // touches no memory. The wait is capped so the audio pump above keeps
    // its cadence and a display job still completes promptly.
    if (!steps && !rendered) {
      uint64_t owed = accumulator < 1000000 ? (1000000 - accumulator) / 60 : 0;
      if (owed > 2000) owed = 2000;
      if (owed) wait_ticks(TICKS_FROM_US(owed));
    }
  }
failed:
  debugf("DIRECTOR64 NATIVE_%s %s\n", values->failed ? "FAIL" : "QUIT",
         values->error);
  for (unsigned i = 0; i < DG_SOUND_CHANNELS; i++)
    sound(NULL, i, NULL);
  rspq_wait();
#if DG_D5
  d5_video_close();
#endif
  mixer_close();
  audio_close();
  notice(values->failed ? "Director64 stopped safely" : "Thanks for playing!",
         values->failed ? values->error : "The game has ended.");
  platform_shutdown();
  return values->failed ? 1 : 0;
}
