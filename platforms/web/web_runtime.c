// The runtime host of the web player and Android TV: one shared engine
// instance, the platform services it calls, the save device and the service
// step. The page (site/player.js) and the Android host (jni/main.c) drive it
// frame by frame; the Node probe harness (node/probe.mjs) drives the same
// functions through a line protocol, so fuzzing and journeys exercise exactly
// what the players run.
#include "ink.h"
#include "web_runtime.h"
#include "compositor.h"
#include "game.h"
#include "stream_name.h"
#include "pointer.h"
#include "game_profile.h" // generated from the game's game.toml [player] table
#ifdef DIRECTOR64_WILLY
#include "willy_input.h"
#endif
#ifdef DIRECTOR64_MUCKLAS
#include "controls.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#ifdef DIRECTOR64_PRINTING
#include "printing.h"
#endif
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

static lv_runtime_t values;
static dg_runtime_t director;
static pointer_control_t pointer;
static wc_t compositor;
static web_host_t host;
static bool booted;
static bool probe_render;
EMSCRIPTEN_KEEPALIVE void d64_probe_render(int enabled) { probe_render = enabled != 0; }
static unsigned seed;
static float served_gain[DG_SOUND_CHANNELS];
// The 128 KiB save image, erased. The host fills it from durable storage
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
  if (game_read_file(name, out, cap, length)) return true;
#ifdef GAME_DATA_FOLDER
  // Installed defaults remain read-only; the save adapter takes precedence
  // after a profile has been edited. Resolve a basename in the declared
  // virtual data folder, without opening arbitrary host paths.
  const char *base = name;
  for (const char *p = name; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
  char path[256];
  int n = snprintf(path, sizeof(path), "%s/%s", GAME_DATA_FOLDER, base);
  if (!*base || n < 0 || n >= (int)sizeof(path)) return false;
  for (int i = (int)strlen(GAME_DATA_FOLDER) + 1; i < n; i++)
    path[i] = (char)toupper((unsigned char)path[i]);
  int32_t size = host.data ? host.data(host.ctx, path, NULL, 0) : -1;
  if (size < 0 || (uint32_t)size >= cap) return false;
  int32_t read = host.data(host.ctx, path, (uint8_t *)out, cap);
  if (read != size) return false;
  out[size] = 0; *length = (unsigned)size;
  return true;
#else
  return false;
#endif
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
#include "audio.inc"
static void trace(void *ctx, const char *text) {
  (void)ctx;
  if (host.trace)
    host.trace(host.ctx, text);
}
#if DG_D10
// A playFile stream holds its channel for as long as it plays: the length
// the host answers.
static void play_file(void *ctx, unsigned channel, const char *name) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS)
    return;
  sound_loops[channel].looping = false;
  sound_loops[channel].rate = 0;
  sound_paused[channel] = false;
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
static bool video_frame(void *ctx, const dg_runtime_t *owner, unsigned sprite, uint32_t *out, uint32_t cap,
                        unsigned *width, unsigned *height) {
  (void)ctx;
  sprite = dg_video_player_id(owner, sprite);
  return host.video_frame &&
         host.video_frame(host.ctx, sprite, out, cap, width, height);
}
#if DG_D10
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
// The character of a text member under (x, y) of its box, as the host sets
// the text the compositor draws: the field's live text, in the sprite's
// rectangle or the member's own size.
static int text_char_at(void *ctx, uint32_t member, unsigned sprite, int x, int y) {
  (void)ctx;
  dg_runtime_t *active = (dg_runtime_t *)values.context;
  const dg_member_t *m = dg_member(active, member);
  if (!m)
    return -1;
  const char *text = m->text;
  for (unsigned j = 0; j < active->field_count; j++)
    if (active->field_members[j] == m->id)
      text = lv_cstr(&values, values.roots[DG_FIELD_TEXT_ROOT + j]);
  int width = m->width, height = m->height;
  if (sprite && sprite < DG_SPRITES) {
    dg_sprite_t flat = active->sprites[sprite];
    flat.value.rotation = flat.value.skew = 0;
    flat.custom_quad = flat.flip_h = flat.flip_v = false;
    int l, t, r, b;
    dg_sprite_bounds(active, &flat, &l, &t, &r, &b);
    width = r - l;
    height = b - t;
  }
  return wc_text_char_at(&compositor, active, m, text, width, height, x, y);
}
#endif
static bool read_image(void *ctx, uint32_t member, dg_pixel_image_t *out) {
  (void)ctx;
  return wc_read_image(&compositor, (dg_runtime_t *)values.context, member, out);
}
static bool flash_field_hit(void *ctx, dg_runtime_t *owner,
                            struct dg_flash_instance *field, float x, float y) {
  (void)ctx;
  return wc_flash_field_hit(&compositor, owner, field, x, y);
}
static void capture_stage(void *ctx, uint32_t member) {
  (void)ctx;
  wc_capture_stage(&compositor, member);
}
#endif
static bool rasterize(void *ctx, const char *text, int width, int height,
                      const wc_text_style_t *style, uint8_t *coverage) {
  (void)ctx;
  return host.text && host.text(host.ctx, text, width, height, style, coverage);
}
static int layout(void *ctx, const char *text, int width, int height,
                  const wc_text_style_t *style, int16_t *boxes, int max_chars) {
  (void)ctx;
  return host.layout ? host.layout(host.ctx, text, width, height, style, boxes, max_chars) : 0;
}

#if DG_D10
// The folder `the moviePath` names, or NULL for the drive root "C:\": a
// port's engine that locates itself from its folder answers its own.
static const char *game_movie_path(void *ctx) {
  (void)ctx;
#ifdef GAME_MOVIE_PATH
  return GAME_MOVIE_PATH;
#elif defined(GAME_STREAM_ROOT)
#ifdef GAME_MOVIE_PATHS
  static const char *const paths[][2] = {GAME_MOVIE_PATHS};
  const char *movie = director.movie ? director.movie->code->name : "";
  for (unsigned i = 0; paths[i][0]; i++) if (!strcmp(paths[i][0], movie)) return paths[i][1];
#endif
  static const char *const folders[][2] = {GAME_MOVIE_FOLDERS};
  const char *name=director.movie ? director.movie->code->name : "";
  const char *split=strstr(name,"__");
  for (unsigned i=0;split && folders[i][0];i++)
    if ((size_t)(split-name)==strlen(folders[i][0]) && !strncmp(name,folders[i][0],(size_t)(split-name)))
      return folders[i][1];
  return GAME_STREAM_ROOT;
#else
  return NULL;
#endif
}
static const char *game_stream_name(void *ctx, const char *name) {
  (void)ctx;
#ifdef GAME_STREAM_NAMES
  static const char *const names[] = {GAME_STREAM_NAMES};
  return dg_stream_name(names, name);
#else
  return name;
#endif
}
// The shipped read-only data folder FileIO reads from, or NULL.
static const char *game_data_folder(void *ctx) {
  (void)ctx;
#ifdef GAME_DATA_FOLDER
  return GAME_DATA_FOLDER;
#else
  return NULL;
#endif
}
#endif
#if DG_D10
// The authored long name find() last resolved through its 8.3 truncation.
static char found_alias[40];
#endif
static const dg_movie_t *find(const char *name) {
  char normalized[64];
#if DG_D10
  found_alias[0] = 0;
#endif
  snprintf(normalized, sizeof(normalized), "%s", name);
  for (char *p = normalized; *p; p++)
    if (*p >= 'a' && *p <= 'z')
      *p -= 32;
  if (!strchr(normalized, '.') && strlen(normalized) + 4 < sizeof(normalized))
    strcat(normalized, ".DXR");
  // Shockwave and unprotected movies convert as .DXR.
  char *ext = strrchr(normalized, '.');
  if (ext && (!strcmp(ext, ".DCR") || !strcmp(ext, ".DIR")))
    memcpy(ext, ".DXR", 4);
  for (unsigned i = 0; i < dg_registry_count; i++)
    if (!strcmp(dg_registry[i]->code->name, normalized))
      return dg_registry[i];
  // Joliet discs name movies by their long names ("Baumhoehle"); the
  // primary volume the importer reads holds the 8.3 truncation.
  char *dot = strchr(normalized, '.');
  // The stem after a folder qualifier ("SCHWEINESPIEL__SCHWEINE") is the
  // part the disc truncated.
  char *stem = normalized;
  for (char *q = strstr(normalized, "__"); q && q < dot; q = strstr(q + 2, "__"))
    stem = q + 2;
  if (dot && dot - stem > 8) {
    memmove(stem + 8, dot, strlen(dot) + 1);
    for (unsigned i = 0; i < dg_registry_count; i++)
      if (!strcmp(dg_registry[i]->code->name, normalized)) {
#if DG_D10
        snprintf(found_alias, sizeof(found_alias), "%s%s", name,
                 strchr(name, '.') ? "" : ".dxr");
#endif
        return dg_registry[i];
      }
  }
  return NULL;
}
#if DG_D10
// A movie a folder-qualified port names by path (source.rs
// policy_source_files: FOLDER__NAME): the folder the path names, then the
// current movie's own folder, then the plain name.
static void qualifier(const char *from, size_t length, char *out, size_t size) {
  size_t n = 0;
  for (size_t i = 0; i < length && n + 1 < size; i++) {
    char c = from[i];
    c = (char)toupper((unsigned char)c);
    out[n++] = (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
  }
  out[n] = 0;
}
// The disc's 8.3 spelling of a long name: letters and digits only, eight
// at most ("Intro_Dach" is INTRODAC).
static void short_name(const char *name, char *out, size_t size) {
  const char *dot = strrchr(name, '.');
  size_t n = 0;
  for (const char *p = name; *p && p != dot && n < 8 && n + 1 < size; p++)
    if (isalnum((unsigned char)*p))
      out[n++] = *p;
  snprintf(out + n, size - n, "%s", dot ? dot : "");
}
static const dg_movie_t *find_path_stem(const char *path, const char *movie) {
  char stem[64];
  const char *base = path;
  const char *folder = NULL;
  size_t folder_length = 0;
  for (const char *p = path; *p; p++)
    if (*p == ':' || *p == '\\' || *p == '/') {
      if (p > base && !(p - base == 1 && *base == '@')) {
        folder = base;
        folder_length = (size_t)(p - base);
      }
      base = p + 1;
    }
  snprintf(stem, sizeof(stem), "%s", movie);
  char candidate[160], prefix[64];
  if (folder) {
    qualifier(folder, folder_length, prefix, sizeof(prefix));
    snprintf(candidate, sizeof(candidate), "%s__%s", prefix, stem);
    const dg_movie_t *m = find(candidate);
    if (m) {
      snprintf(found_alias, sizeof(found_alias), "%s%s", stem, strchr(stem, '.') ? "" : ".dxr");
      return m;
    }
  }
  const char *current = director.movie ? director.movie->code->name : "";
  const char *split = strstr(current, "__");
  if (split && (size_t)(split - current) < sizeof(prefix)) {
    snprintf(prefix, sizeof(prefix), "%.*s", (int)(split - current), current);
    snprintf(candidate, sizeof(candidate), "%s__%s", prefix, stem);
    const dg_movie_t *m = find(candidate);
    if (m) {
      snprintf(found_alias, sizeof(found_alias), "%s%s", stem, strchr(stem, '.') ? "" : ".dxr");
      return m;
    }
  }
  return find(stem);
}
static const dg_movie_t *find_path(const char *path, const char *movie) {
  const dg_movie_t *m = find_path_stem(path, movie);
  char short_stem[64];
  short_name(movie, short_stem, sizeof(short_stem));
  if (m || !strcmp(short_stem, movie))
    return m;
  m = find_path_stem(path, short_stem);
  if (m)
    snprintf(found_alias, sizeof(found_alias), "%s%s", movie, strchr(movie, '.') ? "" : ".dxr");
  return m;
}
#endif
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
#if DG_D10
  memcpy(director.movie_alias, found_alias, sizeof(director.movie_alias));
  found_alias[0] = 0;
#endif
  return dg_enter(&director, movie, frame, shared, count);
}

void web_runtime_host(web_host_t h) { host = h; }

EMSCRIPTEN_KEEPALIVE uint8_t *d64_flash(void) { return flash; }
EMSCRIPTEN_KEEPALIVE unsigned d64_flash_bytes(void) { return sizeof(flash); }

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

#if DG_D5 || DG_D10
#include "video.inc"
#if DG_D10
#include "flash_audio.inc"
#endif
#endif
#include "lifecycle.inc"
// A fresh boot: volatile state resets, only the save image persists.
// Returns the save status; anything but valid or blank has already failed the
// runtime without touching the flash image.
EMSCRIPTEN_KEEPALIVE int d64_boot(unsigned random_seed) {
  if (!dg_registry_count) {
    lv_fail(&values, "no game package loaded");
    return -1;
  }
  release_runtime();
  booted = true;
  seed = random_seed;
  memset(busy_until, 0, sizeof(busy_until));
  memset(sound_paused, 0, sizeof(sound_paused));
  memset(sound_loops, 0, sizeof(sound_loops));
#if DG_EXTENDED
  memset(sound_started, 0, sizeof(sound_started));
#endif
  for (unsigned i = 0; i < DG_SOUND_CHANNELS; i++)
    served_gain[i] = -1.0f;
  wc_init(&compositor,
          (wc_host_t){.read = read_asset, .text = rasterize, .layout = layout,
                      .video_frame = video_frame},
          (size_t)WEB_IMAGE_BUDGET);
  dg_init(&director, &values,
          (dg_platform_t){.nth_file = game_nth_file, .directory = game_directory, .long_date = game_long_date,
#ifdef GAME_SAVE_PATHS
                          .file_paths = true,
#endif
#if DG_D10
                          .stream_name = game_stream_name,
#endif
                          .read_file = read_file, .write_file = write_file,
                          .sound = sound, .sound_pause = sound_pause, .sound_busy = sound_busy,
                          .sound_break_loop = sound_break_loop,
#if DG_EXTENDED
                          .sound_position = sound_position,
#endif
#if DG_D10
                          .play_file = play_file, .find_movie = find_movie,
                          .movie_path = game_movie_path, .data_folder = game_data_folder,
                          .read_data = read_data, .capture_stage = capture_stage, .read_image = read_image, .flash_field_hit = flash_field_hit,
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
                          .text_char_at = text_char_at,
#endif
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
// The game's own service step (games/<slug>/probe/step.inc): its per-game
// input layer runs before the tick.
#include "step.inc"
#else
static bool step(int x, int y, unsigned buttons) {
  if (dg_transition_ready(&director)) {
    char movie[sizeof(director.next_movie)];
    snprintf(movie, sizeof(movie), "%s", director.next_movie);
    unsigned frame = director.next_movie_frame;
#if DG_D10
    const dg_movie_t *target = find_path(director.next_movie_path, movie);
    char path[96];
    snprintf(path, sizeof(path), "%s", director.next_movie_path);
    director.next_movie_path[0] = 0;
#else
    const dg_movie_t *target = find(movie);
    const char *path = "";
#endif
    if (!target && director.platform.trace) {
      char report[200];
      snprintf(report, sizeof(report), "MOVIE_MISSING %s %s", path, movie);
      director.platform.trace(director.context, report);
    }
    if (!enter(target, frame ? frame : 1))
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
  step(x, y, (unsigned)buttons);
  serve_gain();
#if DG_D5 || DG_D10
  update_videos();
#if DG_D10
  update_flash_audio();
#endif
#endif
  return values.failed ? 1 : director.quit ? 2 : 0;
}
// ---- Controllers: up to four players' gamepads ----
// The host reports every connected gamepad as a controller sample (input.h
// buttons, stick in -80..80) and then runs d64_step_pads: each player moves
// their own cursor and whoever presses or points holds Director's one mouse
// (runtime/interaction/pointer.c).
static input_sample_t pads;
EMSCRIPTEN_KEEPALIVE void d64_pad(unsigned player, int connected,
                                  unsigned buttons, int stick_x, int stick_y) {
  if (player >= INPUT_PLAYERS)
    return;
  input_pad_t pad = {.connected = connected != 0,
                     .buttons = (uint16_t)(connected ? buttons : 0),
                     .stick_x = (int8_t)(stick_x < -128 ? -128 : stick_x > 127 ? 127 : stick_x),
                     .stick_y = (int8_t)(stick_y < -128 ? -128 : stick_y > 127 ? 127 : stick_y)};
  if (player) {
    pads.pads[player - 1] = pad;
  } else {
    pads.connected = pad.connected;
    pads.buttons = pad.buttons;
    pads.stick_x = pad.stick_x;
    pads.stick_y = pad.stick_y;
  }
}
// Moves the holder's cursor to the page's mouse, so a controller picks up
// where the mouse left the pointer.
EMSCRIPTEN_KEEPALIVE void d64_pointer_warp(int x, int y) {
  input_state_t *in = &pointer_active(&pointer)->input;
  in->x = (x < 8 ? 8 : x > SCREEN_W - 10 ? SCREEN_W - 10 : x) * INPUT_ONE;
  in->y = (y < 8 ? 8 : y > SCREEN_H - 10 ? SCREEN_H - 10 : y) * INPUT_ONE;
}
EMSCRIPTEN_KEEPALIVE int d64_pointer_x(void) {
  return pointer_driver(&pointer)->input.x / INPUT_ONE;
}
EMSCRIPTEN_KEEPALIVE int d64_pointer_y(void) {
  return pointer_driver(&pointer)->input.y / INPUT_ONE;
}
EMSCRIPTEN_KEEPALIVE unsigned d64_pointer_player(void) { return pointer.active; }
EMSCRIPTEN_KEEPALIVE unsigned d64_text_field(int x, int y);
// One service tick from the controllers. Returns as d64_step does, or 3 when
// the holder pressed A on an editable field: the tick did not run, and the
// host opens its own text entry there (at d64_pointer_x/y).
EMSCRIPTEN_KEEPALIVE int d64_step_pads(void) {
  if (!booted || values.failed)
    return 1;
  if (director.quit)
    return 2;
#ifdef DIRECTOR64_MUCKLAS
  mucklas_pad_keys(&director, &pads);
#endif
  pointer_update(&pointer, &director, &pads);
  const input_state_t *in = &pointer_driver(&pointer)->input;
  int x = in->x / INPUT_ONE, y = in->y / INPUT_ONE;
  if ((in->pressed & INPUT_A) && d64_text_field(x, y))
    return 3;
  return d64_step(x, y, in->held);
}
// The stage with every controller's cursor that shows (pointer_visible), each
// in its player's colour once more than one is connected; the holder draws
// last, so a crowded screen never hides the cursor Director follows.
EMSCRIPTEN_KEEPALIVE uint32_t *d64_render_pads(void) {
  if (!booted || values.failed)
    return NULL;
  unsigned connected = 0;
  for (unsigned i = 0; i < POINTER_PLAYERS; i++)
    connected += input_sample_pad(&pads, i).connected;
  wc_pointer_t shown[WC_POINTERS];
  unsigned count = 0;
  for (unsigned n = 0; n < POINTER_PLAYERS && count < WC_POINTERS; n++) {
    unsigned player = (pointer.active + 1 + n) % POINTER_PLAYERS;
    if (!pointer_visible(&pointer, player))
      continue;
    const input_state_t *in = &pointer.players[player].input;
    pointer_color_t color = pointer_player_color(player);
    shown[count++] = (wc_pointer_t){
        in->x / INPUT_ONE, in->y / INPUT_ONE,
        connected > 1 ? dg_cursor_ink(color.r, color.g, color.b) : 1};
  }
  unsigned externals = host.video_revision ? host.video_revision(host.ctx) : 0;
  return wc_render_pointers(&compositor, &director, shown, count, externals)
             ? compositor.pixels
             : NULL;
}
// Text entry. When a press lands on an editable field the page (or the
// Android host) opens a text box over the field and pauses the game while it
// is open. The same checks decide when a field accepts entry. The field's stage
// rectangle is filled in for the page to place the box.
static int text_field_rect[4];
EMSCRIPTEN_KEEPALIVE unsigned d64_text_field(int x, int y) {
#if DG_EXTENDED
  if (!booted || values.failed || director.text_pending)
    return 0;
  unsigned sprite = dg_mouse_hit(&director, x, y);
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
  return sprite;
#else
  (void)x;
  (void)y;
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE const int *d64_text_field_rect(void) { return text_field_rect; }
// Key fields. Mucklas (D8) takes its player name through its own keyDown
// handlers, with their letter sounds and length limit, rather than a
// committed edit; the host opens its keyboard over the field and sends each
// character as a key. The field
// at the pointer, or with `anywhere` the one taking a name now, while it
// does; its stage rectangle is filled in as d64_text_field's.
#if DG_D10 && !defined(DIRECTOR64_MUCKLAS)
// The field a port's own keyDown handlers type a name into (game.toml
// [player.key_field]): the member of that name in that movie, while the
// score is in one of the frame ranges.
static bool game_key_field(dg_runtime_t *d, unsigned sprite) {
#ifdef GAME_KEY_FIELD_MOVIE
  static const unsigned frames[] = {GAME_KEY_FIELD_FRAMES};
  if (!d->movie || strcasecmp(d->movie->code->name, GAME_KEY_FIELD_MOVIE)) return false;
  bool inside = false;
  for (unsigned i = 0; i + 1 < sizeof(frames) / sizeof(*frames); i += 2)
    inside |= d->frame >= frames[i] && d->frame <= frames[i + 1];
  const dg_member_t *m = inside ? dg_member(d, d->sprites[sprite].value.member) : NULL;
  return m && m->name && !strcasecmp(m->name, GAME_KEY_FIELD_MEMBER);
#else
  (void)d;
  (void)sprite;
  return false;
#endif
}
#endif
EMSCRIPTEN_KEEPALIVE unsigned d64_key_field(int x, int y, int anywhere) {
#if defined(DIRECTOR64_MUCKLAS) || DG_D10
  if (!booted || values.failed || values.depth)
    return 0;
  unsigned first = anywhere ? 1 : dg_mouse_hit(&director, x, y);
  unsigned last = anywhere ? DG_SPRITES - 1 : first;
  for (unsigned sprite = first; sprite && sprite <= last && sprite < DG_SPRITES; sprite++)
#ifdef DIRECTOR64_MUCKLAS
    if (mucklas_name_target(&director, sprite)) {
#else
    if (game_key_field(&director, sprite)) {
#endif
      dg_bounds(&director, sprite, &text_field_rect[0], &text_field_rect[1],
                &text_field_rect[2], &text_field_rect[3]);
      return sprite;
    }
#else
  (void)x;
  (void)y;
  (void)anywhere;
#endif
  return 0;
}
// The field's text as the keyboard starts from: a lone space is empty.
EMSCRIPTEN_KEEPALIVE const char *d64_text_value(unsigned sprite) {
#if DG_EXTENDED || defined(DIRECTOR64_MUCKLAS)
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
#include "keyboard.inc"
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
// 1 to 8), while its page is up; 0 otherwise. The web page opens the
// browser's print dialog for it, Android shows a notice that printing is not
// available, and both then dismiss the page with B (printing.c).
EMSCRIPTEN_KEEPALIVE unsigned d64_print_request(void) {
#ifdef DIRECTOR64_PRINTING
  return booted ? game_print_id() : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t *d64_pixels(void) { return compositor.pixels; }
// The stage the page presents: 640x480, or the authored 800x600 (ink.h).
EMSCRIPTEN_KEEPALIVE unsigned d64_stage_width(void) { return WC_WIDTH; }
EMSCRIPTEN_KEEPALIVE unsigned d64_stage_height(void) { return WC_HEIGHT; }
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

// ---- The probe line protocol (platforms/web/node/probe.mjs) ----

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
// The names the games' state lines use for this runtime's services.
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
#if DG_EXTENDED
  // A typed entry still feeding its characters (the fuzzer waits for it).
  if (director.text_pending)
    emit(",\"text_pending\":true");
#endif
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
         "\"blend\":%u,\"loc\":[%d,%d],\"moveable\":%u",
         i, s->value.member, s->value.script, l, t, r, b, dg_opacity(s),
         s->value.x, s->value.y, s->moveable);
#if DG_EXTENDED
    // Fields a keyboard can type into.
    if (dg_text_editable(&director, i))
      emit(",\"editable\":1");
#endif
    emit("}");
  }
  const pointer_player_t *driver = pointer_driver(&pointer);
  emit("],\"pointer\":{\"player\":%u,\"x\":%d,\"y\":%d},\"saves\":{",
       pointer.active + 1, driver->input.x / INPUT_ONE,
       driver->input.y / INPUT_ONE);
  // The saved files by name, with their text.
  static char contents[16384];
  for (unsigned i = 0; game_save_name(i); i++) {
    unsigned length = 0;
    if (!game_read_file(game_save_name(i), contents, sizeof(contents), &length)) length = 0;
    contents[length] = 0;
    if (i)
      emit(",");
    quoted(game_save_name(i));
    emit(":");
    quoted(contents);
  }
  emit("},\"save_generation\":%u}\n", game_save_generation());
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
#ifdef DIRECTOR64_DEV_LENIENT
  unsigned member_id;
#endif
  if (!strcmp(command, "reboot")) {
    d64_boot(seed);
#ifdef DIRECTOR64_DEV_LENIENT
  } else if (sscanf(command, "member %u", &member_id) == 1) {
    // Development: what a sprite's member id names.
    const dg_member_t *m = dg_member(&director, member_id);
    emit("{\"member\":%u,\"found\":%s", member_id, m ? "true" : "false");
    if (m)
      emit(",\"name\":\"%s\",\"type\":%u,\"number\":%u,\"cast\":%u,\"width\":%u,\"height\":%u,\"asset\":\"%s\"",
           m->name ? m->name : "", m->type, m->number, m->cast, m->width, m->height, m->asset ? m->asset : "");
    emit("}\n");
    return output;
  } else if (sscanf(command, "movie %63s", entered) == 1) {
    // Development: enter a movie directly, as `go to movie` would, at an
    // optional label ("movie VIDEOMAT.DXR 06_02").
    snprintf(director.next_movie, sizeof(director.next_movie), "%s", entered);
    director.next_movie_frame = 1;
#if DG_MODERN
    director.next_movie_label[0] = 0;
    char label[32];
    if (sscanf(command, "movie %*s %31s", label) == 1)
      snprintf(director.next_movie_label, sizeof(director.next_movie_label), "%s", label);
#endif
  } else if (sscanf(command, "hide %u", &member_id) == 1 && member_id < DG_SPRITES) {
    director.sprites[member_id].visible = false;
    director.visual_revision++;
    emit("{}\n");
    return output;
  } else if (sscanf(command, "spritetext %u", &member_id) == 1 && member_id < DG_SPRITES) {
    lv_t text = lv_get(&values, NULL, "text",
                       lv_make(LV_MEMBER, (int32_t)director.sprites[member_id].value.member));
    emit("{\"text\":");
    quoted(lv_cstr(&values, text));
    emit("}\n");
    return output;
  } else if (!strncmp(command, "inspect ", 8)) {
    // Development: a global, then a property path, formatted.
    char words[8][48];
    int n = sscanf(command + 8, "%47s %47s %47s %47s %47s %47s %47s %47s", words[0], words[1],
                   words[2], words[3], words[4], words[5], words[6], words[7]);
    lv_t v = {0};
    for (unsigned i = 0; i < values.global_count; i++)
      if (n > 0 && !strcmp(global_names[i], words[0])) v = values.globals[i];
    for (int i = 1; i < n && !values.failed; i++)
      v = lv_get(&values, NULL, words[i], v);
    static char text[16384];
    if (!lv_format(&values, v, text, sizeof(text))) strcpy(text, "<format failed>");
    emit("{\"inspect\":");
    quoted(text);
    emit("}\n");
    return output;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  } else if (!strcmp(command, "children")) {
    emit("{\"children\":[");
    for (unsigned i = 0; i < director.film_child_count; i++) {
      const dg_film_child_t *c = &director.film_children[i];
      emit("%s[%u,%u,%u,%d,%d,%u,%u]", i ? "," : "", c->parent, c->channel, c->member, c->x, c->y, c->ink, c->blend);
    }
    emit("]}\n");
    return output;
#endif
  } else if (!strcmp(command, "clock")) {
    emit("{\"clock\":%llu,\"deadline\":%llu,\"stalled\":%u,\"tempo\":%u,\"puppet_tempo\":%u,"
         "\"phase\":%u,\"event_cursor\":%u,\"score_delay\":%u,\"next_frame\":%u}\n",
         (unsigned long long)director.clock, (unsigned long long)director.frame_deadline,
         director.frame_stalled, director.tempo, director.puppet_tempo, director.phase,
         director.event_cursor,
#if DG_MODERN
         director.score_delay,
#else
         0u,
#endif
         director.next_frame);
    return output;
  } else if (sscanf(command, "sprite %u", &member_id) == 1 && member_id < DG_SPRITES) {
    const dg_sprite_t *sp = &director.sprites[member_id];
    const dg_member_t *m = dg_member(&director, sp->value.member);
    emit("{\"sprite\":%u,\"member\":%u,\"ink\":%u,\"blend\":%u,\"type\":%u,\"visible\":%u,\"puppet\":%u,"
         "\"fore\":%u,\"back\":%u,\"fore_rgb\":%u,\"back_rgb\":%u,\"rect\":[%d,%d,%d,%d],\"film_frame\":%u,"
         "\"member_type\":%u,\"shape\":%u,\"pattern\":%u,\"filled\":%u,\"line\":%u}\n",
         member_id, sp->value.member, sp->value.ink, sp->value.blend, sp->value.type, sp->visible, sp->puppet,
         sp->value.fore, sp->value.back,
#if DG_MODERN
         sp->value.fore_rgb, sp->value.back_rgb,
#else
         0u, 0u,
#endif
         sp->value.x, sp->value.y,
         sp->value.width, sp->value.height, sp->film_frame, m ? m->type : 0, m ? m->shape : 0, m ? m->pattern : 0,
         m ? m->filled : 0, m ? m->line_width : 0);
    return output;
#endif
  } else if (sscanf(command, "pad %u %d %d %u", &ticks, &x, &y, &buttons) == 4 &&
             ticks <= 100000 && x >= -128 && x <= 127 && y >= -128 && y <= 127 &&
             buttons <= UINT16_MAX) {
    input_sample_t sample = {.connected = true, .stick_x = x, .stick_y = y,
                             .buttons = buttons};
    for (unsigned i = 0; i < ticks && !values.failed && !director.quit; i++) {
      pointer_update(&pointer, &director, &sample);
      const input_state_t *in = &pointer_driver(&pointer)->input;
      bool ok = step(in->x / INPUT_ONE, in->y / INPUT_ONE, !!(in->held & INPUT_A));
      if (probe_render) d64_render(0, 0, 0);
      if (!ok || values.failed) break;
    }
  } else if (sscanf(command, "point %u", &sprite) == 1 && sprite > 0 &&
             sprite < DG_SPRITES) {
    point(sprite);
    return output;
  } else if (sscanf(command, "step %u %d %d %d", &ticks, &x, &y, &down) == 4 &&
             ticks <= 100000) {
    for (unsigned i = 0; i < ticks && !values.failed && !director.quit; i++) {
      bool ok = step(x, y, (unsigned)down);
      if (probe_render) d64_render(0, 0, 0);
      if (!ok || values.failed) break;
    }
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
