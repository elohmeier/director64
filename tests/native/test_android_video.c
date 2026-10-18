// Run the real decoder pump with asynchronous MediaCodec/ImageReader mocks.
#include "../../platforms/android/jni/video.c"
#include <assert.h>
#include <stdio.h>
static int64_t ready_pts[32], image_pts;
static int remaining, cursor, released, displayed, output_error, input_error;
static bool image_ready, bad_plane;
static int flushes;
static AMediaCodec codec;
static AImage image;
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t t) {
  (void)c;
  (void)t;
  return input_error ? -10000 : -1;
}
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t i, size_t *n) {
  (void)c;
  (void)i;
  (void)n;
  return NULL;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t i, off_t o,
                                            size_t n, uint64_t t, uint32_t f) {
  (void)c;
  (void)i;
  (void)o;
  (void)n;
  (void)t;
  (void)f;
  return 0;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c,
                                        AMediaCodecBufferInfo *info,
                                        int64_t t) {
  (void)c;
  (void)t;
  if (output_error)
    return -10000;
  if (!remaining)
    return -1;
  remaining--;
  *info = (AMediaCodecBufferInfo){.presentationTimeUs = ready_pts[cursor]};
  return cursor++;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t i,
                                               bool show) {
  (void)c;
  released++;
  displayed += show;
  if (show) {
    image_pts = ready_pts[i];
    image_ready = true;
  }
  return 0;
}
media_status_t AMediaCodec_flush(AMediaCodec *c) {
  (void)c;
  flushes++;
  return 0;
}
media_status_t AImageReader_acquireLatestImage(AImageReader *r, AImage **out) {
  (void)r;
  if (!image_ready)
    return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
  *out = &image;
  image_ready = false;
  return AMEDIA_OK;
}
media_status_t AImage_getTimestamp(AImage *i, int64_t *at) {
  (void)i;
  *at = image_pts * 1000;
  return AMEDIA_OK;
}
media_status_t AImage_getWidth(AImage *i, int32_t *n) {
  (void)i;
  *n = 1;
  return 0;
}
media_status_t AImage_getHeight(AImage *i, int32_t *n) {
  (void)i;
  *n = 1;
  return 0;
}
media_status_t AImage_getCropRect(AImage *i, AImageCropRect *c) {
  (void)i;
  *c = (AImageCropRect){0, 0, 1, 1};
  return 0;
}
media_status_t AImage_getPlaneData(AImage *i, int p, uint8_t **v, int *n) {
  (void)i;
  (void)p;
  static uint8_t byte = 128;
  *v = &byte;
  *n = bad_plane ? 0 : 1;
  return 0;
}
media_status_t AImage_getPlaneRowStride(AImage *i, int p, int *n) {
  (void)i;
  (void)p;
  *n = 1;
  return 0;
}
media_status_t AImage_getPlanePixelStride(AImage *i, int p, int *n) {
  (void)i;
  (void)p;
  *n = 1;
  return 0;
}
void AImage_delete(AImage *i) { (void)i; }
static void ready(int64_t at) {
  cursor = 0;
  remaining = 1;
  ready_pts[0] = at;
}
int main(void) {
  video_chunk_t frames[40] = {0};
  for (int i = 0; i < 40; i++)
    frames[i].microseconds = i * 40000;
  frames[0].key = true;
  player_t p = {.codec = &codec, .frames = frames, .count = 40, .shown_at = -1};
  // Legal decoder output one frame behind must continue presenting while
  // playing.
  for (int i = 1; i < 10; i++) {
    p.wanted = p.fed = i;
    ready((i - 1) * 40000);
    pump(&p);
    assert(p.shown_at == (i - 1) * 40000);
  }
  assert(released == 9 && displayed == 9 && revision == 9 && !video_error()[0]);
  // A burst keeps the newest eligible frame, with a bounded drain.
  p.wanted = p.fed = 39;
  cursor = 0;
  remaining = 20;
  for (int i = 0; i < 20; i++)
    ready_pts[i] = (i + 10) * 40000;
  unsigned before = revision;
  pump(&p);
  assert(remaining == 20 - PUMP_BUFFERS && p.shown_at == 17 * 40000 &&
         revision == before + 1);
  remaining = 0;
  // Flush generation fences late surface images, including a same-time explicit
  // seek.
  seek(&p, 20 * 24, true);
  assert(flushes == 1 && p.floor_at == 800000 && p.shown_at == -1);
  image_ready = true;
  image_pts = 400000;
  pump(&p);
  assert(p.shown_at == -1);
  ready(800000);
  pump(&p);
  assert(p.shown_at == 800000);
  seek(&p, 20 * 24, true);
  assert(flushes == 2 && p.shown_at == -1);
  // Lagging input is allowed to catch up; normal target motion must not
  // repeatedly flush the decoder just because its fed cursor is far behind.
  seek(&p, 39 * 24, false);
  assert(flushes == 2 && p.wanted == 39);
  // Invalid planes and fatal decoder errors are visible, not frozen pictures.
  bad_plane = true;
  ready(800000);
  pump(&p);
  assert(*video_error());
  bad_plane = false;
  failure[0] = 0;
  output_error = 1;
  pump(&p);
  assert(strstr(video_error(), "output"));
  failure[0] = 0;
  output_error = 0;
  input_error = 1;
  p.fed = 0;
  pump(&p);
  assert(strstr(video_error(), "input"));
  free(p.pixels);
  puts("Android video: delayed output, bounded catch-up, seek fences and "
       "errors pass");
}
