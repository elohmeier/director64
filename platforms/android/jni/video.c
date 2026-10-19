#include "video.h"
#include "json.h"
#include "mixer.h"
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...)                                                               \
  __android_log_print(ANDROID_LOG_INFO, "director64", __VA_ARGS__)

enum { PLAYERS = 8, IMAGES = 4, PUMP_BUFFERS = 8 };

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
  int64_t shown_at,
      floor_at; // Presented timestamp and the current seek boundary.
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
  uint32_t header =
      bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
  if (header > length - 4)
    return false;
  json_t json;
  if (!json_parse(&json, (const char *)bytes + 4, header))
    return false;
  char codec[32];
  json_string(&json, json_get(&json, 0, "codec"), codec, sizeof(codec));
  p->mime = !strcmp(codec, "vp8")        ? "video/x-vnd.on2.vp8"
            : !strncmp(codec, "avc1", 4) ? "video/avc"
            : !strncmp(codec, "vp09", 4) ? "video/x-vnd.on2.vp9"
                                         : NULL;
  p->width = (unsigned)json_number(&json, json_get(&json, 0, "width"));
  p->height = (unsigned)json_number(&json, json_get(&json, 0, "height"));
  unsigned frames = json_get(&json, 0, "frames");
  p->count = frames ? json.tokens[frames].size : 0;
  p->frames = calloc(p->count ? p->count : 1, sizeof(video_chunk_t));
  p->data = bytes + 4 + header;
  uint32_t data_length = length - 4 - header;
  bool ok = p->mime && p->frames && p->count;
  for (unsigned i = 0, at = frames + 1; ok && i < p->count;
       i++, at = json_next(&json, at)) {
    video_chunk_t *c = &p->frames[i];
    c->microseconds = (int64_t)json_number(&json, json_item(&json, at, 0));
    c->key = json_number(&json, json_item(&json, at, 1)) != 0;
    c->offset = (uint32_t)json_number(&json, json_item(&json, at, 2));
    c->size = (uint32_t)json_number(&json, json_item(&json, at, 3));
    ok = c->size && c->offset <= data_length &&
         c->size <= data_length - c->offset && c->microseconds >= 0 &&
         (!i || c->microseconds > p->frames[i - 1].microseconds);
  }
  json_free(&json);
  return ok;
}

static bool configure(player_t *p) {
  // An H.264 header may leave the size at 0; the first frame sets it.
  int32_t width = p->width ? (int32_t)p->width : 320,
          height = p->height ? (int32_t)p->height : 240;
  if (AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, IMAGES,
                       &p->reader) != AMEDIA_OK)
    return false;
  ANativeWindow *window = NULL;
  if (AImageReader_getWindow(p->reader, &window) != AMEDIA_OK || !window)
    return false;
  p->codec = AMediaCodec_createDecoderByType(p->mime);
  if (!p->codec)
    return false;
  AMediaFormat *format = AMediaFormat_new();
  if (!format)
    return false;
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, p->mime);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  bool ok =
      AMediaCodec_configure(p->codec, format, window, NULL, 0) == AMEDIA_OK &&
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
static inline uint8_t clamp8(int v) {
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static bool convert(player_t *p, AImage *image) {
  int32_t width = 0, height = 0;
  if (AImage_getWidth(image, &width) != AMEDIA_OK ||
      AImage_getHeight(image, &height) != AMEDIA_OK)
    return false;
  AImageCropRect crop = {0, 0, width, height};
  if (AImage_getCropRect(image, &crop) != AMEDIA_OK || crop.left < 0 ||
      crop.top < 0 || crop.right > width || crop.bottom > height)
    return false;
  width = crop.right - crop.left;
  height = crop.bottom - crop.top;
  if (width <= 0 || height <= 0 ||
      (size_t)width > SIZE_MAX / 4 / (size_t)height)
    return false;
  if ((unsigned)width != p->width || (unsigned)height != p->height ||
      !p->pixels) {
    free(p->pixels);
    p->width = (unsigned)width;
    p->height = (unsigned)height;
    p->pixels = malloc((size_t)width * (size_t)height * 4);
    if (!p->pixels)
      return false;
  }
  uint8_t *planes[3];
  int lengths[3], rows[3], pixels[3];
  for (int i = 0; i < 3; i++) {
    if (AImage_getPlaneData(image, i, &planes[i], &lengths[i]) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, i, &rows[i]) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, i, &pixels[i]) != AMEDIA_OK ||
        !planes[i] || lengths[i] <= 0 || rows[i] <= 0 || pixels[i] <= 0)
      return false;
    unsigned divisor = i ? 2 : 1;
    uint64_t last =
        (uint64_t)((unsigned)(crop.bottom - 1) / divisor) * (unsigned)rows[i] +
        (uint64_t)((unsigned)(crop.right - 1) / divisor) * (unsigned)pixels[i];
    if (last >= (unsigned)lengths[i])
      return false;
  }
  for (int y = 0; y < height; y++) {
    const uint8_t *yrow =
        planes[0] + (crop.top + y) * rows[0] + crop.left * pixels[0];
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
  return true;
}

// Decode bounded work per service opportunity. A slow decoder still presents
// its newest completed frame; requiring an exact moving target starves video.
static void pump(player_t *p) {
  if (!p || !p->codec || p->wanted < 0 || *failure)
    return;
  for (unsigned n = 0; n < PUMP_BUFFERS && p->fed < p->wanted; n++) {
    ssize_t in = AMediaCodec_dequeueInputBuffer(p->codec, 0);
    if (in == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
      break;
    if (in < 0) {
      snprintf(failure, sizeof(failure), "Video decoder input failed (%zd)",
               in);
      return;
    }
    size_t cap = 0;
    uint8_t *buffer = AMediaCodec_getInputBuffer(p->codec, (size_t)in, &cap);
    const video_chunk_t *c = &p->frames[p->fed + 1];
    if (!buffer || c->size > cap) {
      snprintf(failure, sizeof(failure),
               "Video frame exceeds decoder input capacity");
      return;
    }
    memcpy(buffer, p->data + c->offset, c->size);
    if (AMediaCodec_queueInputBuffer(p->codec, (size_t)in, 0, c->size,
                                     (uint64_t)c->microseconds,
                                     0) != AMEDIA_OK) {
      snprintf(failure, sizeof(failure),
               "Video decoder rejected an encoded frame");
      return;
    }
    p->fed++;
  }
  ssize_t newest = -1;
  int64_t newest_at = p->shown_at;
  for (unsigned n = 0; n < PUMP_BUFFERS; n++) {
    AMediaCodecBufferInfo info;
    ssize_t out = AMediaCodec_dequeueOutputBuffer(p->codec, &info, 0);
    if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
        out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
      continue;
    if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
      break;
    if (out < 0) {
      snprintf(failure, sizeof(failure), "Video decoder output failed (%zd)",
               out);
      break;
    }
    bool eligible =
        info.presentationTimeUs >= p->floor_at &&
        info.presentationTimeUs <= p->frames[p->wanted].microseconds &&
        info.presentationTimeUs > newest_at;
    ssize_t discard = out;
    if (eligible) {
      discard = newest;
      newest = out;
      newest_at = info.presentationTimeUs;
    }
    if (discard >= 0 && AMediaCodec_releaseOutputBuffer(
                            p->codec, (size_t)discard, false) != AMEDIA_OK)
      snprintf(failure, sizeof(failure),
               "Video decoder could not release an output frame");
  }
  if (newest >= 0 && AMediaCodec_releaseOutputBuffer(p->codec, (size_t)newest,
                                                     !*failure) != AMEDIA_OK)
    snprintf(failure, sizeof(failure),
             "Video decoder could not present an output frame");
  if (*failure)
    return;
  AImage *image = NULL;
  media_status_t acquired = AImageReader_acquireLatestImage(p->reader, &image);
  if (acquired == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
    return;
  if (acquired != AMEDIA_OK || !image) {
    snprintf(failure, sizeof(failure), "Video image reader failed (%d)",
             acquired);
    return;
  }
  int64_t timestamp = -1;
  if (AImage_getTimestamp(image, &timestamp) != AMEDIA_OK) {
    snprintf(failure, sizeof(failure),
             "Video image has no presentation timestamp");
  } else {
    // Surface images can arrive after a flush. Their timestamps must belong
    // to this seek, and must never move the displayed picture backwards.
    int64_t at = timestamp / 1000;
    if (at >= p->floor_at && at <= p->frames[p->wanted].microseconds &&
        at > p->shown_at) {
      if (convert(p, image)) {
        p->shown_at = at;
        revision++;
      } else
        snprintf(failure, sizeof(failure),
                 "Invalid video image planes or allocation failure");
    }
  }
  AImage_delete(image);
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

static void seek(player_t *p, double time600, bool discontinuity) {
  int target = frame_at(p, (int64_t)(time600 * 1e6 / 600 + 0.5));
  if (target >= 0 && (target != p->wanted || discontinuity)) {
    int previous = p->wanted;
    p->wanted = target;
    // Far jumps and rewinds restart from the keyframe before the target;
    // near ones decode forward from where the decoder is.
    if (discontinuity || target < previous || target > previous + 30) {
      int key = target;
      while (key > 0 && !p->frames[key].key)
        key--;
      if (AMediaCodec_flush(p->codec) != AMEDIA_OK) {
        snprintf(failure, sizeof(failure), "Video decoder seek failed");
        return;
      }
      p->fed = key - 1;
      p->floor_at = p->frames[target].microseconds;
      p->shown_at = -1;
      free(p->pixels);
      p->pixels = NULL;
      revision++;
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

void video_init(const pack_t *videos) {
  pack = videos;
  failure[0] = 0;
}

void video_update(unsigned sprite, const char *asset, const char *sound,
                  double time600, double rate, unsigned serial, float gain) {
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
      snprintf(failure, sizeof(failure),
               "Concurrent video player capacity exceeded (%u)", PLAYERS);
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
        snprintf(failure, sizeof(failure), "Video %s could not be decoded",
                 asset);
      }
    } else if (*asset) {
      LOG("missing video %s", asset);
      snprintf(failure, sizeof(failure), "Missing video asset %s", asset);
    }
  }
  if (e->player)
    seek(e->player, time600, e->serial != serial);
  // The movie's sound: started (or re-seeked) when it plays and the time
  // jumped, stopped when it stops, pitched by the rate.
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
