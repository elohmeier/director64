// Failed AAudio setup must be a runtime failure rather than silent success.
#define _POSIX_C_SOURCE 200809L
#include "../../platforms/android/jni/mixer.c"
#include <assert.h>

static unsigned mode, closed, deleted;
static AAudioStreamBuilder builder;
static AAudioStream device;

int __android_log_print(int priority, const char *tag, const char *format, ...) {
  (void)priority; (void)tag; (void)format; return 0;
}
void sound_release(sound_t *sound) { (void)sound; }
int AAudio_createStreamBuilder(AAudioStreamBuilder **out) {
  *out = mode == 1 ? NULL : &builder;
  return mode == 1 ? -1 : AAUDIO_OK;
}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, int f) { (void)b; (void)f; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int n) { (void)b; (void)n; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int u) { (void)b; (void)u; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, int t) { (void)b; (void)t; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b,
    aaudio_data_callback_result_t (*fn)(AAudioStream *, void *, void *, int32_t), void *ctx) {
  (void)b; (void)fn; (void)ctx;
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b,
    void (*fn)(AAudioStream *, void *, aaudio_result_t), void *ctx) {
  (void)b; (void)fn; (void)ctx;
}
int AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out) {
  (void)b; *out = mode == 2 ? NULL : &device; return mode == 2 ? -2 : AAUDIO_OK;
}
int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; return mode == 3 ? 0 : 48000; }
int AAudioStream_requestStart(AAudioStream *s) { (void)s; return mode == 4 ? -4 : AAUDIO_OK; }
int AAudioStream_close(AAudioStream *s) { assert(s == &device); closed++; return AAUDIO_OK; }
int AAudioStreamBuilder_delete(AAudioStreamBuilder *b) {
  assert(b == &builder); deleted++; return AAUDIO_OK;
}

int main(void) {
  const char *expected[] = {"", "create", "open", "sample rate", "start"};
  for (mode = 0; mode <= 4; mode++) {
    failure[0] = 0;
    stream = NULL;
    closed = deleted = 0;
    open_output();
    char error[512];
    assert(audio_error(error, sizeof(error)) == (mode != 0));
    assert(strstr(error, expected[mode]));
    assert((stream != NULL) == (mode == 0));
    assert(deleted == (mode != 1));
    assert(closed == (mode == 3 || mode == 4));
  }
  puts("Android AAudio: create/open/rate/start failures reported and resources released");
  return 0;
}
