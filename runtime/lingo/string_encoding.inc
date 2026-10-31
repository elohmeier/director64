// Western Director strings expose byte-valued numToChar/FileIO operations.
// Recovered package text is UTF-8, so character operations must decode that
// transport without changing authored byte algorithms.
static bool text_is_raw(lv_runtime_t *r, lv_t value) {
  lv_object_t *o = lv_type(value) == LV_STRING ? object(r, value) : NULL;
  return o && o->geometry == 1;
}
unsigned lv_byte_count(lv_runtime_t *r, lv_t value) {
  return string_length(r, value);
}
static const uint16_t text_cp1252[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
    0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
    0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
static unsigned text_byte_scalar(unsigned c) {
  return c >= 128 && c < 160 ? text_cp1252[c - 128] : c;
}
static bool text_scalar_byte(unsigned c, unsigned char *out) {
  if (c < 128 || (c >= 160 && c <= 255)) {
    *out = (unsigned char)c;
    return true;
  }
  for (unsigned i = 0; i < 32; i++)
    if (text_cp1252[i] == c) {
      *out = (unsigned char)(128 + i);
      return true;
    }
  return false;
}
// Invalid UTF-8 bytes remain one legacy character, never an out-of-bounds
// continuation read. Embedded zero is a character when the string has one.
static unsigned text_decode(const unsigned char *s, size_t n, size_t *step) {
  unsigned c = s[0], need = c >= 0xc2 && c <= 0xdf   ? 2
                            : c >= 0xe0 && c <= 0xef ? 3
                            : c >= 0xf0 && c <= 0xf4 ? 4
                                                     : 1;
  *step = 1;
  if (need == 1 || need > n)
    return text_byte_scalar(c);
  unsigned cp = c & ((1u << (7 - need)) - 1);
  for (unsigned i = 1; i < need; i++) {
    if ((s[i] & 0xc0) != 0x80)
      return text_byte_scalar(c);
    cp = (cp << 6) | (s[i] & 63);
  }
  if ((need == 2 && cp < 128) || (need == 3 && cp < 2048) ||
      (need == 4 && cp < 65536) || cp > 0x10ffff ||
      (cp >= 0xd800 && cp <= 0xdfff))
    return text_byte_scalar(c);
  *step = need;
  return cp;
}
static unsigned text_next(lv_runtime_t *r, lv_t value, size_t at,
                          size_t *step) {
  const unsigned char *s = (const unsigned char *)lv_cstr(r, value);
  if (text_is_raw(r, value)) {
    *step = 1;
    return text_byte_scalar(s[at]);
  }
  return text_decode(s + at, string_length(r, value) - at, step);
}
static unsigned text_char_count(lv_runtime_t *r, lv_t value) {
  unsigned count = 0;
  size_t n = string_length(r, value);
  if (text_is_raw(r, value))
    return (unsigned)n;
  for (size_t i = 0, step; i < n; i += step) {
    (void)text_next(r, value, i, &step);
    count++;
  }
  return count;
}
static size_t text_char_byte(lv_runtime_t *r, lv_t value, unsigned count) {
  size_t i = 0, n = string_length(r, value), step;
  if (text_is_raw(r, value))
    return count < n ? count : n;
  while (i < n && count--) {
    (void)text_next(r, value, i, &step);
    i += step;
  }
  return i;
}
static unsigned text_encode(unsigned c, unsigned char *out) {
  if (c < 128) {
    out[0] = (unsigned char)c;
    return 1;
  }
  if (c < 2048) {
    out[0] = (unsigned char)(0xc0 | (c >> 6));
    out[1] = (unsigned char)(0x80 | (c & 63));
    return 2;
  }
  if (c < 65536) {
    out[0] = (unsigned char)(0xe0 | (c >> 12));
    out[1] = (unsigned char)(0x80 | ((c >> 6) & 63));
    out[2] = (unsigned char)(0x80 | (c & 63));
    return 3;
  }
  out[0] = (unsigned char)(0xf0 | (c >> 18));
  out[1] = (unsigned char)(0x80 | ((c >> 12) & 63));
  out[2] = (unsigned char)(0x80 | ((c >> 6) & 63));
  out[3] = (unsigned char)(0x80 | (c & 63));
  return 4;
}
lv_t lv_text_utf8(lv_runtime_t *r, lv_t value) {
  if (!text_is_raw(r, value))
    return value;
  unsigned n = string_length(r, value);
  size_t length = 0;
  const unsigned char *s = (const unsigned char *)lv_cstr(r, value);
  for (unsigned i = 0; i < n; i++) {
    unsigned char bytes[4];
    length += text_encode(text_byte_scalar(s[i]), bytes);
  }
  if (length >= LV_HEAP_BYTES) {
    lv_fail(r, "decoded text exceeds heap");
    return 0;
  }
  lv_t result = allocate(r, LV_STRING, (unsigned)length + 1);
  if (r->failed)
    return 0;
  s = (const unsigned char *)lv_cstr(r, value);
  unsigned char *out = r->heap + r->objects[lv_id(result)].offset;
  size_t at = 0;
  for (unsigned i = 0; i < n; i++)
    at += text_encode(text_byte_scalar(s[i]), out + at);
  out[at] = 0;
  return result;
}
static lv_t text_legacy_bytes(lv_runtime_t *r, lv_t value) {
  if (text_is_raw(r, value))
    return value;
  unsigned count = text_char_count(r, value);
  lv_t result = allocate(r, LV_STRING, count + 1);
  if (r->failed)
    return 0;
  unsigned char *out = r->heap + r->objects[lv_id(result)].offset;
  size_t at = 0, step, n = string_length(r, value);
  for (unsigned i = 0; at < n; i++, at += step)
    if (!text_scalar_byte(text_next(r, value, at, &step), out + i)) {
      lv_script_fail(
          r, "character cannot be represented in Western Director byte string");
      return 0;
    }
  out[count] = 0;
  r->objects[lv_id(result)].geometry = 1;
  return result;
}
static unsigned text_fold(unsigned c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
    return c + 32;
  if (c == 0x152)
    return 0x153;
  if (c == 0x160)
    return 0x161;
  if (c == 0x17d)
    return 0x17e;
  if (c == 0x178)
    return 0xff;
  return c;
}
static int text_compare(lv_runtime_t *r, lv_t a, lv_t b) {
  size_t x = 0, y = 0, na = string_length(r, a), nb = string_length(r, b), sx,
         sy;
  while (x < na && y < nb) {
    unsigned ca = text_fold(text_next(r, a, x, &sx)),
             cb = text_fold(text_next(r, b, y, &sy));
    if (ca != cb)
      return ca < cb ? -1 : 1;
    x += sx;
    y += sy;
  }
  return (x < na) - (y < nb);
}
static unsigned text_find(lv_runtime_t *r, lv_t haystack, lv_t needle,
                          bool first_only) {
  size_t n = string_length(r, haystack), m = string_length(r, needle), step;
  unsigned index = 1;
  if (!m)
    return 0;
  for (size_t start = 0; start < n; start += step, index++) {
    size_t a = start, b = 0, sa, sb;
    while (a < n && b < m) {
      if (text_fold(text_next(r, haystack, a, &sa)) !=
          text_fold(text_next(r, needle, b, &sb)))
        break;
      a += sa;
      b += sb;
    }
    if (b == m)
      return index;
    if (first_only)
      break;
    (void)text_next(r, haystack, start, &step);
  }
  return 0;
}
