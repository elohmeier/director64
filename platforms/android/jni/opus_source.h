#ifndef DIRECTOR64_OPUS_SOURCE_H
#define DIRECTOR64_OPUS_SOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t opus_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline bool opus_number(const uint8_t *p, size_t n, const char *key, uint32_t *out) {
  size_t prefix = strlen(key);
  if (n <= prefix || memcmp(p, key, prefix)) return false;
  uint64_t value = 0;
  for (size_t i = prefix; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
    value = value * 10 + p[i] - '0';
    if (value > UINT32_MAX) return false;
  }
  *out = (uint32_t)value;
  return true;
}
// Converter tags preserve the source sample grid across Opus's resampling.
// FFmpeg writes this small comment packet in one page, before audio packets.
static inline bool opus_source(const uint8_t *data, size_t length,
                               uint32_t *rate, uint32_t *frames) {
  bool have_rate = false, have_frames = false;
  for (size_t at = 0; at + 27 <= length;) {
    if (memcmp(data + at, "OggS", 4)) return false;
    size_t body = at + 27 + data[at + 26], size = 0;
    if (body > length) return false;
    for (size_t i = at + 27; i < body; i++) size += data[i];
    if (size > length - body) return false;
    size_t end = body + size;
    if (size >= 16 && !memcmp(data + body, "OpusTags", 8)) {
      size_t p = body + 8;
      uint32_t vendor = opus_u32(data + p); p += 4;
      if (vendor > end - p) return false;
      p += vendor;
      if (end - p < 4) return false;
      uint32_t count = opus_u32(data + p); p += 4;
      for (uint32_t i = 0; i < count; i++) {
        if (end - p < 4) return false;
        uint32_t n = opus_u32(data + p); p += 4;
        if (n > end - p) return false;
        if (opus_number(data + p, n, "D64_SOURCE_RATE=", rate)) have_rate = true;
        if (opus_number(data + p, n, "D64_SOURCE_FRAMES=", frames)) have_frames = true;
        p += n;
      }
      return have_rate && have_frames && *rate > 0;
    }
    at = end;
  }
  return false;
}
#endif
