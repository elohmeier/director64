// AVM strings count UTF-16 code units, while shared Lingo storage is UTF-8.
// Preserve lone surrogates as WTF-8 so charAt/substring can split a pair and
// charCodeAt can still observe the exact 16-bit value on a later operation.
#include "avm1_case.h"
static uint16_t *an_units(dg_avm1_library_t *l, lv_t value, unsigned *count) {
  lv_runtime_t *r = an_values(l);
  lv_t text = dg_avm1_string(l->vm, value);
  const char *p = lv_cstr(r, text);
  unsigned bytes = lv_type(text) == LV_STRING
                       ? r->objects[lv_id(text)].count - 1
                       : (unsigned)strlen(p);
  if (bytes > 1024 * 1024) {
    lv_fail(r, "AVM1 string exceeds 1 MiB");
    return NULL;
  }
  uint16_t *units = malloc(((size_t)bytes + 1) * sizeof(*units));
  if (!units) {
    lv_fail(r, "AVM1 string allocation failed");
    return NULL;
  }
  const char *end = p + bytes;
  unsigned n = 0;
  while (p < end) {
    unsigned cp = dg_utf8_next(&p);
    if (cp > 0xffff && cp <= 0x10ffff) {
      units[n++] = (uint16_t)(0xd800 + ((cp - 0x10000) >> 10));
      units[n++] = (uint16_t)(0xdc00 + ((cp - 0x10000) & 1023));
    } else
      units[n++] = (uint16_t)cp;
  }
  *count = n;
  return units;
}
static lv_t an_units_text(dg_avm1_library_t *l, const uint16_t *units,
                          unsigned count) {
  char *text = malloc((size_t)count * 3 + 1);
  if (!text) {
    lv_fail(an_values(l), "AVM1 string allocation failed");
    return 0;
  }
  unsigned at = 0;
  for (unsigned i = 0; i < count; i++) {
    unsigned cp = units[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < count &&
        units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (units[++i] - 0xdc00);
    }
    if (cp < 128)
      text[at++] = (char)cp;
    else if (cp < 2048) {
      text[at++] = (char)(0xc0 | (cp >> 6));
      text[at++] = (char)(0x80 | (cp & 63));
    } else if (cp < 65536) {
      text[at++] = (char)(0xe0 | (cp >> 12));
      text[at++] = (char)(0x80 | ((cp >> 6) & 63));
      text[at++] = (char)(0x80 | (cp & 63));
    } else {
      text[at++] = (char)(0xf0 | (cp >> 18));
      text[at++] = (char)(0x80 | ((cp >> 12) & 63));
      text[at++] = (char)(0x80 | ((cp >> 6) & 63));
      text[at++] = (char)(0x80 | (cp & 63));
    }
  }
  lv_t result = lv_bytes(an_values(l), text, at);
  free(text);
  return result;
}
// Original Flash Player 32.0.0.465, SWF6/7: simple Unicode mappings.
// In particular sharp-s/ligatures do not expand, and sigma is context-free.
static unsigned an_case_map(unsigned cp, bool lower) {
  const an_case_range_t *ranges = lower ? an_case_lower : an_case_upper;
  unsigned n = lower ? sizeof(an_case_lower) / sizeof(*ranges)
                     : sizeof(an_case_upper) / sizeof(*ranges);
  for (unsigned i = 0; i < n && ranges[i].first <= cp; i++)
    if (cp <= ranges[i].last && (cp - ranges[i].first) % ranges[i].step == 0)
      return (uint32_t)((int64_t)cp + ranges[i].delta);
  return cp;
}
static lv_t an_units_case(dg_avm1_library_t *l, const uint16_t *units,
                          unsigned count, bool lower) {
  uint16_t *result = malloc(((size_t)count * 2 + 1) * sizeof(*result));
  if (!result) {
    lv_fail(an_values(l), "AVM1 string allocation failed");
    return 0;
  }
  unsigned used = 0;
  for (unsigned i = 0; i < count; i++) {
    unsigned cp = units[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < count &&
        units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
      cp = 0x10000 + ((cp - 0xd800) << 10) + (units[++i] - 0xdc00);
    cp = an_case_map(cp, lower);
    if (cp > 65535) {
      result[used++] = (uint16_t)(0xd800 + ((cp - 0x10000) >> 10));
      result[used++] = (uint16_t)(0xdc00 + ((cp - 0x10000) & 1023));
    } else
      result[used++] = (uint16_t)cp;
  }
  lv_t text = an_units_text(l, result, used);
  free(result);
  return text;
}
