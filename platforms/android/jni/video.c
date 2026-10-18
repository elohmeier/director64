#include "video.h"
#include "json.h"
#include "mixer.h"
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "director64", __VA_ARGS__)

enum { PLAYERS = 8, IMAGES = 4 };

typedef struct {
  int64_t microseconds;
  bool key;
  uint32_t offset, size;
} video_chunk_t;

typedef struct {
  unsigned width, height;
  const uint8_t *data;
  video_chunk_t *frames;
  unsigned count;
  AMediaCodec *codec;
  AImageReader *reader;
  const char *mime;
  int fed, wanted; // the last chunk given to the decoder; the frame wanted
  int64_t shown_at; // the wanted frame's time, once the decoder has it
  bool awaiting; // the wanted frame went to the reader; not yet acquired
  uint32_t *pixels;
} player_t;

typedef struct {
  unsigned sprite;
  player_t *player;
  char sound[64];
  unsigned serial;
  bool open, playing;
} entry_t;

static const pack_t *pack;
static entry_t entries[PLAYERS];
static unsigned revision;
static char failure[256];
const char *video_error(void) { return failure; }

// The importer's container: a u32 header length, a JSON header {codec,
// width, height, frames: [[microseconds, key, offset, size]]}, the chunks.
static bool parse(player_t *p, const uint8_t *bytes, uint32_t length) {
  if (length < 4)
    return false;
  uint32_t header = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
  if (header > length - 4)
    return false;
  json_t json;
  if (!json_parse(&json, (const char *)bytes + 4, header))
    return false;
  char codec[32];
  json_string(&json, json_get(&json, 0, "codec"), codec, sizeof(codec));
  p->mime = !strcmp(codec, "vp8")              ? "video/x-vnd.on2.vp8"
            : !strncmp(codec, "avc1", 4)       ? "video/avc"
            : !strncmp(codec, "vp09", 4)       ? "video/x-vnd.on2.vp9"
                                               : NULL;
  p->width = (unsigned)json_number(&json, json_get(&json, 0, "width"));
  p->height = (unsigned)json_number(&json, json_get(&json, 0, "height"));
  unsigned frames = json_get(&json, 0, "frames");
  p->count = frames ? json.tokens[frames].size : 0;
  p->frames = calloc(p->count ? p->count : 1, sizeof(video_chunk_t));
  p->data = bytes + 4 + header;
  uint32_t data_length = length - 4 - header;
  bool ok = p->mime && p->frames && p->count;
  for (unsigned i = 0, at = frames + 1; ok && i < p->count; i++, at = json_next(&json, at)) {
    video_chunk_t *c = &p->frames[i];
    c->microseconds = (int64_t)json_number(&json, json_item(&json, at, 0));
    c->key = json_number(&json, json_item(&json, at, 1)) != 0;
    c->offset = (uint32_t)json_number(&json, json_item(&json, at, 2));
    c->size = (uint32_t)json_number(&json, json_item(&json, at, 3));
    ok = c->offset <= data_length && c->size <= data_length - c->offset;
  }
  json_free(&json);
  return ok;
}

static bool configure(player_t *p) {
  // A console stream's header leaves the size to its first frame.
  int32_t width = p->width ? (int32_t)p->width : 320, height = p->height ? (int32_t)p->height : 240;
  if (AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, IMAGES, &p->reader) !=
      AMEDIA_OK)
    return false;
  ANativeWindow *window = NULL;
  AImageReader_getWindow(p->reader, &window);
  p->codec = AMediaCodec_createDecoderByType(p->mime);
  if (!p->codec)
    return false;
  AMediaFormat *format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, p->mime);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  bool ok = AMediaCodec_configure(p->codec, format, window, NULL, 0) == AMEDIA_OK &&
            AMediaCodec_start(p->codec) == AMEDIA_OK;
  AMediaFormat_delete(format);
  return ok;
}

static void destroy(player_t *p) {
  if (!p)
    return;
  if (p->codec) {
    AMediaCodec_stop(p->codec);
    AMediaCodec_delete(p->codec);
  }
  if (p->reader)
    AImageReader_delete(p->reader);
  free(p->frames);
  free(p->pixels);
  free(p);
}

static player_t *create(const uint8_t *bytes, uint32_t length) {
  player_t *p = calloc(1, sizeof(player_t));
  if (!p)
    return NULL;
  p->fed = p->wanted = -1;
  p->shown_at = -1;
  if (!parse(p, bytes, length) || !configure(p)) {
    destroy(p);
    return NULL;
  }
  return p;
}

// BT.601 limited range, as browsers convert VP8 and H.264.
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void convert(player_t *p, AImage *image) {
  int32_t width = 0, height = 0;
  AImage_getWidth(image, &width);
  AImage_getHeight(image, &height);
  AImageCropRect crop = {0, 0, width, height};
  AImage_getCropRect(image, &crop);
  width = crop.right - crop.left;
  height = crop.bottom - crop.top;
  if (width <= 0 || height <= 0)
    return;
  if ((unsigned)width != p->width || (unsigned)height != p->height || !p->pixels) {
    free(p->pixels);
    p->width = (unsigned)width;
    p->height = (unsigned)height;
    p->pixels = malloc((size_t)width * (size_t)height * 4);
    if (!p->pixels)
      return;
  }
  uint8_t *planes[3];
  int lengths[3], rows[3], pixels[3];
  for (int i = 0; i < 3; i++) {
    AImage_getPlaneData(image, i, &planes[i], &lengths[i]);
    AImage_getPlaneRowStride(image, i, &rows[i]);
    AImage_getPlanePixelStride(image, i, &pixels[i]);
  }
  for (int y = 0; y < height; y++) {
    const uint8_t *yrow = planes[0] + (crop.top + y) * rows[0] + crop.left * pixels[0];
    int cy = (crop.top + y) / 2;
    const uint8_t *urow = planes[1] + cy * rows[1];
    const uint8_t *vrow = planes[2] + cy * rows[2];
    uint32_t *out = p->pixels + (size_t)y * (size_t)width;
    for (int x = 0; x < width; x++) {
      int cx = (crop.left + x) / 2;
      int c = (yrow[x * pixels[0]] - 16) * 298;
      int d = urow[cx * pixels[1]] - 128, e = vrow[cx * pixels[2]] - 128;
      unsigned r = clamp8((c + 409 * e + 128) >> 8);
      unsigned g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
      unsigned b = clamp8((c + 516 * d + 128) >> 8);
      out[x] = r | g << 8 | b << 16 | 0xffu << 24;
    }
  }
}

// Feeds the decoder up to the wanted frame and keeps what it has decoded.
static void pump(player_t *p) {
  if (!p || !p->codec)
    return;
  while (p->fed < p->wanted) {
    ssize_t in = AMediaCodec_dequeueInputBuffer(p->codec, 0);
    if (in < 0)
      break;
    size_t cap = 0;
    uint8_t *buffer = AMediaCodec_getInputBuffer(p->codec, (size_t)in, &cap);
    const video_chunk_t *c = &p->frames[++p->fed];
    if (!buffer || c->size > cap) {
      snprintf(failure, sizeof(failure), "Video frame exceeds decoder input capacity");
      return;
    }
    memcpy(buffer, p->data + c->offset, c->size);
    if (AMediaCodec_queueInputBuffer(p->codec, (size_t)in, 0, c->size,
                                    (uint64_t)c->microseconds, 0) != AMEDIA_OK) {
      snprintf(failure, sizeof(failure), "Video decoder rejected an encoded frame");
      return;
    }
  }
  for (;;) {
    AMediaCodecBufferInfo info;
    ssize_t out = AMediaCodec_dequeueOutputBuffer(p->codec, &info, 0);
    if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
      continue;
    if (out < 0)
      break;
    // Only the frame the runtime wants is shown; frames decoded on the way
    // there are dropped unread.
    bool wanted = p->wanted >= 0 && info.presentationTimeUs == p->frames[p->wanted].microseconds;
    AMediaCodec_releaseOutputBuffer(p->codec, (size_t)out, wanted);
    if (wanted)
      p->awaiting = true;
  }
  if (p->awaiting) {
    AImage *image = NULL;
    if (AImageReader_acquireLatestImage(p->reader, &image) == AMEDIA_OK && image) {
      convert(p, image);
      AImage_delete(image);
      p->awaiting = false;
      revision++;
    }
  }
}

// The last frame at or before `microseconds`.
static int frame_at(const player_t *p, int64_t microseconds) {
  int lo = 0, hi = (int)p->count - 1, found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (p->frames[mid].microseconds <= microseconds) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return found;
}

static void seek(player_t *p, double time600) {
  int target = frame_at(p, (int64_t)(time600 * 1e6 / 600 + 0.5));
  if (target >= 0 && target != p->wanted) {
    p->wanted = target;
    // Far jumps and rewinds restart from the keyframe before the target;
    // near ones decode forward from where the decoder is.
    if (target < p->fed || target > p->fed + 30) {
      int key = target;
      while (key > 0 && !p->frames[key].key)
        key--;
      AMediaCodec_flush(p->codec);
      p->fed = key - 1;
      p->awaiting = false;
    }
  }
  pump(p);
}

static entry_t *find(unsigned sprite) {
  for (unsigned i = 0; i < PLAYERS; i++)
    if (entries[i].open && entries[i].sprite == sprite)
      return &entries[i];
  return NULL;
}

static void close_entry(entry_t *e) {
  destroy(e->player);
  if (e->sound[0])
    audio_stop_video(e->sprite);
  memset(e, 0, sizeof(*e));
  revision++;
}

void video_init(const pack_t *videos) { pack = videos; failure[0] = 0; }

void video_update(unsigned sprite, const char *asset, const char *sound, double time600,
                  double rate, unsigned serial, float gain) {
  entry_t *e = find(sprite);
  if (!asset) {
    if (e)
      close_entry(e);
    return;
  }
  if (!e) {
    for (unsigned i = 0; i < PLAYERS && !e; i++)
      if (!entries[i].open)
        e = &entries[i];
    if (!e) {
      snprintf(failure, sizeof(failure), "Concurrent video player capacity exceeded (%u)", PLAYERS);
      return;
    }
    memset(e, 0, sizeof(*e));
    e->open = true;
    e->sprite = sprite;
    e->serial = UINT32_MAX;
    snprintf(e->sound, sizeof(e->sound), "%s", sound ? sound : "");
    const pack_entry_t *entry = *asset ? pack_find(pack, asset) : NULL;
    if (entry) {
      e->player = create(pack_data(pack, entry), entry->length);
      if (!e->player) {
        LOG("video %s could not be played", asset);
        snprintf(failure, sizeof(failure), "Video %s could not be decoded", asset);
      }
    } else if (*asset) {
      LOG("missing video %s", asset);
      snprintf(failure, sizeof(failure), "Missing video asset %s", asset);
    }
  }
  if (e->player)
    seek(e->player, time600);
  // The movie's sound: started (or re-seeked) when it plays and the time
  // jumped, stopped when it stops, pitched by the rate, as the console's.
  if (e->sound[0]) {
    bool play = rate > 0 && time600 / 600 < audio_duration(e->sound);
    if (play && (!e->playing || e->serial != serial))
      audio_play_video(sprite, e->sound, time600 / 600);
    else if (!play && e->playing)
      audio_stop_video(sprite);
    if (play)
      audio_video_rate(sprite, rate, gain);
    e->playing = play;
  }
  e->serial = serial;
}

bool video_frame(unsigned sprite, uint32_t *out, uint32_t cap, unsigned *width,
                 unsigned *height) {
  entry_t *e = find(sprite);
  player_t *p = e ? e->player : NULL;
  if (!p || !p->pixels || p->width * p->height > cap)
    return false;
  memcpy(out, p->pixels, (size_t)p->width * p->height * 4);
  *width = p->width;
  *height = p->height;
  return true;
}

unsigned video_revision(void) {
  for (unsigned i = 0; i < PLAYERS; i++)
    if (entries[i].open)
      pump(entries[i].player);
  return revision;
}

void video_close_all(void) {
  for (unsigned i = 0; i < PLAYERS; i++)
    if (entries[i].open)
      close_entry(&entries[i]);
}
