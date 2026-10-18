// AVM1 execution from Adobe's SWF19 action-record specification. Values use
// the Lingo heap, while object lookup and coercion follow ActionScript rules.
#include "avm1.h"
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>

#define AVM_FRAMES 32u
#define AVM_STACK 512u
#define AVM_ROOTS 4096u
#define AVM_FUNCTIONS 512u
#define AVM_POOLS 64u
#define AVM_ARGUMENTS 64u
#define AVM_FRAME_ROOT 514u
#define AVM_REGISTER_ROOT (AVM_FRAME_ROOT + AVM_FRAMES * 4u)
enum { AVM_GLOBAL, AVM_RESULT };
enum { AVM_SCOPE, AVM_THIS, AVM_FUNCTION, AVM_TARGET };
enum {
  AVM_PROTO = -1,
  AVM_CODE = -2,
  AVM_CLOSURE = -3,
  AVM_DEF_TARGET = -4,
  AVM_FLAGS = -5,
  AVM_OWNER = -6,
  AVM_SUPER_RECEIVER = -7,
  AVM_CONSTRUCTOR = -8,
  AVM_SUPER_CONSTRUCTOR = -9
};
typedef struct {
  const uint8_t *code, *parameters;
  unsigned length, argument_count, parameter_length, registers, flags, pool;
  bool version2;
  char *native;
} avm_function_t;
typedef struct {
  const uint8_t *source;
  const char **strings;
  unsigned count;
} avm_pool_t;
typedef struct {
  const uint8_t *code;
  unsigned length, pc, stack_base, register_base, register_count, pool;
  bool result, construct;
} avm_frame_t;
struct dg_avm1 {
  lv_runtime_t *r;
  dg_avm1_host_t host;
  void *context;
  uint32_t identity;
  unsigned version, depth, stack_top, register_top, function_count, pool_count,
      native_depth;
  lv_root_span_t span;
  lv_t roots[AVM_ROOTS];
  avm_frame_t frames[AVM_FRAMES];
  avm_function_t functions[AVM_FUNCTIONS];
  avm_pool_t pools[AVM_POOLS];
  lv_root_span_t *instruction_roots;
};
static dg_avm1_status_t avm_run_until(dg_avm1_t *, unsigned, unsigned);
static bool avm_call(dg_avm1_t *, lv_t, lv_t, lv_t, unsigned, const lv_t *,
                     bool, bool);
static lv_t avm_source_text(dg_avm1_t *v, const char *s) {
  // Supported projectors run on Windows. SWF6 introduced UTF-8; older
  // action strings use the projector's Windows-1252 code page.
  if (v->version >= 6)
    return lv_text(v->r, s, false);
  static const uint16_t control[] = {
      0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
      0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
      0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
      0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
  size_t n = strlen(s);
  if (n > 65536) {
    lv_fail(v->r, "AVM1 source string exceeds capacity");
    return 0;
  }
  char *text = malloc(n * 3 + 1);
  if (!text) {
    lv_fail(v->r, "AVM1 source string allocation");
    return 0;
  }
  unsigned at = 0;
  while (*s) {
    unsigned c = (unsigned char)*s++;
    if (c >= 128 && c < 160)
      c = control[c - 128];
    if (c < 128)
      text[at++] = (char)c;
    else if (c < 2048) {
      text[at++] = (char)(0xc0 | (c >> 6));
      text[at++] = (char)(0x80 | (c & 63));
    } else {
      text[at++] = (char)(0xe0 | (c >> 12));
      text[at++] = (char)(0x80 | ((c >> 6) & 63));
      text[at++] = (char)(0x80 | (c & 63));
    }
  }
  text[at] = 0;
  lv_t result = lv_text(v->r, text, false);
  free(text);
  return result;
}
static lv_t avm_internal(dg_avm1_t *v, lv_t object, int key) {
  if (lv_type(object) != LV_PROPLIST)
    return 0;
  for (unsigned i = 1; i <= lv_count(v->r, object); i++) {
    lv_t k = lv_at(v->r, object, i * 2 - 1);
    if (lv_type(k) == LV_NUMBER && lv_numeric(k) == key)
      return lv_at(v->r, object, i * 2);
  }
  return 0;
}
static void avm_internal_set(dg_avm1_t *v, lv_t object, int key, lv_t value) {
  unsigned count = lv_count(v->r, object), at = count * 2 + 1;
  for (unsigned i = 1; i <= count; i++) {
    lv_t k = lv_at(v->r, object, i * 2 - 1);
    if (lv_type(k) == LV_NUMBER && lv_numeric(k) == key) {
      at = i * 2 - 1;
      break;
    }
  }
  lv_set_at(v->r, object, at, lv_num(key));
  lv_set_at(v->r, object, at + 1, value);
}
static lv_t *avm_frame_root(dg_avm1_t *v, unsigned frame, unsigned slot) {
  return &v->roots[AVM_FRAME_ROOT + frame * 4 + slot];
}
static void avm_fail(dg_avm1_t *v, const char *message) {
  char text[160];
  unsigned pc = v->depth ? v->frames[v->depth - 1].pc : 0;
  snprintf(text, sizeof(text), "AVM1 at %u: %s", pc, message);
  lv_fail(v->r, text);
}
static void avm_unroot(lv_runtime_t *r, lv_root_span_t *span) {
  lv_root_span_t **p = &r->external_roots;
  while (*p && *p != span)
    p = &(*p)->next;
  if (*p)
    *p = span->next;
}
lv_runtime_t *dg_avm1_values(dg_avm1_t *v) { return v->r; }
lv_t dg_avm1_globals(dg_avm1_t *v) { return v->roots[AVM_GLOBAL]; }
lv_t dg_avm1_result(dg_avm1_t *v) { return v->roots[AVM_RESULT]; }
lv_t dg_avm1_boolean(bool value) { return lv_make(LV_AVM_BOOLEAN, value); }
lv_t dg_avm1_null(void) { return lv_make(LV_AVM_NULL, 0); }
lv_t dg_avm1_prototype(dg_avm1_t *v, lv_t object) {
  return avm_internal(v, object, AVM_PROTO);
}
void dg_avm1_set_prototype(dg_avm1_t *v, lv_t object, lv_t prototype) {
  if (lv_type(object) != LV_PROPLIST ||
      (prototype && lv_type(prototype) != LV_PROPLIST)) {
    avm_fail(v, "invalid object prototype");
    return;
  }
  avm_internal_set(v, object, AVM_PROTO, prototype);
}
lv_t dg_avm1_object(dg_avm1_t *v, lv_t prototype) {
  lv_t object = lv_list(v->r, 0, NULL, true);
  avm_internal_set(v, object, AVM_OWNER, lv_num(v->identity));
  if (prototype)
    dg_avm1_set_prototype(v, object, prototype);
  return object;
}
bool dg_avm1_owns(dg_avm1_t *v, lv_t object) {
  lv_t owner = avm_internal(v, object, AVM_OWNER);
  return lv_type(owner) == LV_NUMBER && lv_numeric(owner) == v->identity;
}
bool dg_avm1_external_key(dg_avm1_t *v, lv_t object, const char *name,
                          char *out, unsigned capacity) {
  const char *found = name;
  for (unsigned depth = 0; lv_type(object) == LV_PROPLIST;
       depth++, object = dg_avm1_prototype(v, object)) {
    if (depth == 256) {
      avm_fail(v, "external property chain exceeds capacity");
      return false;
    }
    for (unsigned i = 1; i <= lv_count(v->r, object); i++) {
      lv_t k = lv_at(v->r, object, i * 2 - 1);
      if (lv_type(k) != LV_STRING && lv_type(k) != LV_SYMBOL)
        continue;
      const char *s = lv_cstr(v->r, k), *a = s, *b = name;
      while (*a && *b &&
             tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
      }
      if (!*a && !*b) {
        found = s;
        goto resolved;
      }
    }
  }
resolved:
  if (strlen(found) >= capacity) {
    avm_fail(v, "external property name exceeds capacity");
    return false;
  }
  memmove(out, found, strlen(found) + 1);
  return true;
}
static bool avm_key(dg_avm1_t *v, lv_t key, const char *name) {
  if (lv_type(key) != LV_STRING && lv_type(key) != LV_SYMBOL)
    return false;
  const char *text = lv_cstr(v->r, key);
  if (v->version >= 7)
    return !strcmp(text, name);
  while (*text && *name &&
         tolower((unsigned char)*text) == tolower((unsigned char)*name)) {
    text++;
    name++;
  }
  return !*text && !*name;
}
static unsigned avm_own(dg_avm1_t *v, lv_t object, const char *key) {
  if (lv_type(object) != LV_PROPLIST)
    return 0;
  for (unsigned i = 1; i <= lv_count(v->r, object); i++)
    if (avm_key(v, lv_at(v->r, object, i * 2 - 1), key))
      return i * 2;
  return 0;
}
static void avm_own_set(dg_avm1_t *v, lv_t object, const char *key,
                        lv_t value) {
  if (lv_type(object) != LV_PROPLIST) {
    avm_fail(v, "property write on non-object");
    return;
  }
  unsigned at = avm_own(v, object, key);
  if (!at) {
    unsigned count = lv_count(v->r, object);
    at = count * 2 + 2;
    for (unsigned i = 1; i <= count; i++)
      if (lv_type(lv_at(v->r, object, i * 2 - 1)) == LV_VOID) {
        at = i * 2;
        break;
      }
    lv_set_at(v->r, object, at - 1, lv_text(v->r, key, false));
  }
  lv_set_at(v->r, object, at, value);
}
bool dg_avm1_own_get(dg_avm1_t *v, lv_t object, const char *key, lv_t *out) {
  unsigned at = avm_own(v, object, key);
  *out = at ? lv_at(v->r, object, at) : 0;
  return at != 0;
}
void dg_avm1_own_set(dg_avm1_t *v, lv_t object, const char *key, lv_t value) {
  avm_own_set(v, object, key, value);
}
static unsigned avm_attributes(dg_avm1_t *v, lv_t object, const char *key) {
  lv_t flags = avm_internal(v, object, AVM_FLAGS);
  unsigned at = avm_own(v, flags, key);
  return at ? (unsigned)lv_numeric(lv_at(v->r, flags, at)) : 0;
}
void dg_avm1_flags(dg_avm1_t *v, lv_t object, const char *key, unsigned set,
                   unsigned clear) {
  if (lv_type(object) != LV_PROPLIST)
    return;
  lv_t flags = avm_internal(v, object, AVM_FLAGS);
  if (!flags) {
    flags = dg_avm1_object(v, 0);
    avm_internal_set(v, object, AVM_FLAGS, flags);
  }
  for (unsigned i = 1; i <= lv_count(v->r, object); i++) {
    lv_t name = lv_at(v->r, object, i * 2 - 1);
    if (lv_type(name) != LV_STRING && lv_type(name) != LV_SYMBOL)
      continue;
    if (key && !avm_key(v, name, key))
      continue;
    char stable[1024];
    if (strlen(lv_cstr(v->r, name)) >= sizeof(stable)) {
      avm_fail(v, "property name exceeds capacity");
      return;
    }
    strcpy(stable, lv_cstr(v->r, name));
    avm_own_set(
        v, flags, stable,
        lv_num(((avm_attributes(v, object, stable) | set) & ~clear) & 7));
  }
}
lv_t dg_avm1_keys(dg_avm1_t *v, lv_t object, bool inherited) {
  lv_t keys = lv_list(v->r, 0, NULL, false);
  lv_t seen_keys = lv_list(v->r, 0, NULL, false);
  lv_t held[] = {object, keys, seen_keys};
  lv_root_span_t span = {
      .next = v->r->external_roots, .values = held, .count = 3};
  v->r->external_roots = &span;
  unsigned count = 0, seen_count = 0;
  for (unsigned depth = 0; lv_type(object) == LV_PROPLIST; depth++) {
    if (depth == 256) {
      avm_fail(v, "cyclic or over-deep enumeration");
      break;
    }
    for (unsigned i = 1; i <= lv_count(v->r, object); i++) {
      lv_t key = lv_at(v->r, object, i * 2 - 1);
      if (lv_type(key) != LV_STRING && lv_type(key) != LV_SYMBOL)
        continue;
      bool seen = false;
      for (unsigned j = 1; j <= seen_count; j++)
        if (avm_key(v, lv_at(v->r, seen_keys, j), lv_cstr(v->r, key))) {
          seen = true;
          break;
        }
      if (!seen) {
        lv_set_at(v->r, seen_keys, ++seen_count, key);
        if (!(avm_attributes(v, object, lv_cstr(v->r, key)) & 1))
          lv_set_at(v->r, keys, ++count, key);
      }
    }
    if (!inherited)
      break;
    object = dg_avm1_prototype(v, object);
  }
  avm_unroot(v->r, &span);
  return keys;
}
static lv_t avm_get(dg_avm1_t *v, lv_t object, const char *key, lv_t *owner) {
  char stable[1024];
  if (strlen(key) >= sizeof(stable)) {
    avm_fail(v, "property name exceeds capacity");
    return 0;
  }
  strcpy(stable, key);
  key = stable;
  lv_t value;
  if (v->host.get && v->host.get(v->context, v, object, key, &value)) {
    if (owner)
      *owner = object;
    return value;
  }
  if (!strcmp(key, "__proto__"))
    return dg_avm1_prototype(v, object);
  for (unsigned depth = 0; lv_type(object) == LV_PROPLIST; depth++) {
    if (depth == 256) {
      avm_fail(v, "cyclic or over-deep prototype chain");
      return 0;
    }
    unsigned at = avm_own(v, object, key);
    if (at) {
      if (owner)
        *owner = object;
      return lv_at(v->r, object, at);
    }
    object = dg_avm1_prototype(v, object);
  }
  return 0;
}
lv_t dg_avm1_get(dg_avm1_t *v, lv_t object, const char *key) {
  return avm_get(v, object, key, NULL);
}
void dg_avm1_set(dg_avm1_t *v, lv_t object, const char *key, lv_t value) {
  // AVM1's temporary primitive wrappers do not retain property writes.
  if (lv_type(object) != LV_PROPLIST)
    return;
  char stable[1024];
  if (strlen(key) >= sizeof(stable)) {
    avm_fail(v, "property name exceeds capacity");
    return;
  }
  strcpy(stable, key);
  key = stable;
  lv_t owner = object;
  for (unsigned depth = 0; lv_type(owner) == LV_PROPLIST;
       depth++, owner = dg_avm1_prototype(v, owner)) {
    if (depth == 256) {
      avm_fail(v, "cyclic or over-deep property write");
      return;
    }
    if (avm_own(v, owner, key)) {
      if (avm_attributes(v, owner, key) & 4)
        return;
      break;
    }
  }
  if (v->host.set && v->host.set(v->context, v, object, key, value))
    return;
  if (!strcmp(key, "__proto__")) {
    dg_avm1_set_prototype(v, object, value);
    return;
  }
  avm_own_set(v, object, key, value);
}
bool dg_avm1_delete(dg_avm1_t *v, lv_t object, const char *key) {
  unsigned at = avm_own(v, object, key);
  if (at && (avm_attributes(v, object, key) & 2))
    return false;
  if (at) {
    lv_set_at(v->r, object, at - 1, 0);
    lv_set_at(v->r, object, at, 0);
  }
  lv_t flags = avm_internal(v, object, AVM_FLAGS);
  if (flags)
    avm_own_set(v, flags, key, lv_num(0));
  return true;
}
bool dg_avm1_callable(dg_avm1_t *v, lv_t object) {
  if (avm_internal(v, object, AVM_SUPER_RECEIVER))
    return true;
  lv_t code = avm_internal(v, object, AVM_CODE);
  return lv_type(code) == LV_NUMBER && lv_numeric(code) > 0 &&
         lv_numeric(code) <= v->function_count;
}
bool dg_avm1_truth(dg_avm1_t *v, lv_t value) {
  switch (lv_type(value)) {
  case LV_VOID:
  case LV_AVM_NULL:
    return false;
  case LV_AVM_BOOLEAN:
    return lv_id(value) != 0;
  case LV_NUMBER: {
    double n = lv_numeric(value);
    return n != 0 && !isnan(n);
  }
  case LV_STRING:
  case LV_SYMBOL:
    return *lv_cstr(v->r, value) != 0;
  default:
    return true;
  }
}
static lv_t avm_primitive(dg_avm1_t *v, lv_t value, bool string) {
  if (lv_type(value) != LV_PROPLIST)
    return value;
  const char *names[] = {string ? "toString" : "valueOf",
                         string ? "valueOf" : "toString"};
  for (unsigned i = 0; i < 2; i++) {
    lv_t fn = dg_avm1_get(v, value, names[i]), out = 0;
    if (dg_avm1_callable(v, fn) &&
        dg_avm1_invoke(v, fn, value, 0, NULL, 4096, &out) &&
        lv_type(out) != LV_PROPLIST)
      return out;
    if (v->r->failed)
      return 0;
  }
  return lv_text(v->r,
                 dg_avm1_callable(v, value) &&
                         !avm_internal(v, value, AVM_SUPER_RECEIVER)
                     ? "[type Function]"
                     : "[object Object]",
                 false);
}
double dg_avm1_number(dg_avm1_t *v, lv_t value) {
  value = avm_primitive(v, value, false);
  switch (lv_type(value)) {
  case LV_VOID:
    return v->version <= 6 ? 0 : NAN;
  case LV_AVM_NULL:
    return 0;
  case LV_AVM_BOOLEAN:
    return lv_id(value) != 0;
  case LV_NUMBER:
    return lv_numeric(value);
  case LV_STRING:
  case LV_SYMBOL: {
    const char *s = lv_cstr(v->r, value);
    while (isspace((unsigned char)*s))
      s++;
    if (!*s)
      return 0;
    char *end;
    errno = 0;
    double n = strtod(s, &end);
    if (end == s)
      return NAN;
    while (isspace((unsigned char)*end))
      end++;
    return *end ? NAN : n;
  }
  default:
    return NAN;
  }
}
lv_t dg_avm1_string(dg_avm1_t *v, lv_t value) {
  value = avm_primitive(v, value, true);
  switch (lv_type(value)) {
  case LV_STRING:
    return value;
  case LV_SYMBOL:
    return lv_text(v->r, lv_cstr(v->r, value), false);
  case LV_VOID:
    return lv_text(v->r, v->version <= 6 ? "" : "undefined", false);
  case LV_AVM_NULL:
    return lv_text(v->r, "null", false);
  case LV_AVM_BOOLEAN:
    return lv_text(v->r, lv_id(value) ? "true" : "false", false);
  case LV_NUMBER: {
    double n = lv_numeric(value);
    char text[64];
    if (isnan(n))
      snprintf(text, sizeof(text), "NaN");
    else if (isinf(n))
      snprintf(text, sizeof(text), "%sInfinity", n < 0 ? "-" : "");
    else if (n == 0)
      snprintf(text, sizeof(text), "0");
    else
      snprintf(text, sizeof(text), "%.15g", n);
    return lv_text(v->r, text, false);
  }
  default:
    return lv_text(v->r, "undefined", false);
  }
}
dg_avm1_t *dg_avm1_create(lv_runtime_t *r, unsigned version,
                          dg_avm1_host_t host, void *context) {
  static uint32_t serial;
  if (serial == UINT32_MAX) {
    lv_fail(r, "AVM1 identity capacity exceeded");
    return NULL;
  }
  dg_avm1_t *v = calloc(1, sizeof(*v));
  if (!v) {
    lv_fail(r, "AVM1 allocation failed");
    return NULL;
  }
  v->r = r;
  v->identity = ++serial;
  v->version = version;
  v->host = host;
  v->context = context;
  v->span = (lv_root_span_t){
      .next = r->external_roots, .values = v->roots, .count = AVM_ROOTS};
  r->external_roots = &v->span;
  v->roots[AVM_GLOBAL] = dg_avm1_object(v, 0);
  avm_own_set(v, v->roots[AVM_GLOBAL], "_global", v->roots[AVM_GLOBAL]);
  avm_own_set(v, v->roots[AVM_GLOBAL], "undefined", 0);
  avm_own_set(v, v->roots[AVM_GLOBAL], "NaN", lv_float(NAN));
  avm_own_set(v, v->roots[AVM_GLOBAL], "Infinity", lv_float(INFINITY));
  return v;
}
void dg_avm1_dispose(dg_avm1_t *v) {
  if (!v)
    return;
  avm_unroot(v->r, &v->span);
  for (unsigned i = 0; i < v->pool_count; i++)
    free(v->pools[i].strings);
  for (unsigned i = 0; i < v->function_count; i++)
    free(v->functions[i].native);
  free(v);
}
lv_t dg_avm1_native(dg_avm1_t *v, const char *name) {
  unsigned n = 0;
  while (n < v->function_count &&
         (!v->functions[n].native || strcmp(v->functions[n].native, name)))
    n++;
  if (n == v->function_count) {
    if (n == AVM_FUNCTIONS) {
      avm_fail(v, "function definition capacity exceeded");
      return 0;
    }
    char *copy = malloc(strlen(name) + 1);
    if (!copy) {
      avm_fail(v, "native function allocation failed");
      return 0;
    }
    strcpy(copy, name);
    v->functions[n].native = copy;
    v->function_count++;
  }
  lv_t fn = dg_avm1_object(v, 0);
  avm_internal_set(v, fn, AVM_CODE, lv_num(n + 1));
  return fn;
}
static unsigned avm_u16(dg_avm1_t *v, const uint8_t *b, unsigned length,
                        unsigned *at) {
  if (*at > length || length - *at < 2) {
    avm_fail(v, "truncated u16");
    return 0;
  }
  unsigned n = b[*at] | (unsigned)b[*at + 1] << 8;
  *at += 2;
  return n;
}
static const char *avm_cstring(dg_avm1_t *v, const uint8_t *b, unsigned length,
                               unsigned *at) {
  if (*at > length) {
    avm_fail(v, "string outside action");
    return "";
  }
  const uint8_t *end = memchr(b + *at, 0, length - *at);
  if (!end) {
    avm_fail(v, "unterminated string");
    return "";
  }
  const char *text = (const char *)b + *at;
  *at = (unsigned)(end - b) + 1;
  return text;
}
static bool avm_push(dg_avm1_t *v, lv_t value) {
  if (v->stack_top == AVM_STACK) {
    avm_fail(v, "operand stack capacity exceeded");
    return false;
  }
  v->roots[2 + v->stack_top++] = value;
  return true;
}
static lv_t avm_pop(dg_avm1_t *v) {
  unsigned base = v->depth ? v->frames[v->depth - 1].stack_base : 0;
  if (v->stack_top <= base) {
    avm_fail(v, "operand stack underflow");
    return 0;
  }
  lv_t value = v->roots[2 + --v->stack_top];
  v->roots[2 + v->stack_top] = 0;
  if (v->instruction_roots) {
    lv_root_span_t *span = v->instruction_roots;
    if (span->count == AVM_ARGUMENTS + 4) {
      avm_fail(v, "instruction temporary capacity exceeded");
      return 0;
    }
    ((lv_t *)span->values)[span->count++] = value;
  }
  return value;
}
static lv_t avm_scope(dg_avm1_t *v, unsigned slot) {
  return v->depth ? *avm_frame_root(v, v->depth - 1, slot)
                  : v->roots[AVM_GLOBAL];
}
static lv_t avm_target(dg_avm1_t *v, lv_t target) {
  if (lv_type(target) == LV_PROPLIST)
    return target;
  lv_t text = dg_avm1_string(v, target);
  const char *path = lv_cstr(v->r, text);
  if (!*path)
    return avm_scope(v, AVM_TARGET);
  if (v->host.target)
    return v->host.target(v->context, v, avm_scope(v, AVM_TARGET), path);
  return 0;
}
static lv_t avm_variable(dg_avm1_t *v, const char *name, lv_t *owner, char *key,
                         unsigned capacity) {
  size_t length = strlen(name);
  if (length >= capacity) {
    avm_fail(v, "variable path exceeds capacity");
    return 0;
  }
  memcpy(key, name, length + 1);
  char *separator = strrchr(key, ':');
  if (!separator)
    separator = strrchr(key, '.');
  if (separator) {
    *separator = 0;
    lv_t object = 0;
    if (v->host.target)
      object = v->host.target(v->context, v, avm_scope(v, AVM_TARGET), key);
    if (!object) {
      char nested[1024];
      lv_t ignored;
      object = avm_variable(v, key, &ignored, nested, sizeof(nested));
    }
    memmove(key, separator + 1, strlen(separator + 1) + 1);
    if (owner)
      *owner = object;
    return dg_avm1_get(v, object, key);
  }
  lv_t scopes = avm_scope(v, AVM_SCOPE);
  for (unsigned i = 1; i <= lv_count(v->r, scopes); i++) {
    lv_t object = lv_at(v->r, scopes, i), value = dg_avm1_get(v, object, key);
    if (value || avm_own(v, object, key)) {
      if (owner)
        *owner = object;
      return value;
    }
  }
  if (owner)
    *owner = avm_scope(v, AVM_TARGET);
  return 0;
}
static unsigned avm_pool(dg_avm1_t *v, const uint8_t *data, unsigned length) {
  for (unsigned i = 0; i < v->pool_count; i++)
    if (v->pools[i].source == data)
      return i + 1;
  unsigned at = 0, count = avm_u16(v, data, length, &at);
  if (count > 4096 || v->pool_count == AVM_POOLS) {
    avm_fail(v, "constant pool capacity exceeded");
    return 0;
  }
  const char **strings = count ? calloc(count, sizeof(*strings)) : NULL;
  if (count && !strings) {
    avm_fail(v, "constant pool allocation failed");
    return 0;
  }
  for (unsigned i = 0; i < count && !v->r->failed; i++)
    strings[i] = avm_cstring(v, data, length, &at);
  if (at != length || v->r->failed) {
    free(strings);
    avm_fail(v, "invalid constant pool length");
    return 0;
  }
  v->pools[v->pool_count++] = (avm_pool_t){data, strings, count};
  return v->pool_count;
}
static void avm_return(dg_avm1_t *v, lv_t result) {
  unsigned n = v->depth - 1;
  avm_frame_t f = v->frames[n];
  if (f.construct && lv_type(result) != LV_PROPLIST)
    result = *avm_frame_root(v, n, AVM_THIS);
  while (v->stack_top > f.stack_base)
    v->roots[2 + --v->stack_top] = 0;
  for (unsigned i = 0; i < f.register_count; i++)
    v->roots[AVM_REGISTER_ROOT + f.register_base + i] = 0;
  v->register_top = f.register_base;
  for (unsigned i = 0; i < 4; i++)
    *avm_frame_root(v, n, i) = 0;
  v->depth--;
  v->roots[AVM_RESULT] = result;
  if (f.result)
    avm_push(v, result);
}
static bool avm_enter(dg_avm1_t *v, lv_t fn, lv_t receiver, lv_t owner,
                      unsigned argc, const lv_t *args, bool result,
                      bool construct) {
  unsigned id = (unsigned)lv_numeric(avm_internal(v, fn, AVM_CODE)) - 1;
  avm_function_t *function = &v->functions[id];
  if (v->depth == AVM_FRAMES ||
      function->registers > AVM_ROOTS - AVM_REGISTER_ROOT - v->register_top) {
    avm_fail(v, "call frame/register capacity exceeded");
    return false;
  }
  unsigned n = v->depth++;
  avm_frame_t *f = &v->frames[n];
  *f = (avm_frame_t){.code = function->code,
                     .length = function->length,
                     .stack_base = v->stack_top,
                     .register_base = v->register_top,
                     .register_count = function->registers,
                     .pool = function->pool,
                     .result = result,
                     .construct = construct};
  v->register_top += f->register_count;
  *avm_frame_root(v, n, AVM_FUNCTION) = fn;
  *avm_frame_root(v, n, AVM_THIS) = receiver;
  lv_t target = avm_internal(v, fn, AVM_DEF_TARGET);
  *avm_frame_root(v, n, AVM_TARGET) =
      target ? target
      : n    ? *avm_frame_root(v, n - 1, AVM_TARGET)
             : v->roots[AVM_GLOBAL];
  lv_t closure = avm_internal(v, fn, AVM_CLOSURE);
  lv_t scopes = lv_list(v->r, 0, NULL, false);
  *avm_frame_root(v, n, AVM_SCOPE) = scopes;
  lv_t locals = dg_avm1_object(v, 0);
  lv_set_at(v->r, scopes, 1, locals);
  unsigned count = lv_type(closure) == LV_LIST ? lv_count(v->r, closure) : 0;
  for (unsigned i = 1; i <= count; i++)
    lv_set_at(v->r, scopes, i + 1, lv_at(v->r, closure, i));
  if (!count) {
    lv_set_at(v->r, scopes, 2, *avm_frame_root(v, n, AVM_TARGET));
    lv_set_at(v->r, scopes, 3, v->roots[AVM_GLOBAL]);
  }
  unsigned flags = function->flags, reg = 1;
  if (!(flags & 2))
    avm_own_set(v, locals, "this", receiver);
  lv_t super = 0;
  if (v->version >= 6 && (!(flags & 32) || (flags & 16))) {
    super = dg_avm1_object(v, dg_avm1_prototype(v, owner));
    avm_internal_set(v, super, AVM_SUPER_RECEIVER, receiver);
    lv_t constructor = avm_internal(v, owner, AVM_CONSTRUCTOR);
    if (!constructor)
      constructor = dg_avm1_get(v, dg_avm1_prototype(v, owner), "constructor");
    avm_internal_set(v, super, AVM_SUPER_CONSTRUCTOR, constructor);
    if (!(flags & 32))
      avm_own_set(v, locals, "super", super);
  }
  lv_t arguments = dg_avm1_object(v, 0);
  if (!(flags & 8))
    avm_own_set(v, locals, "arguments", arguments);
  for (unsigned i = 0; i < argc; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%u", i);
    avm_own_set(v, arguments, key, args[i]);
  }
  avm_own_set(v, arguments, "length", lv_num(argc));
  avm_own_set(v, arguments, "callee", fn);
  dg_avm1_flags(v, arguments, "length", 1, 0);
  dg_avm1_flags(v, arguments, "callee", 1, 0);
  lv_t *registers = &v->roots[AVM_REGISTER_ROOT + f->register_base];
  unsigned at = 0;
  for (unsigned i = 0; i < function->argument_count && !v->r->failed; i++) {
    unsigned r = function->version2 ? function->parameters[at++] : 0;
    const char *name =
        avm_cstring(v, function->parameters, function->parameter_length, &at);
    lv_t value = i < argc ? args[i] : 0;
    if (r) {
      if (r >= f->register_count) {
        avm_fail(v, "function argument register overflow");
        return false;
      }
      registers[r] = value;
    } else
      avm_own_set(v, locals, name, value);
  }
#define AVM_PRELOAD(flag, value)                                               \
  do {                                                                         \
    if (flags & (flag)) {                                                      \
      if (reg >= f->register_count) {                                          \
        avm_fail(v, "function preload register overflow");                     \
        return false;                                                          \
      }                                                                        \
      registers[reg++] = (value);                                              \
    }                                                                          \
  } while (0)
  AVM_PRELOAD(1, receiver);
  AVM_PRELOAD(4, arguments);
  AVM_PRELOAD(16, super);
  AVM_PRELOAD(64, dg_avm1_get(v, *avm_frame_root(v, n, AVM_TARGET), "_root"));
  AVM_PRELOAD(128,
              dg_avm1_get(v, *avm_frame_root(v, n, AVM_TARGET), "_parent"));
  AVM_PRELOAD(256, v->roots[AVM_GLOBAL]);
#undef AVM_PRELOAD
  return !v->r->failed;
}
// Native callbacks supply a function value rather than a property key. Find
// its defining object so an inherited method's super starts above that
// method, even when called on a further-derived instance.
static lv_t avm_function_owner(dg_avm1_t *v, lv_t receiver, lv_t fn) {
  for (unsigned depth = 0; lv_type(receiver) == LV_PROPLIST; depth++) {
    if (depth == 256) {
      avm_fail(v, "cyclic or over-deep method owner");
      return 0;
    }
    for (unsigned i = 1; i <= lv_count(v->r, receiver); i++) {
      lv_t key = lv_at(v->r, receiver, i * 2 - 1);
      if ((lv_type(key) == LV_STRING || lv_type(key) == LV_SYMBOL) &&
          lv_at(v->r, receiver, i * 2) == fn)
        return receiver;
    }
    receiver = dg_avm1_prototype(v, receiver);
  }
  return 0;
}
static bool avm_call(dg_avm1_t *v, lv_t fn, lv_t receiver, lv_t owner,
                     unsigned argc, const lv_t *args, bool push,
                     bool construct) {
  lv_t super_receiver = avm_internal(v, fn, AVM_SUPER_RECEIVER);
  bool super_call = super_receiver != 0;
  if (super_receiver) {
    receiver = super_receiver;
    fn = avm_internal(v, fn, AVM_SUPER_CONSTRUCTOR);
    owner = dg_avm1_get(v, fn, "prototype");
  } else {
    super_receiver = avm_internal(v, receiver, AVM_SUPER_RECEIVER);
    if (super_receiver) {
      super_call = true;
      receiver = super_receiver;
    }
  }
  if (!dg_avm1_callable(v, fn)) {
    v->roots[AVM_RESULT] = 0;
    return !push || avm_push(v, 0);
  }
  if (!owner)
    owner = avm_function_owner(v, receiver, fn);
  if (construct || !owner)
    owner = dg_avm1_get(v, fn, "prototype");
  // SWF6 starts an ordinary call's super at the receiver's parent prototype,
  // even for an inherited override. Calls through super then advance from
  // that selected prototype. SWF7 uses the actual defining method owner.
  if (v->version == 6 && !construct && !super_call)
    owner = dg_avm1_prototype(v, receiver);
  if (argc > AVM_ARGUMENTS) {
    avm_fail(v, "argument capacity exceeded");
    return false;
  }
  lv_t held[AVM_ARGUMENTS + 2];
  held[0] = fn;
  held[1] = receiver;
  for (unsigned i = 0; i < argc; i++)
    held[i + 2] = args[i];
  lv_root_span_t span = {
      .next = v->r->external_roots, .values = held, .count = argc + 2};
  v->r->external_roots = &span;
  if (construct) {
    held[1] = receiver = dg_avm1_object(v, dg_avm1_get(v, fn, "prototype"));
    avm_internal_set(v, receiver, AVM_CONSTRUCTOR, fn);
  }
  unsigned id = (unsigned)lv_numeric(avm_internal(v, fn, AVM_CODE)) - 1;
  avm_function_t *function = &v->functions[id];
  bool ok;
  if (function->native) {
    lv_t out = 0;
    if (v->native_depth++ == 16) {
      avm_fail(v, "native callback depth exceeded");
      ok = false;
    } else {
      ok = v->host.call && v->host.call(v->context, v, function->native,
                                        receiver, argc, args, &out);
      if (!ok) {
        char message[128];
        snprintf(message, sizeof(message), "native function unavailable: %.80s",
                 function->native);
        avm_fail(v, message);
      }
    }
    v->native_depth--;
    if (construct && lv_type(out) != LV_PROPLIST)
      out = receiver;
    if (construct && lv_type(out) == LV_PROPLIST)
      avm_internal_set(v, out, AVM_CONSTRUCTOR, fn);
    v->roots[AVM_RESULT] = out;
    if (push)
      avm_push(v, out);
  } else
    ok = avm_enter(v, fn, receiver, owner, argc, args, push, construct);
  avm_unroot(v->r, &span);
  return ok && !v->r->failed;
}
bool dg_avm1_start(dg_avm1_t *v, lv_t target, const uint8_t *code,
                   unsigned length) {
  if (v->depth) {
    avm_fail(v, "action started while VM busy");
    return false;
  }
  if (!code || !length || length > 1024 * 1024 ||
      lv_type(target) != LV_PROPLIST) {
    avm_fail(v, "invalid action entry");
    return false;
  }
  v->depth = 1;
  v->stack_top = v->register_top = 0;
  v->frames[0] =
      (avm_frame_t){.code = code, .length = length, .register_count = 4};
  v->register_top = 4;
  *avm_frame_root(v, 0, AVM_THIS) = *avm_frame_root(v, 0, AVM_TARGET) = target;
  lv_t scopes = lv_list(v->r, 0, NULL, false);
  *avm_frame_root(v, 0, AVM_SCOPE) = scopes;
  lv_set_at(v->r, scopes, 1, target);
  lv_set_at(v->r, scopes, 2, v->roots[AVM_GLOBAL]);
  v->roots[AVM_RESULT] = 0;
  return !v->r->failed;
}
bool dg_avm1_invoke(dg_avm1_t *v, lv_t fn, lv_t receiver, unsigned argc,
                    const lv_t *args, unsigned budget, lv_t *out) {
  unsigned depth = v->depth;
  if (!avm_call(v, fn, receiver, 0, argc, args, false, false))
    return false;
  dg_avm1_status_t status = avm_run_until(v, budget, depth);
  if (status == DG_AVM1_BUSY) {
    avm_fail(v, "synchronous call instruction bound exceeded");
    return false;
  }
  *out = v->roots[AVM_RESULT];
  return status == DG_AVM1_IDLE;
}
static bool avm_name(dg_avm1_t *v, lv_t value, char *out, unsigned capacity) {
  lv_t text = dg_avm1_string(v, value);
  const char *s = lv_cstr(v->r, text);
  if (strlen(s) >= capacity) {
    avm_fail(v, "action name exceeds capacity");
    return false;
  }
  strcpy(out, s);
  return !v->r->failed;
}
static unsigned avm_arguments(dg_avm1_t *v, lv_t *args) {
  double count = dg_avm1_number(v, avm_pop(v));
  if (!isfinite(count) || count < 0 || count > AVM_ARGUMENTS ||
      floor(count) != count) {
    avm_fail(v, "invalid argument count");
    return 0;
  }
  for (unsigned i = 0; i < (unsigned)count; i++)
    args[i] = avm_pop(v);
  return (unsigned)count;
}
static bool avm_native_call(dg_avm1_t *v, const char *name, lv_t receiver,
                            unsigned argc, const lv_t *args) {
  lv_t out = 0;
  bool ok = v->host.call &&
            v->host.call(v->context, v, name, receiver, argc, args, &out);
  if (!ok) {
    char message[128];
    snprintf(message, sizeof(message), "native action unavailable: %.80s",
             name);
    avm_fail(v, message);
  }
  return ok;
}
static bool avm_equal(dg_avm1_t *v, lv_t a, lv_t b, bool strict) {
  unsigned ta = lv_type(a), tb = lv_type(b);
  if (ta == LV_SYMBOL)
    ta = LV_STRING;
  if (tb == LV_SYMBOL)
    tb = LV_STRING;
  if (ta == tb) {
    if (ta == LV_NUMBER)
      return lv_numeric(a) == lv_numeric(b);
    if (ta == LV_STRING)
      return !strcmp(lv_cstr(v->r, a), lv_cstr(v->r, b));
    return a == b;
  }
  if (strict)
    return false;
  if ((ta == LV_VOID && tb == LV_AVM_NULL) ||
      (tb == LV_VOID && ta == LV_AVM_NULL))
    return true;
  if (ta == LV_VOID || tb == LV_VOID || ta == LV_AVM_NULL || tb == LV_AVM_NULL)
    return false;
  if (ta == LV_AVM_BOOLEAN)
    a = lv_num(lv_id(a) != 0);
  if (tb == LV_AVM_BOOLEAN)
    b = lv_num(lv_id(b) != 0);
  ta = lv_type(a);
  tb = lv_type(b);
  if (ta == LV_PROPLIST &&
      (tb == LV_NUMBER || tb == LV_STRING || tb == LV_SYMBOL))
    a = avm_primitive(v, a, false);
  if (tb == LV_PROPLIST &&
      (ta == LV_NUMBER || ta == LV_STRING || ta == LV_SYMBOL))
    b = avm_primitive(v, b, false);
  ta = lv_type(a);
  tb = lv_type(b);
  if (ta == LV_NUMBER || tb == LV_NUMBER)
    return dg_avm1_number(v, a) == dg_avm1_number(v, b);
  return false;
}
static uint32_t avm_uint(dg_avm1_t *v, lv_t value) {
  double n = dg_avm1_number(v, value);
  if (!isfinite(n) || n == 0)
    return 0;
  n = fmod(trunc(n), 4294967296.0);
  if (n < 0)
    n += 4294967296.0;
  return (uint32_t)n;
}
static void avm_define_function(dg_avm1_t *v, avm_frame_t *f, unsigned opcode,
                                const uint8_t *data, unsigned size) {
  unsigned at = 0;
  const char *name = avm_cstring(v, data, size, &at);
  unsigned argc = avm_u16(v, data, size, &at), registers = 4, flags = 0;
  if (argc > AVM_ARGUMENTS) {
    avm_fail(v, "function argument capacity exceeded");
    return;
  }
  bool version2 = opcode == 0x8e;
  if (version2) {
    if (at == size) {
      avm_fail(v, "truncated function registers");
      return;
    }
    registers = (unsigned)data[at++];
    flags = avm_u16(v, data, size, &at);
    if (flags & ~511u) {
      avm_fail(v, "invalid function preload flags");
      return;
    }
    if (((flags & 3) == 3) || ((flags & 12) == 12) || ((flags & 48) == 48)) {
      avm_fail(v, "conflicting function preload/suppress flags");
      return;
    }
  }
  unsigned parameters = at;
  for (unsigned i = 0; i < argc && !v->r->failed; i++) {
    if (version2) {
      if (at == size) {
        avm_fail(v, "truncated parameter register");
        return;
      }
      at++;
    }
    avm_cstring(v, data, size, &at);
  }
  unsigned parameter_length = at - parameters;
  unsigned length = avm_u16(v, data, size, &at);
  if (v->r->failed || at != size || length > f->length - f->pc) {
    avm_fail(v, "invalid function body range");
    return;
  }
  const uint8_t *body = f->code + f->pc;
  f->pc += length;
  unsigned id = 0;
  while (id < v->function_count &&
         (v->functions[id].code != body || v->functions[id].pool != f->pool))
    id++;
  if (id == v->function_count) {
    if (id == AVM_FUNCTIONS) {
      avm_fail(v, "function definition capacity exceeded");
      return;
    }
    v->functions[v->function_count++] =
        (avm_function_t){.code = body,
                         .length = length,
                         .parameters = data + parameters,
                         .argument_count = argc,
                         .parameter_length = parameter_length,
                         .registers = registers,
                         .flags = flags,
                         .pool = f->pool,
                         .version2 = version2};
  }
  lv_t fn = dg_avm1_object(v, 0);
  avm_internal_set(v, fn, AVM_CODE, lv_num(id + 1));
  avm_internal_set(v, fn, AVM_CLOSURE, avm_scope(v, AVM_SCOPE));
  avm_internal_set(v, fn, AVM_DEF_TARGET, avm_scope(v, AVM_TARGET));
  lv_t object = dg_avm1_get(v, v->roots[AVM_GLOBAL], "Object");
  lv_t prototype = dg_avm1_object(v, dg_avm1_get(v, object, "prototype"));
  avm_own_set(v, fn, "prototype", prototype);
  avm_own_set(v, prototype, "constructor", fn);
  dg_avm1_flags(v, fn, "prototype", 1, 0);
  dg_avm1_flags(v, prototype, "constructor", 1, 0);
  if (*name) {
    lv_t scopes = avm_scope(v, AVM_SCOPE);
    avm_own_set(v, lv_at(v->r, scopes, 1), name, fn);
  } else
    avm_push(v, fn);
}
static void avm_push_data(dg_avm1_t *v, avm_frame_t *f, const uint8_t *data,
                          unsigned size) {
  unsigned at = 0;
  while (at < size && !v->r->failed) {
    unsigned type = data[at++];
    lv_t value = 0;
    switch (type) {
    case 0:
      value = avm_source_text(v, avm_cstring(v, data, size, &at));
      break;
    case 1: {
      if (size - at < 4) {
        avm_fail(v, "truncated float");
        return;
      }
      uint32_t bits = (uint32_t)data[at] | (uint32_t)data[at + 1] << 8 |
                      (uint32_t)data[at + 2] << 16 |
                      (uint32_t)data[at + 3] << 24;
      float n;
      memcpy(&n, &bits, 4);
      value = lv_float(n);
      at += 4;
      break;
    }
    case 2:
      value = dg_avm1_null();
      break;
    case 3:
      break;
    case 4:
      if (at == size || data[at] >= f->register_count) {
        avm_fail(v, "invalid register operand");
        return;
      }
      value = v->roots[AVM_REGISTER_ROOT + f->register_base + data[at++]];
      break;
    case 5:
      if (at == size || data[at] > 1) {
        avm_fail(v, "invalid boolean operand");
        return;
      }
      value = dg_avm1_boolean(data[at++] != 0);
      break;
    case 6: {
      if (size - at < 8) {
        avm_fail(v, "truncated double");
        return;
      }
      uint64_t bits = 0;
      for (unsigned i = 0; i < 8; i++)
        bits |= (uint64_t)data[at + (i < 4 ? i + 4 : i - 4)] << (i * 8);
      double number;
      memcpy(&number, &bits, 8);
      value = lv_float(number);
      at += 8;
      break;
    }
    case 7: {
      if (size - at < 4) {
        avm_fail(v, "truncated integer");
        return;
      }
      uint32_t bits = (uint32_t)data[at] | (uint32_t)data[at + 1] << 8 |
                      (uint32_t)data[at + 2] << 16 |
                      (uint32_t)data[at + 3] << 24;
      value = lv_num((int32_t)bits);
      at += 4;
      break;
    }
    case 8:
    case 9: {
      unsigned id;
      if (type == 8) {
        if (at == size) {
          avm_fail(v, "truncated constant operand");
          return;
        }
        id = data[at++];
      } else
        id = avm_u16(v, data, size, &at);
      if (!f->pool || id >= v->pools[f->pool - 1].count) {
        avm_fail(v, "constant operand outside pool");
        return;
      }
      value = avm_source_text(v, v->pools[f->pool - 1].strings[id]);
      break;
    }
    default:
      avm_fail(v, "unknown Push value type");
      return;
    }
    avm_push(v, value);
  }
}
static bool avm_boundary(const avm_frame_t *f, unsigned target) {
  unsigned at = 0;
  while (at < target) {
    if (at >= f->length)
      return false;
    unsigned opcode = f->code[at++], length = 0;
    if (opcode >= 128) {
      if (f->length - at < 2)
        return false;
      length = f->code[at] | (unsigned)f->code[at + 1] << 8;
      at += 2;
      if (length > f->length - at)
        return false;
    }
    unsigned end = at + length;
    if (opcode == 0x9b || opcode == 0x8e) {
      if (length < 2)
        return false;
      unsigned body = f->code[end - 2] | (unsigned)f->code[end - 1] << 8;
      if (body > f->length - end)
        return false;
      end += body;
    }
    at = end;
  }
  return at == target;
}
static void avm_branch(dg_avm1_t *v, avm_frame_t *f, const uint8_t *data,
                       unsigned length) {
  if (length != 2) {
    avm_fail(v, "invalid branch operand");
    return;
  }
  int64_t next = (int64_t)f->pc + (int16_t)(data[0] | (unsigned)data[1] << 8);
  if (next < 0 || next > f->length || !avm_boundary(f, (unsigned)next)) {
    avm_fail(v, "branch outside action boundary");
    return;
  }
  f->pc = (unsigned)next;
}
static void avm_step(dg_avm1_t *v) {
  avm_frame_t *f = &v->frames[v->depth - 1];
  if (f->pc == f->length) {
    avm_return(v, 0);
    return;
  }
  if (f->pc > f->length) {
    avm_fail(v, "program counter outside action");
    return;
  }
  unsigned opcode = f->code[f->pc++], size = 0;
  if (opcode >= 128)
    size = avm_u16(v, f->code, f->length, &f->pc);
  if (v->r->failed || size > f->length - f->pc) {
    avm_fail(v, "truncated action record");
    return;
  }
  const uint8_t *data = f->code + f->pc;
  f->pc += size;
  lv_t held[AVM_ARGUMENTS + 4];
  lv_root_span_t span = {.next = v->r->external_roots, .values = held};
  lv_root_span_t *previous = v->instruction_roots;
  v->instruction_roots = &span;
  v->r->external_roots = &span;
  v->r->atomic_depth++;
  v->r->step_serial++;
  v->r->steps++;
  lv_t a = 0, b = 0, c = 0;
  char name[1024];
  unsigned at = 0;
  switch (opcode) {
  case 0:
    avm_return(v, 0);
    break;
  case 4:
  case 5:
    avm_native_call(v,
                    opcode == 4 ? "MovieClip.nextFrame" : "MovieClip.prevFrame",
                    avm_scope(v, AVM_TARGET), 0, NULL);
    break;
  case 6:
  case 7:
    avm_native_call(v, opcode == 6 ? "MovieClip.play" : "MovieClip.stop",
                    avm_scope(v, AVM_TARGET), 0, NULL);
    break;
  case 0x0a:
  case 0x0b:
  case 0x0c:
  case 0x0d:
  case 0x3f:
    b = avm_pop(v);
    a = avm_pop(v);
    {
      double left = dg_avm1_number(v, a), right = dg_avm1_number(v, b);
      avm_push(v, lv_float(opcode == 0x0a   ? left + right
                           : opcode == 0x0b ? left - right
                           : opcode == 0x0c ? left * right
                           : opcode == 0x0d ? left / right
                                            : fmod(left, right)));
    }
    break;
  case 0x10:
  case 0x11:
    b = avm_pop(v);
    a = avm_pop(v);
    avm_push(v,
             dg_avm1_boolean(opcode == 0x10
                                 ? dg_avm1_truth(v, a) && dg_avm1_truth(v, b)
                                 : dg_avm1_truth(v, a) || dg_avm1_truth(v, b)));
    break;
  case 0x12:
    avm_push(v, dg_avm1_boolean(!dg_avm1_truth(v, avm_pop(v))));
    break;
  case 0x17:
    (void)avm_pop(v);
    break;
  case 0x1c:
    a = avm_pop(v);
    if (avm_name(v, a, name, sizeof(name))) {
      lv_t owner;
      char key[1024];
      avm_push(v, avm_variable(v, name, &owner, key, sizeof(key)));
    }
    break;
  case 0x1d:
  case 0x3c:
    b = avm_pop(v);
    a = avm_pop(v);
    if (avm_name(v, a, name, sizeof(name))) {
      if (opcode == 0x3c) {
        lv_t scopes = avm_scope(v, AVM_SCOPE);
        avm_own_set(v, lv_at(v->r, scopes, 1), name, b);
      } else {
        lv_t owner;
        char key[1024];
        (void)avm_variable(v, name, &owner, key, sizeof(key));
        dg_avm1_set(v, owner, key, b);
      }
    }
    break;
  case 0x20:
    a = avm_target(v, avm_pop(v));
    if (lv_type(a) != LV_PROPLIST)
      avm_fail(v, "SetTarget target missing");
    else
      *avm_frame_root(v, v->depth - 1, AVM_TARGET) = a;
    break;
  case 0x22:
  case 0x23: {
    static const char *const properties[] = {"_x",
                                             "_y",
                                             "_xscale",
                                             "_yscale",
                                             "_currentframe",
                                             "_totalframes",
                                             "_alpha",
                                             "_visible",
                                             "_width",
                                             "_height",
                                             "_rotation",
                                             "_target",
                                             "_framesloaded",
                                             "_name",
                                             "_droptarget",
                                             "_url",
                                             "_highquality",
                                             "_focusrect",
                                             "_soundbuftime",
                                             "_quality",
                                             "_xmouse",
                                             "_ymouse"};
    if (opcode == 0x23)
      c = avm_pop(v);
    b = avm_pop(v);
    a = avm_target(v, avm_pop(v));
    double index = dg_avm1_number(v, b);
    if (index < 0 || index >= sizeof(properties) / sizeof(*properties) ||
        floor(index) != index)
      avm_fail(v, "invalid indexed property");
    else if (opcode == 0x22)
      avm_push(v, dg_avm1_get(v, a, properties[(unsigned)index]));
    else
      dg_avm1_set(v, a, properties[(unsigned)index], c);
    break;
  }
  case 0x26:
    a = dg_avm1_string(v, avm_pop(v));
    if (v->host.trace)
      v->host.trace(v->context, lv_cstr(v->r, a));
    break;
  case 0x27: {
    lv_t args[6];
    a = avm_target(v, avm_pop(v));
    args[0] = avm_pop(v);
    b = avm_pop(v);
    unsigned count = 1;
    if (dg_avm1_truth(v, b)) {
      lv_t y2 = avm_pop(v), x2 = avm_pop(v), y1 = avm_pop(v), x1 = avm_pop(v);
      args[1] = x1;
      args[2] = y1;
      args[3] = x2;
      args[4] = y2;
      count = 5;
    }
    avm_native_call(v, "MovieClip.startDrag", a, count, args);
    break;
  }
  case 0x28:
    avm_native_call(v, "MovieClip.stopDrag", avm_scope(v, AVM_TARGET), 0, NULL);
    break;
  case 0x30: {
    double limit = dg_avm1_number(v, avm_pop(v));
    v->r->random_state = v->r->random_state * 1664525u + 1013904223u;
    avm_push(v, lv_num(isfinite(limit) && limit > 0
                           ? floor((double)v->r->random_state / 4294967296.0 *
                                   floor(limit))
                           : 0));
    break;
  }
  case 0x34: {
    lv_t out = 0;
    if (!v->host.call || !v->host.call(v->context, v, "getTimer",
                                       avm_scope(v, AVM_TARGET), 0, NULL, &out))
      avm_fail(v, "clock service unavailable");
    else
      avm_push(v, out);
    break;
  }
  case 0x3a:
    b = avm_pop(v);
    a = avm_pop(v);
    if (avm_name(v, b, name, sizeof(name)))
      avm_push(v, dg_avm1_boolean(dg_avm1_delete(v, a, name)));
    break;
  case 0x3b:
    a = avm_pop(v);
    if (avm_name(v, a, name, sizeof(name))) {
      lv_t owner;
      char key[1024];
      (void)avm_variable(v, name, &owner, key, sizeof(key));
      avm_push(v, dg_avm1_boolean(dg_avm1_delete(v, owner, key)));
    }
    break;
  case 0x3d:
  case 0x40:
  case 0x52:
  case 0x53: {
    lv_t args[AVM_ARGUMENTS], receiver = avm_scope(v, AVM_TARGET), fn = 0,
                              owner = 0;
    a = avm_pop(v);
    if (opcode == 0x52 || opcode == 0x53) {
      receiver = avm_pop(v);
      if (lv_type(a) == LV_VOID ||
          (avm_name(v, a, name, sizeof(name)) && !*name))
        fn = receiver;
      else if (!v->r->failed)
        fn = avm_get(v, receiver, name, &owner);
    } else if (dg_avm1_callable(v, a))
      fn = a;
    else if (avm_name(v, a, name, sizeof(name))) {
      char key[1024];
      fn = avm_variable(v, name, &receiver, key, sizeof(key));
    }
    unsigned count = avm_arguments(v, args);
    if (!v->r->failed)
      avm_call(v, fn, receiver, owner, count, args, true,
               opcode == 0x40 || opcode == 0x53);
    break;
  }
  case 0x3e:
    a = avm_pop(v);
    if (!v->r->failed)
      avm_return(v, a);
    break;
  case 0x41:
    a = avm_pop(v);
    if (avm_name(v, a, name, sizeof(name))) {
      lv_t scopes = avm_scope(v, AVM_SCOPE);
      avm_own_set(v, lv_at(v->r, scopes, 1), name, 0);
    }
    break;
  case 0x42:
  case 0x43: {
    double count = dg_avm1_number(v, avm_pop(v));
    unsigned maximum = opcode == 0x43 ? AVM_ARGUMENTS / 2 : AVM_ARGUMENTS;
    if (!isfinite(count) || count < 0 || count > maximum ||
        floor(count) != count) {
      avm_fail(v, "invalid initializer count");
      break;
    }
    lv_t constructor = dg_avm1_get(v, v->roots[AVM_GLOBAL],
                                   opcode == 0x43 ? "Object" : "Array");
    lv_t object = dg_avm1_object(v, dg_avm1_get(v, constructor, "prototype"));
    for (unsigned i = 0; i < (unsigned)count && !v->r->failed; i++) {
      a = avm_pop(v);
      if (opcode == 0x43)
        avm_name(v, avm_pop(v), name, sizeof(name));
      else
        snprintf(name, sizeof(name), "%u", i);
      avm_own_set(v, object, name, a);
    }
    if (opcode == 0x42) {
      avm_own_set(v, object, "length", lv_num(count));
      dg_avm1_flags(v, object, "length", 1, 0);
    }
    avm_push(v, object);
    break;
  }
  case 0x44: {
    a = avm_pop(v);
    const char *type =
        lv_type(a) == LV_VOID                                ? "undefined"
        : lv_type(a) == LV_AVM_BOOLEAN                       ? "boolean"
        : lv_type(a) == LV_NUMBER                            ? "number"
        : lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL ? "string"
        : dg_avm1_callable(v, a) && !avm_internal(v, a, AVM_SUPER_RECEIVER)
            ? "function"
            : "object";
    b = 0;
    if (lv_type(a) == LV_PROPLIST && v->host.get &&
        v->host.get(v->context, v, a, "__avm_typeof", &b) &&
        lv_type(b) == LV_STRING)
      avm_push(v, b);
    else
      avm_push(v, lv_text(v->r, type, false));
    break;
  }
  case 0x47:
    b = avm_primitive(v, avm_pop(v), false);
    a = avm_primitive(v, avm_pop(v), false);
    if (lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL ||
        lv_type(b) == LV_STRING || lv_type(b) == LV_SYMBOL) {
      a = dg_avm1_string(v, a);
      b = dg_avm1_string(v, b);
      size_t left = strlen(lv_cstr(v->r, a)), right = strlen(lv_cstr(v->r, b));
      if (left + right > 65536) {
        avm_fail(v, "concatenated string exceeds budget");
        break;
      }
      char *text = malloc(left + right + 1);
      if (!text) {
        avm_fail(v, "string allocation failed");
        break;
      }
      memcpy(text, lv_cstr(v->r, a), left);
      memcpy(text + left, lv_cstr(v->r, b), right + 1);
      c = lv_text(v->r, text, false);
      free(text);
      avm_push(v, c);
    } else
      avm_push(v, lv_float(dg_avm1_number(v, a) + dg_avm1_number(v, b)));
    break;
  case 0x48:
  case 0x67:
    b = avm_primitive(v, avm_pop(v), false);
    a = avm_primitive(v, avm_pop(v), false);
    if ((lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL) &&
        (lv_type(b) == LV_STRING || lv_type(b) == LV_SYMBOL)) {
      int order = strcmp(lv_cstr(v->r, a), lv_cstr(v->r, b));
      avm_push(v, dg_avm1_boolean(opcode == 0x48 ? order < 0 : order > 0));
    } else {
      double left = dg_avm1_number(v, a), right = dg_avm1_number(v, b);
      avm_push(v,
               dg_avm1_boolean(opcode == 0x48 ? left < right : left > right));
    }
    break;
  case 0x49:
  case 0x66:
    b = avm_pop(v);
    a = avm_pop(v);
    avm_push(v, dg_avm1_boolean(avm_equal(v, a, b, opcode == 0x66)));
    break;
  case 0x4a:
    avm_push(v, lv_float(dg_avm1_number(v, avm_pop(v))));
    break;
  case 0x4b:
    avm_push(v, dg_avm1_string(v, avm_pop(v)));
    break;
  case 0x4c:
    if (v->stack_top <= f->stack_base)
      avm_fail(v, "duplicate on empty stack");
    else
      avm_push(v, v->roots[2 + v->stack_top - 1]);
    break;
  case 0x4d:
    b = avm_pop(v);
    a = avm_pop(v);
    avm_push(v, b);
    avm_push(v, a);
    break;
  case 0x4e:
    b = avm_pop(v);
    a = avm_pop(v);
    if (avm_name(v, b, name, sizeof(name)))
      avm_push(v, dg_avm1_get(v, a, name));
    break;
  case 0x4f:
    c = avm_pop(v);
    b = avm_pop(v);
    a = avm_pop(v);
    if (avm_name(v, b, name, sizeof(name)))
      dg_avm1_set(v, a, name, c);
    break;
  case 0x50:
  case 0x51:
    a = avm_pop(v);
    avm_push(v, lv_float(dg_avm1_number(v, a) + (opcode == 0x50 ? 1 : -1)));
    break;
  case 0x54: {
    b = avm_pop(v);
    a = avm_pop(v);
    lv_t prototype = dg_avm1_get(v, b, "prototype");
    bool found = false;
    for (unsigned i = 0; lv_type(a) == LV_PROPLIST && i < 256; i++) {
      a = dg_avm1_prototype(v, a);
      if (a && a == prototype) {
        found = true;
        break;
      }
    }
    avm_push(v, dg_avm1_boolean(found));
    break;
  }
  case 0x55:
  case 0x46: {
    a = avm_pop(v);
    if (opcode == 0x46 && avm_name(v, a, name, sizeof(name))) {
      lv_t owner;
      char key[1024];
      a = avm_variable(v, name, &owner, key, sizeof(key));
    }
    lv_t keys = dg_avm1_keys(v, a, true);
    avm_push(v, dg_avm1_null());
    for (unsigned i = 1; i <= lv_count(v->r, keys) && !v->r->failed; i++)
      avm_push(v, lv_at(v->r, keys, i));
    break;
  }
  case 0x60:
  case 0x61:
  case 0x62:
  case 0x63:
  case 0x64:
  case 0x65: {
    b = avm_pop(v);
    a = avm_pop(v);
    uint32_t left = avm_uint(v, a), right = avm_uint(v, b), result;
    unsigned shift = right & 31;
    if (opcode == 0x60)
      result = left & right;
    else if (opcode == 0x61)
      result = left | right;
    else if (opcode == 0x62)
      result = left ^ right;
    else if (opcode == 0x63)
      result = left << shift;
    else if (opcode == 0x65)
      result = left >> shift;
    else
      result =
          (left >> shift) |
          ((left & 0x80000000u) && shift ? (UINT32_MAX << (32 - shift)) : 0);
    avm_push(v, opcode == 0x65 ? lv_num(result) : lv_num((int32_t)result));
    break;
  }
  case 0x69:
    b = avm_pop(v);
    a = avm_pop(v);
    c = dg_avm1_object(v, dg_avm1_get(v, b, "prototype"));
    avm_internal_set(v, c, AVM_CONSTRUCTOR, b);
    avm_own_set(v, c, "constructor", b);
    dg_avm1_flags(v, c, "constructor", 1, 0);
    avm_own_set(v, a, "prototype", c);
    break;
  case 0x81:
    if (size != 2) {
      avm_fail(v, "invalid GotoFrame");
      break;
    }
    a = lv_num(avm_u16(v, data, size, &at) + 1);
    avm_native_call(v, "MovieClip.gotoAndStop", avm_scope(v, AVM_TARGET), 1,
                    &a);
    break;
  case 0x87:
    if (size != 1 || data[0] >= f->register_count ||
        v->stack_top <= f->stack_base)
      avm_fail(v, "invalid StoreRegister");
    else
      v->roots[AVM_REGISTER_ROOT + f->register_base + data[0]] =
          v->roots[2 + v->stack_top - 1];
    break;
  case 0x88:
    f->pool = avm_pool(v, data, size);
    break;
  case 0x8b: {
    const char *path = avm_cstring(v, data, size, &at);
    if (at != size) {
      avm_fail(v, "invalid SetTarget operand");
      break;
    }
    lv_t target =
        !*path ? avm_scope(v, AVM_THIS)
        : v->host.target
            ? v->host.target(v->context, v, avm_scope(v, AVM_TARGET), path)
            : 0;
    if (lv_type(target) != LV_PROPLIST)
      avm_fail(v, "SetTarget target missing");
    else
      *avm_frame_root(v, v->depth - 1, AVM_TARGET) = target;
    break;
  }
  case 0x8e:
  case 0x9b:
    avm_define_function(v, f, opcode, data, size);
    break;
  case 0x96:
    avm_push_data(v, f, data, size);
    break;
  case 0x99:
    avm_branch(v, f, data, size);
    break;
  case 0x9a: {
    b = avm_pop(v);
    a = avm_pop(v);
    if (size != 1 || (data[0] & 0x3c)) {
      avm_fail(v, "invalid GetURL2 flags");
      break;
    }
    lv_t args[] = {a, b, lv_num(data[0])};
    avm_native_call(v, "getURL", avm_scope(v, AVM_TARGET), 3, args);
    break;
  }
  case 0x9d:
    if (dg_avm1_truth(v, avm_pop(v)))
      avm_branch(v, f, data, size);
    else if (size != 2)
      avm_fail(v, "invalid If operand");
    break;
  case 0x9f: {
    if (!size || (data[0] & ~3u) || size != (unsigned)((data[0] & 2) ? 3 : 1)) {
      avm_fail(v, "invalid GotoFrame2 flags");
      break;
    }
    a = avm_pop(v);
    if (data[0] & 2) {
      at = 1;
      a = lv_num(dg_avm1_number(v, a) + avm_u16(v, data, size, &at));
    }
    avm_native_call(
        v, (data[0] & 1) ? "MovieClip.gotoAndPlay" : "MovieClip.gotoAndStop",
        avm_scope(v, AVM_TARGET), 1, &a);
    break;
  }
  default: {
    char message[64];
    snprintf(message, sizeof(message), "unsupported opcode 0x%02x", opcode);
    avm_fail(v, message);
    break;
  }
  }
  v->instruction_roots = previous;
  avm_unroot(v->r, &span);
  v->r->atomic_depth--;
}
static dg_avm1_status_t avm_run_until(dg_avm1_t *v, unsigned budget,
                                      unsigned depth) {
  while (v->depth > depth && budget-- && !v->r->failed)
    avm_step(v);
  lv_collect(v->r);
  return v->r->failed       ? DG_AVM1_ERROR
         : v->depth > depth ? DG_AVM1_BUSY
                            : DG_AVM1_IDLE;
}
dg_avm1_status_t dg_avm1_run(dg_avm1_t *v, unsigned budget) {
  return avm_run_until(v, budget, 0);
}
