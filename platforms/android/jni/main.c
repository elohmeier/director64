// The Android TV backend: the browser runtime's engine (web_runtime.c and
// its software compositor) with native services, driven by a game thread at
// the console's 60 Hz service clock, as the page's player drives it
// (platforms/web/site/player.js). The activity (GameActivity.java) owns the
// surface, the controllers and the dialogs; this owns the engine.
#include "compositor.h"
#include "decode.h"
#include "mixer.h"
#include "packs.h"
#include "video.h"
#include "web_runtime.h"
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "director64", __VA_ARGS__)

// web_runtime.c's entry points (the page calls them through Emscripten).
uint8_t *d64_flash(void);
unsigned d64_flash_bytes(void);
int d64_load_package(const uint8_t *data, size_t length);
const char *d64_package_error(void);
int d64_boot(unsigned seed);
int d64_step(int x, int y, int buttons);
void d64_pad(unsigned player, int connected, unsigned buttons, int stick_x, int stick_y);
void d64_pointer_warp(int x, int y);
int d64_pointer_x(void);
int d64_pointer_y(void);
unsigned d64_pointer_player(void);
int d64_step_pads(void);
uint32_t *d64_render_pads(void);
uint32_t *d64_render(int x, int y, int pointer_shown);
uint32_t *d64_pixels(void);
unsigned d64_text_field(int x, int y);
const int *d64_text_field_rect(void);
const char *d64_text_value(unsigned sprite);
unsigned d64_key_field(int x, int y, int anywhere);
void d64_type_key(unsigned character, int down);
int d64_edit_text(unsigned sprite, const char *text);
void d64_key(unsigned code, unsigned character, int down);
unsigned d64_print_request(void);
const char *d64_error(void);
const char *d64_movie(void);
unsigned d64_frame(void);
unsigned d64_script_errors(void);
unsigned d64_image_bytes(void);

// The stage the runtime draws: 640x480, or an 800x600 game unscaled.
enum { WIDTH = WC_WIDTH, HEIGHT = WC_HEIGHT, PORTS = 4 };
// The console's catch-up rule (platforms/n64/director_main.c): at most four
// ticks per pass and twelve ticks of backlog; time beyond that is dropped
// rather than rushed through.
enum { MAX_STEPS = 4, BACKLOG_TICKS = 12 };
// What holds the game: the activity paused, its menu, text entry, a print
// request. Game time and audio stop together while anything holds them.
enum { HOLD_PAUSED = 1, HOLD_MENU = 2, HOLD_TEXT = 4, HOLD_PRINT = 8, HOLD_SURFACE = 16 };

typedef struct {
  bool connected;
  unsigned buttons, masked;
  int stick_x, stick_y;
} pad_t;

static JavaVM *vm;
static jobject activity;
static jmethodID on_text, on_layout, on_entry, on_entry_closed, on_stopped, on_print, on_saved;
static pthread_t thread;
static bool thread_running;
static pthread_mutex_t engine = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
// Guarded by `state`.
static unsigned holds;
static bool quitting, fresh_surface;
static ANativeWindow *window;
static pad_t pads[PORTS];
static struct {
  int x, y;
  unsigned buttons, pressed;
  bool inside, moved;
} mouse = {WIDTH / 2, HEIGHT / 2, 0, 0, false, false};
static char save_path[512];
static jobject asset_manager;
static float volume = 1.0f;
// The game thread's own.
static pack_t images, sounds, streams, data, videos;
static bool using_pads;
static unsigned entry_sprite;
// The open entry types into a key field (Mucklas' name) as keys.
static bool key_entry;
static char diagnostics[256];

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static JNIEnv *env_of_thread(void) {
  JNIEnv *env = NULL;
  (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
  return env;
}

static jbyteArray bytes_of(JNIEnv *env, const char *text) {
  size_t length = strlen(text);
  jbyteArray array = (*env)->NewByteArray(env, (jsize)length);
  if (array)
    (*env)->SetByteArrayRegion(env, array, 0, (jsize)length, (const jbyte *)text);
  return array;
}

// ---- The runtime's services ----

static uint32_t host_asset(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  uint32_t length = pack_read(&images, name, out, cap);
  if (!length)
    LOG("missing image %s", name);
  return length;
}

// Text is set by the activity with Android's own text renderer, as the page
// sets it on a canvas (player.js textRasterizer).
static bool host_text(void *ctx, const char *utf8, int width, int height,
                      const wc_text_style_t *style, uint8_t *coverage) {
  (void)ctx;
  JNIEnv *env = env_of_thread();
  if (!env || width <= 0 || height <= 0)
    return false;
  jbyteArray text = bytes_of(env, utf8);
  jbyteArray plane = (*env)->CallObjectMethod(
      env, activity, on_text, text, width, height, style ? (jint)style->font : 0,
      style ? (jint)style->size : 0, style ? (jint)style->align : 0,
      style ? style->ascent : 0, style ? style->line_height : 0);
  (*env)->DeleteLocalRef(env, text);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return false;
  }
  bool ok = plane && (*env)->GetArrayLength(env, plane) == width * height;
  if (ok)
    (*env)->GetByteArrayRegion(env, plane, 0, width * height, (jbyte *)coverage);
  if (plane)
    (*env)->DeleteLocalRef(env, plane);
  return ok;
}

// Where the activity set each character of a text box (Text.layout).
static int host_layout(void *ctx, const char *utf8, int width, int height,
                       const wc_text_style_t *style, int16_t *boxes, int max_chars) {
  (void)ctx;
  JNIEnv *env = env_of_thread();
  if (!env || width <= 0 || height <= 0)
    return 0;
  jbyteArray text = bytes_of(env, utf8);
  jshortArray placed = (*env)->CallObjectMethod(
      env, activity, on_layout, text, width, height, style ? (jint)style->font : 0,
      style ? (jint)style->size : 0, style ? (jint)style->align : 0,
      style ? style->ascent : 0, style ? style->line_height : 0);
  (*env)->DeleteLocalRef(env, text);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return 0;
  }
  if (!placed)
    return 0;
  int count = (*env)->GetArrayLength(env, placed) / 4;
  if (count > max_chars)
    count = max_chars;
  (*env)->GetShortArrayRegion(env, placed, 0, count * 4, (jshort *)boxes);
  (*env)->DeleteLocalRef(env, placed);
  return count;
}

static void host_sound(void *ctx, unsigned channel, const char *asset, bool looping,
                       uint32_t loop_start, uint32_t loop_end) {
  (void)ctx;
  audio_play(channel, asset, looping, loop_start, loop_end);
}

static void host_gain(void *ctx, unsigned channel, float gain) {
  (void)ctx;
  audio_gain(channel, gain);
}

// A committed save generation: the flash image is written whole beside the
// old one and replaces it, so an interrupted write keeps the previous save.
static void host_save_commit(void *ctx, bool ok) {
  (void)ctx;
  if (ok) {
    char temporary[600];
    snprintf(temporary, sizeof(temporary), "%s.new", save_path);
    FILE *f = fopen(temporary, "wb");
    ok = f && fwrite(d64_flash(), 1, d64_flash_bytes(), f) == d64_flash_bytes();
    if (f) {
      ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
      ok = fclose(f) == 0 && ok;
    }
    ok = ok && rename(temporary, save_path) == 0;
  }
  JNIEnv *env = env_of_thread();
  if (env)
    (*env)->CallVoidMethod(env, activity, on_saved, (jboolean)ok);
}

static void host_trace(void *ctx, const char *text) {
  (void)ctx;
  LOG("%s", text);
}

static uint32_t host_stream(void *ctx, unsigned channel, const char *name) {
  (void)ctx;
  if (!name) {
    audio_stop(channel);
    return 0;
  }
  return audio_play_stream(channel, name);
}

static int32_t host_data(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  const pack_entry_t *entry = pack_find(&data, name);
  if (!entry)
    return -1;
  if (out && cap >= entry->length)
    memcpy(out, pack_data(&data, entry), entry->length);
  return (int32_t)entry->length;
}

static void host_video(void *ctx, unsigned sprite, const char *asset, const char *audio,
                       double time, double rate, unsigned serial, float gain) {
  (void)ctx;
  video_update(sprite, asset, audio, time, rate, serial, gain);
}

static bool host_video_frame(void *ctx, unsigned sprite, uint32_t *out, uint32_t cap,
                             unsigned *width, unsigned *height) {
  (void)ctx;
  return video_frame(sprite, out, cap, width, height);
}

static unsigned host_video_revision(void *ctx) {
  (void)ctx;
  return video_revision();
}

// ---- The game thread ----

static void stopped(int status, const char *message) {
  JNIEnv *env = env_of_thread();
  jbyteArray text = bytes_of(env, message);
  (*env)->CallVoidMethod(env, activity, on_stopped, status, text);
  (*env)->DeleteLocalRef(env, text);
}

static bool load(char *error, size_t cap) {
  static const char *const kinds[] = {"images", "audio", "streams", "data", "video"};
  pack_t *packs[] = {&images, &sounds, &streams, &data, &videos};
  for (unsigned i = 0; i < 5; i++)
    if (!pack_open(packs[i], kinds[i], error, cap))
      return false;
  size_t length = 0;
  char *package = game_asset("game.d64p", &length);
  if (!package) {
    snprintf(error, cap, "game.d64p is missing");
    return false;
  }
  int loaded = d64_load_package((const uint8_t *)package, length);
  free(package);
  if (!loaded) {
    snprintf(error, cap, "The game package was rejected: %s", d64_package_error());
    return false;
  }
  uint8_t *flash = d64_flash();
  size_t saved = 0;
  char *save = read_file(save_path, &saved);
  if (save && saved == d64_flash_bytes())
    memcpy(flash, save, saved);
  else
    memset(flash, 0xff, d64_flash_bytes());
  free(save);
  return true;
}

static void present(const uint32_t *pixels) {
  pthread_mutex_lock(&state);
  ANativeWindow_Buffer buffer;
  if (window && ANativeWindow_lock(window, &buffer, NULL) == 0) {
    uint32_t *out = buffer.bits;
    for (int y = 0; y < HEIGHT && y < buffer.height; y++)
      memcpy(out + (size_t)y * (size_t)buffer.stride, pixels + y * WIDTH,
             (size_t)(buffer.width < WIDTH ? buffer.width : WIDTH) * 4);
    ANativeWindow_unlockAndPost(window);
  }
  pthread_mutex_unlock(&state);
}

// Shows the activity's text entry for a field, its rectangle from the last
// d64_text_field or d64_key_field.
static void show_entry(unsigned sprite, bool keys) {
  const int *rect = d64_text_field_rect();
  entry_sprite = sprite;
  key_entry = keys;
  JNIEnv *env = env_of_thread();
  jbyteArray value = bytes_of(env, d64_text_value(sprite));
  // The activity places it in the 640x480 units its mouse speaks.
  (*env)->CallVoidMethod(env, activity, on_entry, (jint)sprite, value, rect[0] * 640 / WIDTH,
                         rect[1] * 480 / HEIGHT, rect[2] * 640 / WIDTH, rect[3] * 480 / HEIGHT,
                         (jboolean)keys);
  (*env)->DeleteLocalRef(env, value);
}

// Opens text entry over the field at the pointer: the game holds until the
// activity commits or cancels it.
static bool open_entry(int x, int y) {
  unsigned sprite = d64_text_field(x, y);
  if (!sprite)
    return false;
  pthread_mutex_lock(&state);
  holds |= HOLD_TEXT;
  pthread_mutex_unlock(&state);
  show_entry(sprite, false);
  return true;
}

// A key field (Mucklas' name) takes the TV's keyboard when it starts taking
// a name and when it is clicked, and lets it go when the game is done with
// it. The game runs meanwhile: each character the keyboard types reaches its
// key handlers at once (Native.typeKey), as on the console.
static void serve_key_field(bool clicked, int x, int y) {
  static bool was_taking;
  unsigned taking = d64_key_field(0, 0, 1);
  if (key_entry && !taking) {
    JNIEnv *env = env_of_thread();
    (*env)->CallVoidMethod(env, activity, on_entry_closed);
    entry_sprite = 0;
    key_entry = false;
  } else if (taking && !entry_sprite &&
             (!was_taking || (clicked && d64_key_field(x, y, 0) == taking))) {
    d64_key_field(0, 0, 1); // the rectangle of the field itself
    show_entry(taking, true);
  }
  was_taking = taking != 0;
}

static void *game(void *arg) {
  (void)arg;
  JNIEnv *env;
  (*vm)->AttachCurrentThread(vm, &env, NULL);
  char error[512];
  if (!load(error, sizeof(error))) {
    stopped(1, error);
    goto done;
  }
  web_runtime_host((web_host_t){.asset = host_asset, .text = host_text, .layout = host_layout,
                                .sound = host_sound,
                                .gain = host_gain, .save_commit = host_save_commit,
                                .trace = host_trace, .stream = host_stream, .data = host_data,
                                .video = host_video, .video_frame = host_video_frame,
                                .video_revision = host_video_revision});
  audio_init(&sounds, &streams);
  audio_volume(volume);
  video_init(&videos);
  unsigned seed = (unsigned)(now() * 1e6) ^ (unsigned)getpid() * 2654435761u;
  pthread_mutex_lock(&engine);
  d64_boot(seed);
  const char *failure = d64_error();
  pthread_mutex_unlock(&engine);
  if (*failure) {
    stopped(1, failure);
    goto done;
  }
  double last = now(), phase = 0, dropped = 0, window_start = last;
  unsigned steps = 0, renders = 0, printed = 0, injected[3], injections = 0;
  bool redraw = true, held = false, pad_a = false;
  for (;;) {
    pthread_mutex_lock(&state);
    while (!quitting && holds) {
      if (!held)
        audio_suspend(true);
      held = true;
      // The surface may change while the game is held.
      if (fresh_surface)
        break;
      pthread_cond_wait(&changed, &state);
    }
    bool quit = quitting;
    bool hold_now = holds != 0;
    if (fresh_surface) {
      fresh_surface = false;
      redraw = true;
    }
    if (!hold_now && held) {
      held = false;
      audio_suspend(false);
      last = now();
      phase = 0;
      // Whatever is held now belongs to the screen that just closed.
      for (unsigned i = 0; i < PORTS; i++)
        pads[i].masked = pads[i].buttons;
      mouse.buttons = mouse.pressed = 0;
    }
    // This pass's input.
    pad_t sample[PORTS];
    bool pads_active = false, pads_connected = false;
    for (unsigned i = 0; i < PORTS; i++) {
      pads[i].masked &= pads[i].buttons;
      sample[i] = pads[i];
      sample[i].buttons &= ~pads[i].masked;
      pads_connected |= sample[i].connected;
      pads_active |= sample[i].buttons || sample[i].stick_x || sample[i].stick_y;
    }
    bool mouse_moved = mouse.moved;
    mouse.moved = false;
    pthread_mutex_unlock(&state);
    if (quit)
      break;
    audio_poll();
    pthread_mutex_lock(&engine);
    if (hold_now) {
      if (redraw)
        present(d64_pixels());
      redraw = false;
      pthread_mutex_unlock(&engine);
      continue;
    }
    // Controllers drive the pointer from their first use until the mouse
    // moves.
    if (mouse_moved && !pads_active)
      using_pads = false;
    for (unsigned i = 0; i < PORTS; i++)
      d64_pad(i, sample[i].connected, sample[i].buttons, sample[i].stick_x, sample[i].stick_y);
    if (pads_active && !using_pads) {
      d64_pointer_warp(mouse.x, mouse.y);
      using_pads = true;
    }
    if (using_pads && !pads_connected)
      using_pads = false;
    double t = now();
    phase += (t - last) * 60e6;
    last = t;
    if (phase > BACKLOG_TICKS * 1e6) {
      dropped += (phase - BACKLOG_TICKS * 1e6) / 60e6;
      phase = BACKLOG_TICKS * 1e6;
    }
    unsigned owed = (unsigned)(phase / 1e6);
    if (owed > MAX_STEPS)
      owed = MAX_STEPS;
    phase -= owed * 1e6;
    // A press this pass, from the mouse or any controller's A.
    bool clicked, a_now = false;
    for (unsigned i = 0; i < PORTS; i++)
      a_now |= sample[i].connected && (sample[i].buttons & 1);
    clicked = using_pads && a_now && !pad_a;
    pad_a = a_now;
    int status = 0;
    for (unsigned i = 0; i < owed && !status; i++) {
      pthread_mutex_lock(&state);
      unsigned buttons = mouse.buttons | mouse.pressed;
      bool mouse_press = (mouse.pressed & 1) != 0;
      clicked |= mouse_press;
      mouse.pressed = 0;
      int mx = mouse.x, my = mouse.y;
      pthread_mutex_unlock(&state);
      if (injections) {
        status = d64_step(mx, my, (int)injected[0]);
        memmove(injected, injected + 1, sizeof(injected) - sizeof(*injected));
        injections--;
      } else if (using_pads) {
        status = d64_step_pads();
        pthread_mutex_lock(&state);
        mouse.x = d64_pointer_x();
        mouse.y = d64_pointer_y();
        mouse.inside = true;
        pthread_mutex_unlock(&state);
        if (status == 3) {
          status = 0;
          open_entry(mouse.x, mouse.y);
          break;
        }
      } else {
        // A press on an editable field opens text entry instead.
        if (mouse_press && open_entry(mx, my))
          break;
        status = d64_step(mx, my, (int)buttons);
      }
      steps++;
      // The console shows a QR link to a print document; the TV has no
      // printer, so it says so and dismisses the page as the console's B
      // does (games/loewenzahn-1/runtime/printing.c).
      unsigned request = d64_print_request();
      if (request && request != printed && !injections) {
        printed = request;
        injected[0] = 0;
        injected[1] = 2;
        injected[2] = 0;
        injections = 3;
        (*env)->CallVoidMethod(env, activity, on_print, (jint)request);
      } else if (!request) {
        printed = 0;
      }
    }
    if (!status && *d64_error())
      status = 1;
    if (status) {
      char message[512];
      snprintf(message, sizeof(message), "%s", status == 2 ? "" : d64_error());
      pthread_mutex_unlock(&engine);
      stopped(status, message);
      break;
    }
    pthread_mutex_lock(&state);
    int px = mouse.x, py = mouse.y;
    bool inside = mouse.inside;
    pthread_mutex_unlock(&state);
    serve_key_field(clicked, px, py);
    uint32_t *pixels = using_pads ? d64_render_pads() : d64_render(px, py, inside);
    if (pixels || redraw) {
      present(d64_pixels());
      renders++;
      redraw = false;
    }
    if (t - window_start >= 1.0) {
      double seconds = t - window_start;
      snprintf(diagnostics, sizeof(diagnostics),
               "%s frame %u · %.0f ticks/s · %.0f redraws/s · images %u MB · dropped %.1f s · "
               "alerts %u%s",
               d64_movie(), d64_frame(), steps / seconds, renders / seconds,
               d64_image_bytes() >> 20, dropped, d64_script_errors(),
               using_pads ? " · pad" : "");
      steps = renders = 0;
      window_start = t;
    }
    pthread_mutex_unlock(&engine);
    // Until the next tick is owed.
    double wait = (1e6 - phase) / 60e6;
    if (wait > 0) {
      struct timespec ts = {0, (long)(wait * 1e9)};
      nanosleep(&ts, NULL);
    }
  }
done:
  video_close_all();
  audio_shutdown();
  pack_t *packs[] = {&images, &sounds, &streams, &data, &videos};
  for (unsigned i = 0; i < 5; i++)
    pack_close(packs[i]);
  (*vm)->DetachCurrentThread(vm);
  return NULL;
}

// ---- JNI ----

JNIEXPORT jint JNI_OnLoad(JavaVM *machine, void *reserved) {
  (void)reserved;
  vm = machine;
  return JNI_VERSION_1_6;
}

static void copy_string(JNIEnv *env, jstring from, char *to, size_t cap) {
  const char *text = (*env)->GetStringUTFChars(env, from, NULL);
  snprintf(to, cap, "%s", text);
  (*env)->ReleaseStringUTFChars(env, from, text);
}

#define NATIVE(name) JNICALL Java_org_director64_tv_Native_##name

JNIEXPORT void NATIVE(start)(JNIEnv *env, jclass cls, jobject owner, jobject assets,
                             jstring save, jfloat level) {
  (void)cls;
  if (thread_running)
    return;
  activity = (*env)->NewGlobalRef(env, owner);
  jclass type = (*env)->GetObjectClass(env, owner);
  on_text = (*env)->GetMethodID(env, type, "rasterize", "([BIIIIIII)[B");
  on_layout = (*env)->GetMethodID(env, type, "layout", "([BIIIIIII)[S");
  on_entry = (*env)->GetMethodID(env, type, "openTextEntry", "(I[BIIIIZ)V");
  on_entry_closed = (*env)->GetMethodID(env, type, "closeTextEntry", "()V");
  on_stopped = (*env)->GetMethodID(env, type, "gameStopped", "(I[B)V");
  on_print = (*env)->GetMethodID(env, type, "printRequested", "(I)V");
  on_saved = (*env)->GetMethodID(env, type, "saveCommitted", "(Z)V");
  // The game's packs ship as the APK's assets.
  asset_manager = (*env)->NewGlobalRef(env, assets);
  packs_use(AAssetManager_fromJava(env, asset_manager));
  copy_string(env, save, save_path, sizeof(save_path));
  volume = level;
  quitting = false;
  thread_running = pthread_create(&thread, NULL, game, NULL) == 0;
}

JNIEXPORT void NATIVE(stop)(JNIEnv *env, jclass cls) {
  (void)cls;
  if (!thread_running)
    return;
  pthread_mutex_lock(&state);
  quitting = true;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&state);
  pthread_join(thread, NULL);
  thread_running = false;
  pthread_mutex_lock(&state);
  if (window)
    ANativeWindow_release(window);
  window = NULL;
  pthread_mutex_unlock(&state);
  (*env)->DeleteGlobalRef(env, activity);
  activity = NULL;
  (*env)->DeleteGlobalRef(env, asset_manager);
  asset_manager = NULL;
}

JNIEXPORT void NATIVE(surface)(JNIEnv *env, jclass cls, jobject surface) {
  (void)cls;
  pthread_mutex_lock(&state);
  if (window)
    ANativeWindow_release(window);
  window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
  if (window)
    ANativeWindow_setBuffersGeometry(window, WIDTH, HEIGHT, WINDOW_FORMAT_RGBX_8888);
  fresh_surface = window != NULL;
  if (window)
    holds &= ~(unsigned)HOLD_SURFACE;
  else
    holds |= HOLD_SURFACE;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&state);
}

JNIEXPORT void NATIVE(hold)(JNIEnv *env, jclass cls, jint reason, jboolean on) {
  (void)env;
  (void)cls;
  pthread_mutex_lock(&state);
  if (on)
    holds |= (unsigned)reason;
  else
    holds &= ~(unsigned)reason;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&state);
}

JNIEXPORT void NATIVE(pad)(JNIEnv *env, jclass cls, jint port, jboolean connected,
                           jint buttons, jint stick_x, jint stick_y) {
  (void)env;
  (void)cls;
  if (port < 0 || port >= PORTS)
    return;
  pthread_mutex_lock(&state);
  pads[port].connected = connected;
  pads[port].buttons = connected ? (unsigned)buttons : 0;
  pads[port].stick_x = stick_x;
  pads[port].stick_y = stick_y;
  pthread_mutex_unlock(&state);
}

// The mouse in 640x480 units (GameActivity), mapped to stage pixels;
// `buttons` are input.h bits (left A, right B).
JNIEXPORT void NATIVE(mouse)(JNIEnv *env, jclass cls, jint x, jint y, jint buttons,
                             jboolean inside) {
  (void)env;
  (void)cls;
  x = x * WIDTH / 640;
  y = y * HEIGHT / 480;
  pthread_mutex_lock(&state);
  mouse.moved |= x != mouse.x || y != mouse.y || (unsigned)buttons != mouse.buttons;
  mouse.x = x < 0 ? 0 : x >= WIDTH ? WIDTH - 1 : x;
  mouse.y = y < 0 ? 0 : y >= HEIGHT ? HEIGHT - 1 : y;
  // A click shorter than a tick still reaches the score.
  mouse.pressed |= (unsigned)buttons & ~mouse.buttons;
  mouse.buttons = (unsigned)buttons;
  mouse.inside = inside;
  pthread_mutex_unlock(&state);
}

JNIEXPORT void NATIVE(key)(JNIEnv *env, jclass cls, jint code, jint character, jboolean down) {
  (void)env;
  (void)cls;
  pthread_mutex_lock(&engine);
  d64_key((unsigned)code, (unsigned)character, down);
  pthread_mutex_unlock(&engine);
}

JNIEXPORT void NATIVE(cancelText)(JNIEnv *env, jclass cls) {
  pthread_mutex_lock(&engine);
  entry_sprite = 0;
  key_entry = false;
  pthread_mutex_unlock(&engine);
  Java_org_director64_tv_Native_hold(env, cls, HOLD_TEXT, JNI_FALSE);
}

// Commits text entry the way the keyboard's Start does; false when the
// field refuses it (the dialog stays open).
JNIEXPORT jboolean NATIVE(commitText)(JNIEnv *env, jclass cls, jbyteArray text) {
  (void)cls;
  char value[64];
  jsize length = (*env)->GetArrayLength(env, text);
  if (length >= (jsize)sizeof(value))
    return JNI_FALSE;
  (*env)->GetByteArrayRegion(env, text, 0, length, (jbyte *)value);
  value[length] = 0;
  pthread_mutex_lock(&engine);
  bool accepted = entry_sprite && d64_edit_text(entry_sprite, value);
  pthread_mutex_unlock(&engine);
  if (accepted)
    Java_org_director64_tv_Native_cancelText(env, cls);
  return accepted;
}

// One character the keyboard typed into a key field; 8 deletes.
JNIEXPORT void NATIVE(typeKey)(JNIEnv *env, jclass cls, jint character) {
  (void)env;
  (void)cls;
  pthread_mutex_lock(&engine);
  if (key_entry) {
    d64_type_key((unsigned)character, 1);
    d64_type_key((unsigned)character, 0);
  }
  pthread_mutex_unlock(&engine);
}

JNIEXPORT void NATIVE(volume)(JNIEnv *env, jclass cls, jfloat level) {
  (void)env;
  (void)cls;
  volume = level;
  audio_volume(level);
}

JNIEXPORT jstring NATIVE(diagnostics)(JNIEnv *env, jclass cls) {
  (void)cls;
  char copy[256];
  pthread_mutex_lock(&engine);
  snprintf(copy, sizeof(copy), "%s", diagnostics);
  pthread_mutex_unlock(&engine);
  return (*env)->NewStringUTF(env, copy);
}
