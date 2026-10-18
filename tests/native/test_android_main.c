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
  assert(va_arg(args, int) == 42);
  assert(va_arg(args, int));
  va_end(args);
  committed++;
}
static const struct JNINativeInterface_ table = {.NewStringUTF = string_copy,
                                                 .CallVoidMethod = callback};
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
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t x, int32_t y,
                                         int32_t f) {
  (void)w;
  (void)x;
  (void)y;
  (void)f;
  return 0;
}
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *b, void *r) {
  (void)w;
  (void)b;
  (void)r;
  blocking_work();
  return -1;
}
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) {
  (void)w;
  return 0;
}
static void *present_work(void *unused) {
  (void)unused;
  present(NULL);
  return NULL;
}
int main(void) {
  alarm(5); // A regressed UI/engine lock is an immediate test failure.
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
  puts("Android main: UI queues and diagnostics stay responsive, stale edits "
       "rejected, surface lifetime safe");
}
