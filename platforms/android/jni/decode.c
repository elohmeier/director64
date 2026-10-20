#include "decode.h"
#include "opus_source.h"
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "director64", __VA_ARGS__)

sound_t *sound_retain(sound_t *s) {
  if (s)
    atomic_fetch_add(&s->references, 1);
  return s;
}

void sound_release(sound_t *s) {
  if (s && atomic_fetch_sub(&s->references, 1) == 1) {
    free(s->pcm);
    free(s);
  }
}

size_t sound_bytes(const sound_t *s) { return (size_t)s->frames * s->channels * 2; }

static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool is_wav(const uint8_t *data, size_t length) {
  return length >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4);
}

typedef struct {
  unsigned channels, rate, bits;
  const uint8_t *data;
  size_t bytes;
} wav_t;

static bool parse_wav(const uint8_t *bytes, size_t length, wav_t *wav) {
  if (!is_wav(bytes, length))
    return false;
  unsigned code = 0;
  memset(wav, 0, sizeof(*wav));
  for (size_t at = 12; length - at >= 8;) {
    uint32_t size = u32(bytes + at + 4);
    size_t body = at + 8;
    if (size > length - body)
      return false;
    if (!memcmp(bytes + at, "fmt ", 4) && size >= 16) {
      code = u16(bytes + body);
      wav->channels = u16(bytes + body + 2);
      wav->rate = u32(bytes + body + 4);
      wav->bits = u16(bytes + body + 14);
    } else if (!memcmp(bytes + at, "data", 4)) {
      wav->data = bytes + body;
      wav->bytes = size;
    }
    at = body + size;
    if ((size & 1) && at < length) at++;
  }
  return code == 1 && wav->data && (wav->bits == 8 || wav->bits == 16) &&
         wav->channels >= 1 && wav->channels <= 2 && wav->rate;
}

// An Ogg Opus file's length: the last page's granule position (48 kHz
// samples) less the header's pre-skip.
static double ogg_opus_duration(const uint8_t *data, size_t length) {
  if (length < 64 || memcmp(data, "OggS", 4) || memcmp(data + 28, "OpusHead", 8))
    return 0;
  unsigned pre_skip = data[38] | data[39] << 8;
  for (size_t at = length - 27; at > 0; at--)
    if (!memcmp(data + at, "OggS", 4)) {
      int64_t granule = 0;
      for (unsigned i = 0; i < 8; i++)
        granule |= (int64_t)data[at + 6 + i] << (8 * i);
      return granule > pre_skip ? (double)(granule - pre_skip) / 48000.0 : 0;
    }
  return 0;
}

double wav_duration(const uint8_t *data, size_t length) {
  uint32_t rate, frames;
  if (opus_source(data, length, &rate, &frames)) return (double)frames / rate;
  wav_t wav;
  if (!parse_wav(data, length, &wav))
    return ogg_opus_duration(data, length);
  return (double)(wav.bytes / (wav.bits / 8 * wav.channels)) / wav.rate;
}

static sound_t *new_sound(unsigned channels, unsigned rate, uint32_t frames);
sound_t *sound_source_bounds(sound_t *sound, const uint8_t *data, size_t length) {
  uint32_t rate, frames;
  if (!sound || !data || !opus_source(data, length, &rate, &frames)) return sound;
  if (rate == sound->rate && frames == sound->frames) return sound;
  // Restore source-rate samples so authored loops and Flash envelopes retain
  // their sample units. Platform Opus output normally runs at 48 kHz.
  sound_t *out = new_sound(sound->channels, rate, frames);
  if (out && sound->frames) {
    for (uint32_t i = 0; i < frames; i++) {
      double position = (double)i * sound->rate / rate;
      uint32_t left = position < sound->frames ? (uint32_t)position : sound->frames - 1;
      uint32_t right = left + 1 < sound->frames ? left + 1 : left;
      double fraction = position - left;
      if (fraction > 1) fraction = 1;
      for (unsigned c = 0; c < sound->channels; c++) {
        int a = sound->pcm[(size_t)left * sound->channels + c];
        int b = sound->pcm[(size_t)right * sound->channels + c];
        out->pcm[(size_t)i * sound->channels + c] = (int16_t)(a + (b - a) * fraction);
      }
    }
  }
  sound_release(sound);
  return out;
}

static sound_t *new_sound(unsigned channels, unsigned rate, uint32_t frames) {
  if (!channels || frames > SIZE_MAX / channels / 2) return NULL;
  sound_t *s = calloc(1, sizeof(sound_t));
  if (!s)
    return NULL;
  s->pcm = malloc((size_t)(frames ? frames : 1) * channels * 2);
  if (!s->pcm) {
    free(s);
    return NULL;
  }
  s->channels = channels;
  s->rate = rate;
  s->frames = frames;
  atomic_init(&s->references, 1);
  return s;
}

sound_t *decode_wav(const uint8_t *data, size_t length) {
  wav_t wav;
  if (!parse_wav(data, length, &wav))
    return NULL;
  unsigned stride = wav.bits / 8 * wav.channels;
  uint32_t frames = (uint32_t)(wav.bytes / stride);
  sound_t *s = new_sound(wav.channels, wav.rate, frames);
  if (!s)
    return NULL;
  size_t samples = (size_t)frames * wav.channels;
  if (wav.bits == 16) {
    for (size_t i = 0; i < samples; i++)
      s->pcm[i] = (int16_t)u16(wav.data + i * 2);
  } else {
    for (size_t i = 0; i < samples; i++)
      s->pcm[i] = (int16_t)(((int)wav.data[i] - 128) * 256);
  }
  return s;
}

// ---- MediaCodec ----

typedef struct {
  int16_t *pcm;
  size_t frames, cap;
} growing_t;

static bool append(growing_t *g, const int16_t *frames, size_t count, unsigned in_channels,
                   unsigned out_channels) {
  if (count > SIZE_MAX - g->frames || g->frames + count > UINT32_MAX ||
      g->frames + count > SIZE_MAX / out_channels / sizeof(*g->pcm)) return false;
  if (g->frames + count > g->cap) {
    size_t cap = g->cap ? g->cap : 65536;
    while (cap < g->frames + count) {
      if (cap > SIZE_MAX / 2 / out_channels / sizeof(*g->pcm)) {
        cap = g->frames + count;
        break;
      }
      cap *= 2;
    }
    int16_t *grown = realloc(g->pcm, cap * out_channels * 2);
    if (!grown)
      return false;
    g->pcm = grown;
    g->cap = cap;
  }
  for (size_t i = 0; i < count; i++)
    for (unsigned c = 0; c < out_channels; c++)
      g->pcm[(g->frames + i) * out_channels + c] = frames[i * in_channels + c];
  g->frames += count;
  return true;
}

sound_t *decode_media(int fd, uint64_t offset, uint64_t length, char *error,
                      size_t error_capacity) {
  AMediaExtractor *extractor = AMediaExtractor_new();
  AMediaCodec *codec = NULL;
  AMediaFormat *format = NULL;
  growing_t out = {0};
  int32_t channels = 0, rate = 0, encoding = 2;
  sound_t *result = NULL;
  const char *failure = "could not open audio media";
  bool completed = false;
  if (error_capacity) error[0] = 0;
  if (!extractor || AMediaExtractor_setDataSourceFd(extractor, fd, (off64_t)offset,
                                                    (off64_t)length) != AMEDIA_OK)
    goto done;
  for (size_t i = 0; i < AMediaExtractor_getTrackCount(extractor); i++) {
    AMediaFormat *f = AMediaExtractor_getTrackFormat(extractor, i);
    const char *mime = NULL;
    if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && !strncmp(mime, "audio/", 6)) {
      AMediaExtractor_selectTrack(extractor, i);
      codec = AMediaCodec_createDecoderByType(mime);
      format = f;
      break;
    }
    AMediaFormat_delete(f);
  }
  failure = "could not configure audio decoder";
  if (!codec || AMediaCodec_configure(codec, format, NULL, NULL, 0) != AMEDIA_OK ||
      AMediaCodec_start(codec) != AMEDIA_OK)
    goto done;
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
  bool input_done = false;
  for (unsigned idle = 0; idle < 400;) {
    if (!input_done) {
      ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 5000);
      if (in >= 0) {
        size_t cap = 0;
        uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)in, &cap);
        if (!buffer || !cap) { failure = "audio decoder supplied no input buffer"; goto done; }
        ssize_t size = AMediaExtractor_readSampleData(extractor, buffer, cap);
        media_status_t queued;
        if (size < 0) {
          queued = AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0,
                                                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          input_done = true;
        } else {
          queued = AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)size,
                                                (uint64_t)AMediaExtractor_getSampleTime(extractor), 0);
          AMediaExtractor_advance(extractor);
        }
        if (queued != AMEDIA_OK) { failure = "audio decoder rejected an input packet"; goto done; }
      } else if (in != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        failure = "audio decoder input failed"; goto done;
      }
    }
    AMediaCodecBufferInfo info;
    ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat *f = AMediaCodec_getOutputFormat(codec);
      AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
      AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
      AMediaFormat_getInt32(f, "pcm-encoding", &encoding);
      AMediaFormat_delete(f);
      continue;
    }
    if (index < 0) {
      if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
      if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        failure = "audio decoder output failed"; goto done;
      }
      idle++;
      continue;
    }
    idle = 0;
    size_t cap = 0;
    uint8_t *buffer = AMediaCodec_getOutputBuffer(codec, (size_t)index, &cap);
    bool valid = info.size >= 0 && info.offset >= 0 && (size_t)info.offset <= cap &&
                 (size_t)info.size <= cap - (size_t)info.offset;
    bool appended = true;
    if (info.size > 0 && (!valid || !buffer || channels <= 0 || rate <= 0 ||
                          (encoding != 2 && encoding != 4))) {
      AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
      failure = "audio decoder returned invalid PCM"; goto done;
    }
    if (info.size > 0) {
      unsigned keep = channels > 2 ? 2 : (unsigned)channels;
      if (encoding == 4) { // float
        size_t count = (size_t)info.size / 4 / (size_t)channels;
        const float *f = (const float *)(buffer + info.offset);
        int16_t converted[1024 * 2];
        for (size_t done = 0; done < count;) {
          size_t n = count - done > 1024 ? 1024 : count - done;
          for (size_t i = 0; i < n; i++)
            for (unsigned c = 0; c < keep; c++) {
              float v = f[(done + i) * (size_t)channels + c] * 32767.0f;
              converted[i * keep + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
          if (!append(&out, converted, n, keep, keep)) { appended = false; break; }
          done += n;
        }
      } else {
        size_t count = (size_t)info.size / 2 / (size_t)channels;
        appended = append(&out, (const int16_t *)(buffer + info.offset), count, (unsigned)channels, keep);
      }
    }
    AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
    if (!appended) { failure = "out of memory decoding complete audio"; goto done; }
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      completed = true;
      break;
    }
  }
  if (!completed) { failure = "audio decoder timed out before end of stream"; goto done; }
  failure = "audio decoder produced no sound";
  if (out.frames && rate > 0 && channels > 0) {
    result = calloc(1, sizeof(sound_t));
    if (!result) failure = "out of memory allocating decoded sound";
    if (result) {
      result->pcm = out.pcm;
      out.pcm = NULL;
      result->frames = (uint32_t)out.frames;
      result->channels = channels > 2 ? 2 : (unsigned)channels;
      result->rate = (unsigned)rate;
      atomic_init(&result->references, 1);
    }
  } else {
    LOG("media decode produced no sound");
  }
done:
  if (!result && error_capacity) snprintf(error, error_capacity, "%s", failure);
  free(out.pcm);
  if (codec) {
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
  }
  if (format)
    AMediaFormat_delete(format);
  if (extractor)
    AMediaExtractor_delete(extractor);
  return result;
}
