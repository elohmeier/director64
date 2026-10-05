// The browser backend's runtime host: one shared engine instance, the
// platform services it calls, the save device and the service step. The page
// (site/app.js) drives it frame by frame; the Node probe harness drives the
// same functions through the line protocol the native probes speak, so parity
// against the native probe exercises exactly what the page runs.
#include "web_runtime.h"
#include "compositor.h"
#include "game.h"
#include "pointer.h"
#include "web_profile.h" // generated: entry movie and the game's save files
#ifdef DIRECTOR64_WILLY
#include "willy_input.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#ifdef DIRECTOR64_PRINTING
#include "printing.h"
#endif
#ifdef DIRECTOR64_PACKAGE
// The game arrives as a package (docs/package-format.md); this build links
// no game data, only the engine and the game profile's adapter.
#include "package.h"
static const uint8_t runtime_abi[DG_PACKAGE_ABI_BYTES] = DIRECTOR64_ABI;
static dg_package_t package;
static char package_error[256];
static const dg_movie_t *const *dg_registry;
static unsigned dg_registry_count;
static const char *const *global_names;
static unsigned global_count;
#else
extern const dg_movie_t *const dg_registry[];
extern const unsigned dg_registry_count;
static const char *const global_names[] = {
#include "globals.inc"
};
static const unsigned global_count = sizeof(global_names) / sizeof(*global_names);
#endif
#ifndef DIRECTOR64_GAME_PROBE
// The default state line reports the save archive's files; a game's own
// probe state (games/<slug>/probe) reports what its native probe does.
static const char *const save_files[] = {DIRECTOR64_SAVE_FILES};
enum { SAVE_FILE_COUNT = sizeof(save_files) / sizeof(*save_files) };
#endif

static lv_runtime_t values;
static dg_runtime_t director;
static pointer_control_t pointer;
static wc_t compositor;
static web_host_t host;
static bool booted;
static unsigned seed;
// Sounds started since the module loaded; like the native probe's, it
// survives a reboot.
static unsigned sound_count;
static unsigned busy_until[DG_SOUND_CHANNELS];
#if DG_EXTENDED
static unsigned sound_started[DG_SOUND_CHANNELS];
#endif
static float served_gain[DG_SOUND_CHANNELS];
// The console's FlashRAM, erased. The page fills it from durable storage
// before boot and persists it after every committed save.
static uint8_t flash[WEB_FLASH_BYTES];
static bool flash_dirty;
static char output[1 << 20];
static size_t output_length;

static void emit(const char *format, ...) {
  if (output_length >= sizeof(output) - 1)
    return;
  va_list args;
  va_start(args, format);
  int n = vsnprintf(output + output_length, sizeof(output) - output_length,
                    format, args);
  va_end(args);
  if (n > 0)
    output_length += (size_t)n < sizeof(output) - output_length
                         ? (size_t)n
                         : sizeof(output) - output_length - 1;
}

static bool flash_read(void *ctx, size_t offset, void *data, size_t length) {
  (void)ctx;
  if (offset > sizeof(flash) || length > sizeof(flash) - offset)
    return false;
  memcpy(data, flash + offset, length);
  return true;
}
static bool flash_write(void *ctx, size_t offset, const void *data,
                        size_t length) {
  (void)ctx;
  if (offset > sizeof(flash) || length > sizeof(flash) - offset)
    return false;
  memcpy(flash + offset, data, length);
  flash_dirty = true;
  return true;
}
static bool read_file(void *ctx, const char *name, char *out, unsigned cap,
                      unsigned *length) {
  (void)ctx;
  return game_read_file(name, out, cap, length);
}
static bool write_file(void *ctx, const char *name, const char *data,
                       unsigned length) {
  (void)ctx;
  bool ok = game_write_file(name, data, length);
  // The archive has committed a generation to the flash image; the page makes
  // it durable and reports it only once its own write has succeeded.
  if (flash_dirty && host.save_commit)
    host.save_commit(host.ctx, ok);
  flash_dirty = false;
  return ok;
}
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
  busy_until[channel] =
      director.ticks +
      (m && m->rate ? (m->samples * 60u + m->rate - 1) / m->rate : 0);
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
  return channel < DG_SOUND_CHANNELS && director.ticks < busy_until[channel];
}
#if DG_EXTENDED
// How far a channel's sound has played, in milliseconds, for cue points. Like
// channel lifetime it follows the service clock, as on the native probe.
static unsigned sound_position(void *ctx, unsigned channel) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS || director.ticks <= sound_started[channel])
    return 0;
  return (director.ticks - sound_started[channel]) * 1000u / 60u;
}
#endif
static void trace(void *ctx, const char *text) {
  (void)ctx;
  if (host.trace)
    host.trace(host.ctx, text);
}
#if DG_D10
// A playFile stream holds its channel for as long as it plays: the length
// the host answers (the native probe's is a nominal second).
static void play_file(void *ctx, unsigned channel, const char *name) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS)
    return;
  unsigned milliseconds = host.stream ? host.stream(host.ctx, channel, name) : 0;
  busy_until[channel] = director.ticks + (milliseconds * 60u + 999u) / 1000u;
  sound_started[channel] = director.ticks;
  sound_count++;
}
// Shipped read-only data (the exercise task databases), by the runtime's
// virtual "folder/path"; a file larger than the buffer reads as incomplete.
static bool read_data(void *ctx, const char *name, char *out, unsigned cap,
                      unsigned *length) {
  (void)ctx;
  int32_t size = host.data ? host.data(host.ctx, name, NULL, 0) : -1;
  if (size < 0)
    return false;
  if ((uint32_t)size > cap) {
    *length = 0;
    return false;
  }
  *length = (unsigned)host.data(host.ctx, name, (uint8_t *)out, cap);
  return true;
}
// A registered movie by authored stem (case-insensitive, extension ignored)
// or, with a NULL stem, by scene file id; every movie stays resident here.
static const dg_movie_t *find_movie(void *ctx, const char *stem, unsigned file) {
  (void)ctx;
  for (unsigned i = 0; i < dg_registry_count; i++) {
    const dg_movie_t *m = dg_registry[i];
    if (!stem) {
      if (m->id == file)
        return m;
      continue;
    }
    const char *n = m->code->name;
    unsigned k = 0;
    for (; stem[k] && n[k] && n[k] != '.'; k++) {
      char a = stem[k], b = n[k];
      if (a >= 'a' && a <= 'z') a -= 32;
      if (b >= 'a' && b <= 'z') b -= 32;
      if (a != b)
        break;
    }
    if (!stem[k] && (!n[k] || n[k] == '.'))
      return m;
  }
  return NULL;
}
#endif
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx;
  return wc_hit(&compositor, &values, m, ink, x, y);
}
static uint32_t read_asset(void *ctx, const char *name, uint8_t *out,
                           uint32_t cap) {
  (void)ctx;
  return host.asset ? host.asset(host.ctx, name, out, cap) : 0;
}
static bool video_frame(void *ctx, unsigned sprite, uint32_t *out, uint32_t cap,
                        unsigned *width, unsigned *height) {
  (void)ctx;
  return host.video_frame &&
         host.video_frame(host.ctx, sprite, out, cap, width, height);
}
static bool rasterize(void *ctx, const char *text, int width, int height,
                      const wc_text_style_t *style, uint8_t *coverage) {
  (void)ctx;
  return host.text && host.text(host.ctx, text, width, height, style, coverage);
}

static const dg_movie_t *find(const char *name) {
  char normalized[64];
  snprintf(normalized, sizeof(normalized), "%s", name);
  for (char *p = normalized; *p; p++)
    if (*p >= 'a' && *p <= 'z')
      *p -= 32;
  if (!strchr(normalized, '.') && strlen(normalized) + 4 < sizeof(normalized))
    strcat(normalized, ".DXR");
  for (unsigned i = 0; i < dg_registry_count; i++)
    if (!strcmp(dg_registry[i]->code->name, normalized))
      return dg_registry[i];
  return NULL;
}
static bool enter(const dg_movie_t *movie, unsigned frame) {
  const dg_movie_t *shared[DG_FILES];
  unsigned count = 0;
  if (!movie) {
    lv_fail(&values, "missing movie");
    return false;
  }
  for (unsigned i = 0; i < movie->cast_count; i++)
    if (movie->casts[i].file != movie->id)
      for (unsigned j = 0; j < dg_registry_count; j++)
        if (dg_registry[j]->id == movie->casts[i].file) {
          bool added = false;
          for (unsigned k = 0; k < count; k++)
            if (shared[k] == dg_registry[j])
              added = true;
          if (!added && count < DG_FILES)
            shared[count++] = dg_registry[j];
        }
  emit("ENTER %s frame=%u tick=%u\n", movie->code->name, frame, director.ticks);
  if (host.trace) {
    char line[96];
    snprintf(line, sizeof(line), "ENTER %s frame=%u tick=%u",
             movie->code->name, frame, director.ticks);
    host.trace(host.ctx, line);
  }
  return dg_enter(&director, movie, frame, shared, count);
}

void web_runtime_host(web_host_t h) { host = h; }

EMSCRIPTEN_KEEPALIVE uint8_t *d64_flash(void) { return flash; }
EMSCRIPTEN_KEEPALIVE unsigned d64_flash_bytes(void) { return sizeof(flash); }

#ifdef DIRECTOR64_PACKAGE
// Loads the game package the runtime then boots. Returns false with
// d64_package_error() naming the check that rejected it.
EMSCRIPTEN_KEEPALIVE int d64_load_package(const uint8_t *data, size_t length) {
  if (booted)
    return 0; // the loaded movies back the running engine
  dg_package_free(&package);
  dg_registry = NULL;
  dg_registry_count = global_count = 0;
  if (!dg_package_load(&package, data, length, runtime_abi, package_error,
                       sizeof(package_error)))
    return 0;
  dg_registry = package.registry;
  dg_registry_count = package.movie_count;
  global_names = package.globals;
  global_count = package.global_count;
  lv_use_symbols(package.symbols);
  return 1;
}
EMSCRIPTEN_KEEPALIVE const char *d64_package_error(void) { return package_error; }
#endif

#if DG_D5
// Digital video players, one per sprite channel showing a video member, run
// by the page (d5_video.c runs the console's): opened when a sprite shows a
// video, told its time every step, closed when the sprite moves on.
static uint32_t video_member[DG_SPRITES];
static void close_videos(void) {
  for (unsigned i = 0; i < DG_SPRITES; i++)
    if (video_member[i] && host.video)
      host.video(host.ctx, i, NULL, "", 0, 0, 0, 0);
  memset(video_member, 0, sizeof(video_member));
}
static void update_videos(void) {
  if (!host.video)
    return;
  float volume = director.sound_level <= 0   ? 0.0f
                 : director.sound_level >= 7 ? 1.0f
                                             : director.sound_level / 7.0f;
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    const dg_sprite_t *s = &director.sprites[i];
    const dg_member_t *m = s->value.type ? dg_member(&director, s->value.member) : NULL;
    if (!m || m->type != 10) {
      if (video_member[i])
        host.video(host.ctx, i, NULL, "", 0, 0, 0, 0);
      video_member[i] = 0;
      continue;
    }
    if (video_member[i] && video_member[i] != m->id)
      host.video(host.ctx, i, NULL, "", 0, 0, 0, 0);
    video_member[i] = m->id;
    const char *video = m->asset && !(m->video_flags & 512) ? m->asset : "";
    const char *audio = m->video_audio && (m->video_flags & 8) ? m->video_audio : "";
    host.video(host.ctx, i, video, audio, s->video_time, s->video_rate, s->video_serial,
               volume * s->video_volume / 255.0f);
  }
}
#endif
// A console power-on: volatile state resets, only the flash image persists.
// Returns the save status; anything but valid or blank has already failed the
// runtime without touching the flash image.
EMSCRIPTEN_KEEPALIVE int d64_boot(unsigned random_seed) {
#ifdef DIRECTOR64_PACKAGE
  if (!dg_registry_count) {
    lv_fail(&values, "no game package loaded");
    return -1;
  }
#endif
  if (booted)
    wc_release(&compositor);
#if DG_D5
  // A reboot with the source dialog open releases the suspended stage.
  if (director.suspended_stage) {
    free(director.suspended_stage);
    director.suspended_stage = NULL;
  }
  close_videos();
#endif
  booted = true;
  seed = random_seed;
  memset(busy_until, 0, sizeof(busy_until));
#if DG_EXTENDED
  memset(sound_started, 0, sizeof(sound_started));
#endif
  for (unsigned i = 0; i < DG_SOUND_CHANNELS; i++)
    served_gain[i] = -1.0f;
  wc_init(&compositor,
          (wc_host_t){.read = read_asset, .text = rasterize, .video_frame = video_frame},
          (size_t)WEB_IMAGE_BUDGET);
  dg_init(&director, &values,
          (dg_platform_t){.nth_file = game_nth_file, .long_date = game_long_date,
                          .read_file = read_file, .write_file = write_file,
                          .sound = sound, .sound_busy = sound_busy,
#if DG_EXTENDED
                          .sound_position = sound_position,
#endif
#if DG_D10
                          .play_file = play_file, .find_movie = find_movie,
                          .read_data = read_data,
#endif
                          .trace = trace, .hit = hit},
          NULL, global_names, global_count, random_seed);
  pointer_init(&pointer);
#ifdef DIRECTOR64_PRINTING
  game_print_init(&values);
#endif
  save_status_t status = game_save_load((save_backend_t){flash_read, flash_write, NULL});
  if (status != SAVE_VALID && status != SAVE_BLANK) {
    lv_fail(&values, "Save data unreadable; existing saves were not changed");
    return (int)status;
  }
  enter(find(DIRECTOR64_ENTRY_MOVIE), 1);
  return (int)status;
}

#ifdef DIRECTOR64_GAME_PROBE
// The game's own service step (games/<slug>/probe/step.inc), shared with its
// native probe so both run the same per-game input layer before the tick.
// The console's working-set recorder has no part in the browser.
#define draw_record_tick(d) ((void)(d))
// Nor are the native probes' host CPU-time accounts (probe_stats.inc).
#define cpu_us() ((uint64_t)0)
#define perf_account(movie, tick, stage) ((void)(movie), (void)(tick), (void)(stage))
#include "step.inc"
#else
static bool step(int x, int y, unsigned buttons) {
  if (dg_transition_ready(&director)) {
    char movie[32];
    snprintf(movie, sizeof(movie), "%s", director.next_movie);
    unsigned frame = director.next_movie_frame;
    if (!enter(find(movie), frame ? frame : 1))
      return false;
  }
  bool ok = dg_tick(&director, x, y, (buttons & INPUT_A) != 0, DG_SERVICE_BUDGET);
  dg_update_stage(&director);
  return ok && !values.failed;
}
#endif
static void serve_gain(void) {
  if (!host.gain)
    return;
  float volume = director.sound_level <= 0   ? 0.0f
                 : director.sound_level >= 7 ? 1.0f
                                             : director.sound_level / 7.0f;
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
    float gain = volume;
#if DG_MODERN
    gain *= director.channel_volume[ch] / 255.0f;
#endif
    if (gain != served_gain[ch]) {
      served_gain[ch] = gain;
      host.gain(host.ctx, ch, gain);
    }
  }
}

// One 60 Hz service tick with the page's mouse; `buttons` are input.h bits
// (INPUT_A for the left button, INPUT_B for the right). Returns 0 while
// running, 1 once the runtime has failed and 2 once the movie has quit.
EMSCRIPTEN_KEEPALIVE int d64_step(int x, int y, int buttons) {
  if (!booted || values.failed)
    return 1;
  if (director.quit)
    return 2;
#if DG_D10
  // The page's 640x480 pointer maps into the authored 800x600 stage, as the
  // console's does; the probe protocol speaks authored coordinates.
  x = x * 5 / 4;
  y = y * 5 / 4;
#endif
  step(x, y, (unsigned)buttons);
  serve_gain();
#if DG_D5
  update_videos();
#endif
  return values.failed ? 1 : director.quit ? 2 : 0;
}
// Text entry. The console opens its on-screen keyboard when a press lands on
// an editable field (platforms/n64/text_input.c); the page opens a text box
// over the field instead and pauses the game while it is open, as the console
// does. The same checks decide when a field accepts entry. The field's stage
// rectangle is filled in for the page to place the box.
static int text_field_rect[4];
EMSCRIPTEN_KEEPALIVE unsigned d64_text_field(int x, int y) {
#if DG_EXTENDED
  if (!booted || values.failed || values.depth || director.text_pending)
    return 0;
#if DG_D10
  // The editable-field hit test works in the authored 800x600 space.
  unsigned sprite = dg_mouse_hit(&director, x * 5 / 4, y * 5 / 4);
#else
  unsigned sprite = dg_mouse_hit(&director, x, y);
#endif
  if (!sprite || sprite >= DG_SPRITES || !dg_text_editable(&director, sprite))
    return 0;
  const dg_member_t *m = dg_member(&director, director.sprites[sprite].value.member);
  if (!m || (m->type != 3 && m->type != 7))
    return 0;
  lv_t text = lv_get(&values, NULL, "text", lv_make(LV_MEMBER, (int32_t)m->id));
  if (strlen(lv_cstr(&values, text)) > 20)
    return 0;
  dg_bounds(&director, sprite, &text_field_rect[0], &text_field_rect[1],
            &text_field_rect[2], &text_field_rect[3]);
#if DG_D10
  for (unsigned i = 0; i < 4; i++)
    text_field_rect[i] = text_field_rect[i] * 4 / 5;
#endif
  return sprite;
#else
  (void)x;
  (void)y;
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE const int *d64_text_field_rect(void) { return text_field_rect; }
// The field's text as the keyboard starts from: a lone space is empty.
EMSCRIPTEN_KEEPALIVE const char *d64_text_value(unsigned sprite) {
#if DG_EXTENDED
  if (booted && sprite && sprite < DG_SPRITES) {
    const dg_member_t *m = dg_member(&director, director.sprites[sprite].value.member);
    if (m) {
      const char *text = lv_cstr(&values, lv_get(&values, NULL, "text",
                                                 lv_make(LV_MEMBER, (int32_t)m->id)));
      return strcmp(text, " ") ? text : "";
    }
  }
#else
  (void)sprite;
#endif
  return "";
}
// Commits the entry the way the keyboard's Start does; 0 when the field
// refuses it (the page keeps the box open).
EMSCRIPTEN_KEEPALIVE int d64_edit_text(unsigned sprite, const char *text) {
#if DG_EXTENDED
  return booted && !values.failed && dg_edit_text(&director, sprite, text);
#else
  (void)sprite;
  (void)text;
  return 0;
#endif
}
// A key of the page's keyboard, as a desktop projector receives it: the
// Macintosh virtual key code and the character it types (dg_key).
EMSCRIPTEN_KEEPALIVE void d64_key(unsigned code, unsigned character, int down) {
#if DG_CAP_KEYBOARD
  // The page may outrun the score (a held key repeats, typing is fast);
  // what the queue cannot take is dropped rather than failing the game.
  if (booted && !values.failed && code < 128 && character <= 255 && director.key_count < 32)
    dg_key(&director, code, character, down != 0);
#else
  (void)code;
  (void)character;
  (void)down;
#endif
}
// The composited stage with the page's pointer, or NULL when nothing changed.
EMSCRIPTEN_KEEPALIVE uint32_t *d64_render(int x, int y, int pointer_shown) {
  if (!booted || values.failed)
    return NULL;
  unsigned externals = host.video_revision ? host.video_revision(host.ctx) : 0;
  return wc_render(&compositor, &director, x, y, pointer_shown != 0, externals)
             ? compositor.pixels
             : NULL;
}
// The print document the game asked for (1-16: book BAS then REZ, chapters
// 1 to 8), while its page is up; 0 otherwise. The page opens the browser's
// print dialog for it and dismisses the page as the console's B does.
EMSCRIPTEN_KEEPALIVE unsigned d64_print_request(void) {
#ifdef DIRECTOR64_PRINTING
  return booted ? game_print_id() : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t *d64_pixels(void) { return compositor.pixels; }
EMSCRIPTEN_KEEPALIVE const char *d64_error(void) {
  return values.failed ? values.error : "";
}
EMSCRIPTEN_KEEPALIVE const char *d64_movie(void) {
  return director.movie ? director.movie->code->name : "";
}
EMSCRIPTEN_KEEPALIVE unsigned d64_ticks(void) { return director.ticks; }
EMSCRIPTEN_KEEPALIVE unsigned d64_frame(void) { return director.frame; }
EMSCRIPTEN_KEEPALIVE unsigned d64_script_errors(void) {
  return values.script_error_count;
}
EMSCRIPTEN_KEEPALIVE const char *d64_last_script_error(void) {
  return values.last_script_error;
}
EMSCRIPTEN_KEEPALIVE unsigned d64_save_generation(void) {
  return game_save_generation();
}
EMSCRIPTEN_KEEPALIVE unsigned d64_image_bytes(void) {
  return (unsigned)compositor.image_bytes;
}
EMSCRIPTEN_KEEPALIVE unsigned d64_image_loads(void) { return compositor.loads; }

// ---- The native probes' line protocol (games/*/tests/director_probe.c) ----

static void quoted(const char *text) {
  emit("\"");
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
#if DG_D10
    // D10's strings mix windows-1252 bytes (numToChar-built database text)
    // with UTF-8 member text; its probe escapes every non-ASCII byte so the
    // report stays JSON either way.
    if (*p > 126) {
      emit("\\u%04x", *p);
      continue;
    }
#endif
    if (*p < 32 || *p == '"' || *p == '\\')
      emit("\\u%04x", *p);
    else
      emit("%c", *p);
  }
  emit("\"");
}
#ifdef DIRECTOR64_GAME_PROBE
// The game's own state line (games/<slug>/probe/state.inc), printed through
// this runtime's output buffer.
#define printf(...) emit(__VA_ARGS__)
#define putchar(c) emit("%c", (c))
#define puts(s) emit("%s\n", (s))
#define fflush(f) ((void)(f))
// The names the native probes' state lines use for this runtime's services.
#define busy sound_busy
#define native_cursor_bitmap(v, m, mask, out) wc_cursor_bitmap(&compositor, v, m, mask, out)
#include "state_keys.inc"
#include "state.inc"
#undef busy
#undef native_cursor_bitmap
#undef printf
#undef putchar
#undef puts
#undef fflush
#else
static void state(void) {
  static char text[16384];
  emit("{\"movie\":");
  quoted(director.movie ? director.movie->code->name : "");
  emit(",\"frame\":%u,\"tick\":%u,\"depth\":%u,\"quit\":%s,\"sounds\":%u,\"error\":",
       director.frame, director.ticks, values.depth,
       director.quit ? "true" : "false", sound_count);
  quoted(values.failed ? values.error : "");
  emit(",\"script_errors\":%u,\"last_script_error\":", values.script_error_count);
  quoted(values.last_script_error);
  emit(",\"handler\":");
  quoted(values.depth ? values.frames[values.depth - 1].handler->name : "");
  emit(",\"line\":%u,\"globals\":{",
       values.depth ? values.frames[values.depth - 1].line : 0);
  bool comma = false;
  for (unsigned i = 0; i < values.global_count; i++)
    if (lv_type(values.globals[i])) {
      if (!lv_format(&values, values.globals[i], text, sizeof(text)))
        break;
      if (comma)
        emit(",");
      comma = true;
      quoted(global_names[i]);
      emit(":");
      quoted(text);
    }
  emit("},\"sprites\":[");
  comma = false;
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    dg_sprite_t *s = &director.sprites[i];
    if (!s->value.type || !s->visible)
      continue;
    int l, t, r, b;
    dg_bounds(&director, i, &l, &t, &r, &b);
    if (comma)
      emit(",");
    comma = true;
    emit("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],"
         "\"blend\":%u,\"loc\":[%d,%d],\"moveable\":%u}",
         i, s->value.member, s->value.script, l, t, r, b, dg_opacity(s),
         s->value.x, s->value.y, s->moveable);
  }
  const pointer_player_t *driver = pointer_driver(&pointer);
  emit("],\"pointer\":{\"player\":%u,\"x\":%d,\"y\":%d},\"save_lengths\":[",
       pointer.active + 1, driver->input.x / INPUT_ONE,
       driver->input.y / INPUT_ONE);
  static char contents[SAVE_FILE_COUNT][16384];
  unsigned lengths[SAVE_FILE_COUNT];
  for (unsigned i = 0; i < SAVE_FILE_COUNT; i++) {
    lengths[i] = 0;
    if (!game_read_file(save_files[i], contents[i], sizeof(contents[i]), &lengths[i]))
      lengths[i] = 0;
    contents[i][lengths[i]] = 0;
    emit("%s%u", i ? "," : "", lengths[i]);
  }
  emit("],\"save_contents\":[");
  for (unsigned i = 0; i < SAVE_FILE_COUNT; i++) {
    if (i)
      emit(",");
    quoted(contents[i]);
  }
  emit("],\"save_generation\":%u}\n", game_save_generation());
}
#endif
static void point(unsigned sprite) {
  int l, t, r, b;
  dg_bounds(&director, sprite, &l, &t, &r, &b);
  int cx = (l + r) / 2, cy = (t + b) / 2;
  if (cx >= 8 && cx < 630 && cy >= 8 && cy < 470 &&
      dg_mouse_hit(&director, cx, cy) == sprite) {
    emit("{\"point\":[%d,%d],\"error\":\"\"}\n", cx, cy);
    return;
  }
  for (int spacing = 8; spacing >= 1; spacing /= 8)
    for (int yy = t; yy < b; yy += spacing)
      for (int xx = l; xx < r; xx += spacing)
        if (xx >= 8 && xx < 630 && yy >= 8 && yy < 470 &&
            dg_mouse_hit(&director, xx, yy) == sprite) {
          emit("{\"point\":[%d,%d],\"error\":\"\"}\n", xx, yy);
          return;
        }
  emit("{\"point\":null,\"error\":\"\"}\n");
}
// Runs one protocol command and returns everything it printed: ENTER lines
// for movies it entered, then the resulting state (or point) as one JSON line.
// An empty command boots with the given seed and reports the first state.
EMSCRIPTEN_KEEPALIVE const char *d64_rpc(const char *command) {
  output_length = 0;
  output[0] = 0;
  unsigned ticks, sprite, buttons;
  int x, y, down;
  char entered[64];
  (void)entered;
  if (!strcmp(command, "reboot")) {
    d64_boot(seed);
  } else if (sscanf(command, "pad %u %d %d %u", &ticks, &x, &y, &buttons) == 4 &&
             ticks <= 100000 && x >= -128 && x <= 127 && y >= -128 && y <= 127 &&
             buttons <= UINT16_MAX) {
    input_sample_t sample = {.connected = true, .stick_x = x, .stick_y = y,
                             .buttons = buttons};
    for (unsigned i = 0; i < ticks && !values.failed && !director.quit; i++) {
      pointer_update(&pointer, &director, &sample);
      const input_state_t *in = &pointer_driver(&pointer)->input;
      if (!step(in->x / INPUT_ONE, in->y / INPUT_ONE, !!(in->held & INPUT_A)))
        break;
    }
  } else if (sscanf(command, "point %u", &sprite) == 1 && sprite > 0 &&
             sprite < DG_SPRITES) {
    point(sprite);
    return output;
  } else if (sscanf(command, "step %u %d %d %d", &ticks, &x, &y, &down) == 4 &&
             ticks <= 100000) {
    for (unsigned i = 0; i < ticks && !values.failed && !director.quit; i++)
      if (!step(x, y, (unsigned)down))
        break;
#if DG_CAP_KEYBOARD
  } else if (sscanf(command, "key %u %d %d", &ticks, &x, &down) == 3) {
    dg_key(&director, ticks, (unsigned)x, down != 0);
#endif
#if DG_EXTENDED
  } else if (sscanf(command, "text %u %63[^\n]", &sprite, entered) == 2) {
    if (!dg_edit_text(&director, sprite, entered))
      lv_fail(&values, "invalid text entry");
#endif
  } else if (strcmp(command, "state")) {
    return NULL;
  }
  state();
  return output;
}
EMSCRIPTEN_KEEPALIVE const char *d64_boot_rpc(unsigned random_seed) {
  output_length = 0;
  output[0] = 0;
  d64_boot(random_seed);
  state();
  return output;
}
