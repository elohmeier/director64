#define _POSIX_C_SOURCE 200809L
#include "../../platforms/android/jni/main.c"
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed = PTHREAD_COND_INITIALIZER;
static bool entered, proceed;
static pthread_t engine_owner;
static unsigned keys, edits, committed;
static unsigned test_user_id, test_user_kind, file_replies, file_completions, file_cancels;
static bool legacy_finished;
static unsigned cursor_callbacks;
static int cursor_callback_kind;
static char diagnostic_copy[256];
struct ANativeWindow {
  unsigned refs;
};
static struct ANativeWindow target = {1};
static void blocking_work(void) {
  pthread_mutex_lock(&test_lock);
  entered = true;
  pthread_cond_signal(&test_changed);
  while (!proceed)
    pthread_cond_wait(&test_changed, &test_lock);
  pthread_mutex_unlock(&test_lock);
}
static void wait_busy(void) {
  pthread_mutex_lock(&test_lock);
  while (!entered)
    pthread_cond_wait(&test_changed, &test_lock);
  pthread_mutex_unlock(&test_lock);
}
static void unblock(void) {
  pthread_mutex_lock(&test_lock);
  proceed = true;
  pthread_cond_signal(&test_changed);
  pthread_mutex_unlock(&test_lock);
}
void d64_key(unsigned code, unsigned character, int down) {
  (void)code;
  (void)character;
  (void)down;
  assert(pthread_equal(engine_owner, pthread_self()));
  if (!keys++)
    blocking_work();
}
void d64_type_key(unsigned c, int down) {
  (void)c;
  (void)down;
  assert(false);
}
int d64_edit_text(unsigned sprite, const char *text) {
  assert(pthread_equal(engine_owner, pthread_self()));
  assert(sprite == 7 && !strcmp(text, "name"));
  edits++;
  return 1;
}
unsigned d64_key_capacity(void) { return 32; }
unsigned d64_user_request_id(void) { return test_user_id; }
unsigned d64_user_request_kind(void) { return test_user_kind; }
unsigned d64_user_size(void) { return 0; }
const uint8_t *d64_user_data(void) { return NULL; }
const char *d64_user_title(void) { return ""; }
const char *d64_user_name(void) { return ""; }
const char *d64_user_filter(void) { return ""; }
int d64_user_reply(unsigned id, const char *name, const uint8_t *data, unsigned size,
                    const uint32_t *pixels, unsigned width, unsigned height) {
  assert(pthread_equal(engine_owner, pthread_self()));
  assert(id == 91 && test_user_kind == 1 &&
         !strcmp(name, "\303\204uto Stra\303\237e \342\202\254.bmp"));
  assert(size == 3 && data[0] == 11 && data[1] == 22 && data[2] == 33);
  assert(width == 1 && height == 1 && pixels[0] == 0x04030201u);
  file_replies++;
  test_user_kind = 0;
  return 1;
}
int d64_user_cancel(unsigned id) {
  assert(pthread_equal(engine_owner, pthread_self()));
  assert(id == test_user_id);
  file_cancels++;
  test_user_kind = 0;
  return 1;
}
int d64_user_complete(unsigned id, int success) {
  assert(pthread_equal(engine_owner, pthread_self()));
  assert(id == test_user_id && test_user_kind == 3 && success);
  file_completions++;
  test_user_kind = 0;
  return 1;
}
void audio_suspend(bool held) { (void)held; }
static jstring JNICALL string_copy(JNIEnv *env, const char *value) {
  (void)env;
  snprintf(diagnostic_copy, sizeof(diagnostic_copy), "%s", value);
  return (jstring)diagnostic_copy;
}
static void JNICALL callback(JNIEnv *env, jobject obj, jmethodID method, ...) {
  (void)env;
  (void)obj;
  (void)method;
  va_list args;
  va_start(args, method);
  if(on_pointer && method==on_pointer) {
    (void)va_arg(args,int);
    cursor_callback_kind=va_arg(args,int);
    cursor_callbacks++;
    va_end(args);
    return;
  }
  if (on_user_reply && method == on_user_reply) {
    assert(va_arg(args, int) == 91);
    assert(va_arg(args, int) == 1);
    assert(va_arg(args, int));
    va_end(args);
    return;
  }
  assert(va_arg(args, int) == 42);
  assert(va_arg(args, int));
  va_end(args);
  committed++;
}
typedef struct { jsize size; jbyte *data; } test_array_t;
static jsize JNICALL array_length(JNIEnv *env, jarray array) {
  (void)env;
  return ((test_array_t *)array)->size;
}
static void JNICALL array_region(JNIEnv *env, jbyteArray array, jsize start,
                                 jsize count, jbyte *out) {
  (void)env;
  test_array_t *input = (test_array_t *)array;
  assert(start >= 0 && count >= 0 && start + count <= input->size);
  memcpy(out, input->data + start, (size_t)count);
}
static jboolean JNICALL no_exception(JNIEnv *env) { (void)env; return JNI_FALSE; }
static const struct JNINativeInterface_ table = {.NewStringUTF = string_copy,
    .CallVoidMethod = callback, .GetArrayLength = array_length,
    .GetByteArrayRegion = array_region, .ExceptionCheck = no_exception};
static JNIEnv environment = &table;
static void *engine_work(void *unused) {
  (void)unused;
  engine_owner = pthread_self();
  for (;;) {
    host_command_t c;
    pthread_mutex_lock(&state);
    bool got = host_command_pop(&commands, &c);
    pthread_mutex_unlock(&state);
    if (!got)
      break;
    run_command(&environment, &c);
  }
  return NULL;
}
static void *file_engine_work(void *unused) {
  (void)unused;
  engine_owner = pthread_self();
  legacy_finished = run_user_message(&environment);
  return NULL;
}
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface) {
  (void)env;
  (void)surface;
  target.refs++;
  return &target;
}
void ANativeWindow_acquire(ANativeWindow *w) { w->refs++; }
void ANativeWindow_release(ANativeWindow *w) {
  assert(w->refs);
  w->refs--;
}
static bool present_blocks=true, replace_during_post;
static unsigned geometry_failures,lock_failures,post_failures,geometry_calls,post_calls;
static uint32_t output_pixels[WIDTH*HEIGHT];
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w,int32_t x,int32_t y,int32_t f) {
  (void)w;(void)x;(void)y;(void)f;geometry_calls++;
  if(geometry_failures){geometry_failures--;return -1;}
  return 0;
}
int32_t ANativeWindow_lock(ANativeWindow *w,ANativeWindow_Buffer *b,void *r) {
  (void)w;(void)r;
  if(present_blocks){blocking_work();return -1;}
  if(lock_failures){lock_failures--;return -1;}
  b->bits=output_pixels;b->width=WIDTH;b->height=HEIGHT;b->stride=WIDTH;
  return 0;
}
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) {
  (void)w;post_calls++;
  if(replace_during_post){replace_during_post=false;surface_generation++;}
  if(post_failures){post_failures--;return -1;}
  return 0;
}
static void cursor_post_failures(void) {
  static uint32_t cpu_pixels[WIDTH*HEIGHT];
  present_blocks=false;window=&target;surface_generation=100;
  on_pointer=(jmethodID)(uintptr_t)2;
  for(unsigned failure=0;failure<4;failure++) {
    surface_generation++;
    host_cursor_init(&cursor_policy);
    cursor_callbacks=0;
    bool redraw=true;
    assert(post_cursor_frame(&environment,cpu_pixels,true,HOST_CURSOR_SOFTWARE,&redraw));
    assert(cursor_drawn && !redraw);
    host_cursor_request(&cursor_policy,0);
    if(failure==0)lock_failures=1;
    if(failure==1)post_failures=1;
    if(failure==2){surface_generation++;geometry_failures=1;}
    if(failure==3)replace_during_post=true;
    unsigned before_geometry=geometry_calls;
    unsigned before_posts=post_calls;
    assert(!post_cursor_frame(&environment,cpu_pixels,false,0,&redraw));
    assert(redraw && cursor_drawn && cursor_callbacks==0);
    // A static renderer returns no new pixels: retained redraw retries its
    // previous cursor-free CPU image before enabling the native icon.
    assert(post_cursor_frame(&environment,cpu_pixels,false,0,&redraw));
    assert(!redraw && !cursor_drawn && cursor_callbacks==1 && cursor_callback_kind==0);
    if(failure==2)assert(geometry_calls==before_geometry+2);
    assert(post_calls-before_posts==((failure==1 || failure==3)?2u:1u));
  }
  host_cursor_request(&cursor_policy,1);
  unsigned old=cursor_policy.sequence;
  Java_org_director64_tv_Native_surface(NULL,NULL,NULL);
  assert(!Java_org_director64_tv_Native_pointerCurrent(NULL,NULL,(jint)old,1));
  assert(!present(cpu_pixels));
  window=NULL;target.refs=1;present_blocks=true;on_pointer=NULL;
}
static void *present_work(void *unused) {
  (void)unused;
  present(NULL);
  return NULL;
}
int main(void) {
  alarm(5); // A regressed UI/engine lock is an immediate test failure.
  cursor_post_failures();
  holds=0;
  Java_org_director64_tv_Native_pad(NULL,NULL,true,1,0,0);
  assert(input_from_pad);
  Java_org_director64_tv_Native_mouse(NULL,NULL,100,100,0,true,true);
  assert(!input_from_pad && physical_pointer && pad.masked==1);
  Java_org_director64_tv_Native_pad(NULL,NULL,true,1,0,0);
  assert(!input_from_pad); // A still-held controller cannot steal the mouse.
  Java_org_director64_tv_Native_pad(NULL,NULL,true,0,0,0);
  assert(!input_from_pad && !pad.masked);
  Java_org_director64_tv_Native_pad(NULL,NULL,true,2,0,0);
  assert(input_from_pad);
  Java_org_director64_tv_Native_mouse(NULL,NULL,101,100,0,true,true);
  assert(!input_from_pad && pad.masked==2);
  Java_org_director64_tv_Native_pad(NULL,NULL,true,2,80,0);
  assert(input_from_pad); // Deliberate new stick motion takes the single input.
  memset(&pad,0,sizeof(pad));
  memset(&mouse,0,sizeof(mouse));
  input_from_pad=physical_pointer=false;
  holds = HOLD_TEXT;
  entry_sprite = 7;
  entry_token = 42;
  snprintf(diagnostics, sizeof(diagnostics), "worker is busy");
  Java_org_director64_tv_Native_key(NULL, NULL, 0, 97, true);
  pthread_t worker;
  assert(!pthread_create(&worker, NULL, engine_work, NULL));
  wait_busy();
  // A UI call must not enter the engine, even while engine work cannot finish.
  Java_org_director64_tv_Native_key(NULL, NULL, 0, 97, false);
  Java_org_director64_tv_Native_diagnostics(&environment, NULL);
  assert(!strcmp(diagnostic_copy, "worker is busy"));
  assert(enqueue(
      (host_command_t){.kind = HOST_COMMIT, .code = 41, .text = "stale"}));
  assert(enqueue(
      (host_command_t){.kind = HOST_COMMIT, .code = 42, .text = "name"}));
  unblock();
  pthread_join(worker, NULL);
  assert(keys == 2 && edits == 1 && committed == 1 && !entry_sprite && !holds);
  // Surface loss must not wait on a blocked Android buffer producer.
  entered = proceed = false;
  window = &target;
  surface_generation = 1;
  assert(!pthread_create(&worker, NULL, present_work, NULL));
  wait_busy();
  Java_org_director64_tv_Native_surface(NULL, NULL, NULL);
  assert(!window && (holds & HOLD_SURFACE));
  unblock();
  pthread_join(worker, NULL);
  assert(target.refs == 0);
  // File callbacks own their queued bytes, reject stale (id,kind) pairs, and
  // enter the engine only from its worker even while a modal hold is active.
  on_user_reply = (jmethodID)(uintptr_t)1;
  holds = 0;
  test_user_id = 91;
  test_user_kind = 1;
  user_hold(91, 1);
  jbyte name_bytes[] = "\303\204uto Stra\303\237e \342\202\254.bmp";
  jbyte raw_bytes[] = {11, 22, 33}, rgba_bytes[] = {1, 2, 3, 4};
  test_array_t name = {(jsize)sizeof(name_bytes)-1, name_bytes};
  test_array_t raw = {3, raw_bytes}, rgba = {4, rgba_bytes};
  test_array_t oversized = {USER_RAW_LIMIT + 1, NULL};
  assert(!Java_org_director64_tv_Native_fileReply(&environment, NULL, 91, 1,
      (jbyteArray)&name, (jbyteArray)&oversized, NULL, 0, 0));
  assert(!Java_org_director64_tv_Native_fileReply(&environment, NULL, 91, 1,
      (jbyteArray)&name, (jbyteArray)&raw, (jbyteArray)&rgba, 4096, 4096));
  assert(!Java_org_director64_tv_Native_fileReply(&environment, NULL, 90, 1,
      (jbyteArray)&name, (jbyteArray)&raw, (jbyteArray)&rgba, 1, 1));
  assert(Java_org_director64_tv_Native_fileReply(&environment, NULL, 91, 1,
      (jbyteArray)&name, (jbyteArray)&raw, (jbyteArray)&rgba, 1, 1));
  raw_bytes[0] = rgba_bytes[0] = 99; // UI buffers cannot mutate the queued reply.
  assert(!file_replies && (holds & HOLD_USER));
  assert(!Java_org_director64_tv_Native_fileCancel(NULL, NULL, 91, 1));
  assert(!pthread_create(&worker, NULL, file_engine_work, NULL));
  pthread_join(worker, NULL);
  assert(file_replies == 1 && !holds && !user_message.action && !legacy_finished);
  // SAVE and EXPORT deliberately reuse the id; an old SAVE response must not
  // complete the later write request.
  test_user_kind = 3;
  user_hold(91, 3);
  assert(!Java_org_director64_tv_Native_fileCancel(NULL, NULL, 91, 2));
  assert(!Java_org_director64_tv_Native_fileComplete(NULL, NULL, 91, 2, true));
  assert(Java_org_director64_tv_Native_fileComplete(NULL, NULL, 91, 3, true));
  assert(!pthread_create(&worker, NULL, file_engine_work, NULL));
  pthread_join(worker, NULL);
  assert(file_completions == 1 && !holds);
  test_user_id = 92; test_user_kind = 1; user_hold(92, 1);
  assert(Java_org_director64_tv_Native_fileCancel(NULL, NULL, 92, 1));
  assert(!pthread_create(&worker, NULL, file_engine_work, NULL));
  pthread_join(worker, NULL);
  assert(file_cancels == 1 && !holds);
  // Legacy print completion dismisses the authored document notice only after
  // Android's print UI ends, without invoking a modern engine file capability.
  test_user_kind = 0; legacy_print = true; user_hold(4, 4);
  assert(Java_org_director64_tv_Native_fileComplete(NULL, NULL, 4, 4, false));
  assert(!pthread_create(&worker, NULL, file_engine_work, NULL));
  pthread_join(worker, NULL);
  assert(legacy_finished && !legacy_print && !holds && file_completions == 1);
  puts("Android main: UI queues and diagnostics stay responsive, stale edits "
       "rejected, surface lifetime safe; bounded file replies and print completion stay on worker");
}
