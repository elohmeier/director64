// ActionScript 1/2 standard objects and event services. Contract reference:
// Macromedia Flash 8 ActionScript 2.0 Language Reference. Display-object
// methods are forwarded to the owning Flash instance, never emulated by
// content-specific handlers.
#include "avm1_native.h"
#include <ctype.h>
#include <float.h>
#include <limits.h>

#define AN_TIMERS 32u
#define AN_WATCHES 64u
#define AN_ROOTS 448u
#define AN_TIMER_ROOT 32u
#define AN_WATCH_ROOT (AN_TIMER_ROOT + AN_TIMERS * 4u)
#define AN_BUDGET 16384u
enum {
  AN_OBJECT,
  AN_ARRAY,
  AN_STRING,
  AN_NUMBER,
  AN_BOOLEAN,
  AN_MOVIECLIP,
  AN_TEXTFIELD,
  AN_TEXTFORMAT,
  AN_COLOR,
  AN_MATH,
  AN_KEY,
  AN_MOUSE,
  AN_SELECTION,
  AN_CLASSES,
  AN_FOCUS
};
enum { AN_KIND = -16, AN_TARGET = -17, AN_PRIMITIVE = -18, AN_LISTENERS = -19 };
enum {
  AN_ARRAY_KIND = 1,
  AN_COLOR_KIND,
  AN_STRING_KIND,
  AN_NUMBER_KIND,
  AN_BOOLEAN_KIND
};
typedef struct {
  uint64_t due;
  uint32_t id, delay;
  bool busy;
} an_timer_t;
typedef struct {
  bool busy;
} an_watch_t;
struct dg_avm1_library {
  dg_avm1_t *vm;
  unsigned selection_begin, selection_end, selection_caret;
  dg_avm1_host_t downstream;
  void *context;
  lv_root_span_t span;
  lv_t roots[AN_ROOTS];
  an_timer_t timers[AN_TIMERS];
  an_watch_t watches[AN_WATCHES];
  uint64_t clock;
  uint32_t next_timer, random;
  unsigned key, ascii, buttons;
  int mouse_x, mouse_y;
  uint8_t keys[256];
};
static lv_runtime_t *an_values(dg_avm1_library_t *l) {
  return dg_avm1_values(l->vm);
}
static lv_t an_hidden(dg_avm1_library_t *l, lv_t o, int key) {
  lv_runtime_t *r = an_values(l);
  if (lv_type(o) != LV_PROPLIST)
    return 0;
  for (unsigned i = 1; i <= lv_count(r, o); i++) {
    lv_t k = lv_at(r, o, i * 2 - 1);
    if (lv_type(k) == LV_NUMBER && lv_numeric(k) == key)
      return lv_at(r, o, i * 2);
  }
  return 0;
}
static void an_hide(dg_avm1_library_t *l, lv_t o, int key, lv_t value) {
  lv_runtime_t *r = an_values(l);
  unsigned count = lv_count(r, o), at = count * 2 + 1;
  for (unsigned i = 1; i <= count; i++) {
    lv_t k = lv_at(r, o, i * 2 - 1);
    if (lv_type(k) == LV_NUMBER && lv_numeric(k) == key) {
      at = i * 2 - 1;
      break;
    }
  }
  lv_set_at(r, o, at, lv_num(key));
  lv_set_at(r, o, at + 1, value);
}
static unsigned an_kind(dg_avm1_library_t *l, lv_t o) {
  for (unsigned depth = 0; o && depth < 256; depth++) {
    lv_t k = an_hidden(l, o, AN_KIND);
    if (lv_type(k) == LV_NUMBER)
      return (unsigned)lv_numeric(k);
    o = dg_avm1_prototype(l->vm, o);
  }
  return 0;
}
static lv_t an_text(dg_avm1_library_t *l, const char *s) {
  return lv_text(an_values(l), s, false);
}
static const char *an_string(dg_avm1_library_t *l, lv_t o) {
  return lv_cstr(an_values(l), dg_avm1_string(l->vm, o));
}
static double an_number(dg_avm1_library_t *l, lv_t o) {
  return dg_avm1_number(l->vm, o);
}
static int32_t an_int(dg_avm1_library_t *l, lv_t o) {
  double n = an_number(l, o);
  if (!isfinite(n) || !n)
    return 0;
  double u = fmod(trunc(n), 4294967296.0);
  if (u < 0)
    u += 4294967296.0;
  return u < 2147483648.0 ? (int32_t)u : (int32_t)(u - 4294967296.0);
}
static lv_t an_get_index(dg_avm1_library_t *l, lv_t o, unsigned i) {
  char key[16];
  snprintf(key, sizeof(key), "%u", i);
  return dg_avm1_get(l->vm, o, key);
}
static void an_set_index(dg_avm1_library_t *l, lv_t o, unsigned i, lv_t v) {
  char key[16];
  snprintf(key, sizeof(key), "%u", i);
  dg_avm1_own_set(l->vm, o, key, v);
}
static void an_delete_index(dg_avm1_library_t *l, lv_t o, unsigned i) {
  char key[16];
  snprintf(key, sizeof(key), "%u", i);
  dg_avm1_delete(l->vm, o, key);
}
static unsigned an_length(dg_avm1_library_t *l, lv_t o) {
  double n = an_number(l, dg_avm1_get(l->vm, o, "length"));
  if (!isfinite(n) || n < 0 || n > 65536) {
    lv_fail(an_values(l), "AVM1 array exceeds 65536 elements");
    return 0;
  }
  return (unsigned)n;
}
static lv_t an_array(dg_avm1_library_t *l, unsigned n, const lv_t *values) {
  lv_t o = dg_avm1_object(l->vm, l->roots[AN_ARRAY]);
  an_hide(l, o, AN_KIND, lv_num(AN_ARRAY_KIND));
  dg_avm1_own_set(l->vm, o, "length", lv_num(n));
  dg_avm1_flags(l->vm, o, "length", 1, 0);
  if (values)
    for (unsigned i = 0; i < n; i++)
      an_set_index(l, o, i, values[i]);
  return o;
}
static bool an_index(const char *key, unsigned *out) {
  if (!*key || (*key == '0' && key[1]))
    return false;
  uint64_t n = 0;
  for (const char *p = key; *p; p++) {
    if (*p < '0' || *p > '9')
      return false;
    n = n * 10 + (unsigned)(*p - '0');
    if (n > 65535)
      return false;
  }
  *out = (unsigned)n;
  return true;
}
#include "avm1_string.inc"
static bool an_get(void *context, dg_avm1_t *v, lv_t object, const char *key,
                   lv_t *out) {
  dg_avm1_library_t *l = context;
  if (l->downstream.get && l->downstream.get(l->context, v, object, key, out))
    return true;
  lv_type_t type = lv_type(object);
  if (type == LV_STRING || type == LV_SYMBOL ||
      an_kind(l, object) == AN_STRING_KIND) {
    if (!strcmp(key, "length")) {
      unsigned count = 0;
      lv_t primitive = an_hidden(l, object, AN_PRIMITIVE);
      uint16_t *units = an_units(l, primitive ? primitive : object, &count);
      free(units);
      *out = lv_num(count);
      return true;
    }
    return dg_avm1_own_get(v, l->roots[AN_STRING], key, out);
  }
  if (type == LV_NUMBER)
    return dg_avm1_own_get(v, l->roots[AN_NUMBER], key, out);
  if (type == LV_AVM_BOOLEAN)
    return dg_avm1_own_get(v, l->roots[AN_BOOLEAN], key, out);
  return false;
}
static bool an_set(void *context, dg_avm1_t *v, lv_t object, const char *key,
                   lv_t value) {
  dg_avm1_library_t *l = context;
  if (!strcmp(key, "__proto__"))
    return false;
  lv_runtime_t *r = an_values(l);
  for (unsigned i = 0; i < AN_WATCHES; i++) {
    lv_t *w = &l->roots[AN_WATCH_ROOT + i * 4];
    if (w[0] != object || !w[1] || l->watches[i].busy ||
        strcmp(lv_cstr(r, w[1]), key))
      continue;
    lv_t args[] = {w[1], dg_avm1_get(v, object, key), value, w[3]},
         replacement = 0;
    l->watches[i].busy = true;
    bool ok = dg_avm1_invoke(v, w[2], object, 4, args, AN_BUDGET, &replacement);
    l->watches[i].busy = false;
    if (!ok)
      return true;
    value = replacement;
  }
  if (l->downstream.set && l->downstream.set(l->context, v, object, key, value))
    return true;
  if (an_kind(l, object) == AN_ARRAY_KIND) {
    unsigned length = an_length(l, object), index = 0;
    if (!strcmp(key, "length")) {
      double n = an_number(l, value);
      if (!isfinite(n) || n < 0 || n > 65536 || n != trunc(n)) {
        lv_fail(r, "AVM1 invalid array length");
        return true;
      }
      for (unsigned i = (unsigned)n; i < length; i++)
        an_delete_index(l, object, i);
      dg_avm1_own_set(v, object, key, lv_num(n));
      return true;
    }
    if (an_index(key, &index) && index >= length)
      dg_avm1_own_set(v, object, "length", lv_num(index + 1));
  }
  dg_avm1_own_set(v, object, key, value);
  return true;
}
static lv_t an_target(void *context, dg_avm1_t *v, lv_t o, const char *path) {
  dg_avm1_library_t *l = context;
  return l->downstream.target ? l->downstream.target(l->context, v, o, path)
                              : 0;
}
static void an_trace(void *context, const char *text) {
  dg_avm1_library_t *l = context;
  if (l->downstream.trace)
    l->downstream.trace(l->context, text);
}
static lv_t an_listener_list(dg_avm1_library_t *l, lv_t o) {
  lv_t list = an_hidden(l, o, AN_LISTENERS);
  if (!list) {
    list = lv_list(an_values(l), 0, NULL, false);
    an_hide(l, o, AN_LISTENERS, list);
  }
  return list;
}
bool dg_avm1_library_event(dg_avm1_library_t *l, lv_t object, const char *name,
                           unsigned argc, const lv_t *args) {
  lv_t fn = dg_avm1_get(l->vm, object, name), out = 0;
  if (!dg_avm1_callable(l->vm, fn))
    return true;
  return dg_avm1_invoke(l->vm, fn, object, argc, args, AN_BUDGET, &out);
}
static void an_broadcast(dg_avm1_library_t *l, lv_t object, const char *event,
                         unsigned argc, const lv_t *args) {
  lv_runtime_t *r = an_values(l);
  lv_t list = an_hidden(l, object, AN_LISTENERS);
  // Retain the event's original listener set even if callbacks remove peers.
  lv_t copy[64];
  unsigned n = lv_type(list) == LV_LIST ? lv_count(r, list) : 0;
  if (n > 64)
    n = 64;
  for (unsigned i = 0; i < n; i++)
    copy[i] = lv_at(r, list, i + 1);
  lv_root_span_t span = {r->external_roots, copy, n};
  r->external_roots = &span;
  for (unsigned i = 0; i < n && !r->failed; i++)
    dg_avm1_library_event(l, copy[i], event, argc, args);
  r->external_roots = span.next;
}
static bool an_math(dg_avm1_library_t *l, const char *name, unsigned n,
                    const lv_t *a, lv_t *out) {
  double x = n ? an_number(l, a[0]) : NAN, y = n > 1 ? an_number(l, a[1]) : NAN,
         result = NAN;
  if (!strcmp(name, "abs"))
    result = fabs(x);
  else if (!strcmp(name, "floor"))
    result = floor(x);
  else if (!strcmp(name, "ceil"))
    result = ceil(x);
  else if (!strcmp(name, "round"))
    result = x < 0 && x >= -0.5 ? -0.0 : floor(x + 0.5);
  else if (!strcmp(name, "sqrt"))
    result = sqrt(x);
  else if (!strcmp(name, "sin"))
    result = sin(x);
  else if (!strcmp(name, "cos"))
    result = cos(x);
  else if (!strcmp(name, "tan"))
    result = tan(x);
  else if (!strcmp(name, "asin"))
    result = asin(x);
  else if (!strcmp(name, "acos"))
    result = acos(x);
  else if (!strcmp(name, "atan"))
    result = atan(x);
  else if (!strcmp(name, "atan2"))
    result = atan2(x, y);
  else if (!strcmp(name, "pow"))
    result = pow(x, y);
  else if (!strcmp(name, "exp"))
    result = exp(x);
  else if (!strcmp(name, "log"))
    result = log(x);
  else if (!strcmp(name, "min") || !strcmp(name, "max")) {
    bool min = !strcmp(name, "min");
    result = min ? INFINITY : -INFINITY;
    for (unsigned i = 0; i < n; i++) {
      double z = an_number(l, a[i]);
      if (isnan(z)) {
        result = NAN;
        break;
      }
      if (min ? z < result : z > result)
        result = z;
    }
  } else if (!strcmp(name, "random")) {
    l->random = l->random * 1664525u + 1013904223u;
    result = (double)l->random / 4294967296.0;
  } else
    return false;
  *out = lv_num(result);
  return true;
}
static bool an_array_call(dg_avm1_library_t *l, const char *name, lv_t object,
                          unsigned n, const lv_t *a, lv_t *out) {
  dg_avm1_t *v = l->vm;
  unsigned length = an_length(l, object);
  if (!strcmp(name, "push") || !strcmp(name, "unshift")) {
    if (n > 65536 - length) {
      lv_fail(an_values(l), "AVM1 array exceeds 65536 elements");
      return true;
    }
    if (!strcmp(name, "unshift"))
      for (unsigned i = length; i; i--)
        an_set_index(l, object, i + n - 1, an_get_index(l, object, i - 1));
    for (unsigned i = 0; i < n; i++)
      an_set_index(l, object, !strcmp(name, "push") ? length + i : i, a[i]);
    dg_avm1_own_set(v, object, "length", lv_num(length + n));
    *out = lv_num(length + n);
    return true;
  }
  if (!strcmp(name, "pop") || !strcmp(name, "shift")) {
    if (!length) {
      *out = 0;
      return true;
    }
    bool shift = !strcmp(name, "shift");
    *out = an_get_index(l, object, shift ? 0 : length - 1);
    if (shift)
      for (unsigned i = 1; i < length; i++)
        an_set_index(l, object, i - 1, an_get_index(l, object, i));
    an_delete_index(l, object, length - 1);
    dg_avm1_own_set(v, object, "length", lv_num(length - 1));
    return true;
  }
  if (!strcmp(name, "reverse")) {
    for (unsigned i = 0; i < length / 2; i++) {
      lv_t x = an_get_index(l, object, i);
      an_set_index(l, object, i, an_get_index(l, object, length - 1 - i));
      an_set_index(l, object, length - 1 - i, x);
    }
    *out = object;
    return true;
  }
  if (!strcmp(name, "slice") || !strcmp(name, "splice")) {
    int start = n ? an_int(l, a[0]) : 0;
    if (start < 0)
      start += (int)length;
    if (start < 0)
      start = 0;
    if ((unsigned)start > length)
      start = (int)length;
    int end = n > 1 ? an_int(l, a[1]) : (int)length;
    bool splice = !strcmp(name, "splice");
    if (splice) {
      if (end < 0)
        end = 0;
      end += (int)start;
    } else if (end < 0)
      end += (int)length;
    if (end < start)
      end = start;
    if ((unsigned)end > length)
      end = (int)length;
    unsigned removed = (unsigned)(end - start);
    *out = an_array(l, removed, NULL);
    for (unsigned i = 0; i < removed; i++)
      an_set_index(l, *out, i, an_get_index(l, object, (unsigned)start + i));
    if (splice) {
      unsigned added = n > 2 ? n - 2 : 0, new_length = length - removed + added;
      if (new_length > 65536) {
        lv_fail(an_values(l), "AVM1 array exceeds 65536 elements");
        return true;
      }
      if (added > removed)
        for (unsigned i = length; i > (unsigned)end; i--)
          an_set_index(l, object, i - 1 + added - removed,
                       an_get_index(l, object, i - 1));
      else
        for (unsigned i = (unsigned)end; i < length; i++)
          an_set_index(l, object, i - removed + added,
                       an_get_index(l, object, i));
      for (unsigned i = 0; i < added; i++)
        an_set_index(l, object, (unsigned)start + i, a[i + 2]);
      for (unsigned i = new_length; i < length; i++)
        an_delete_index(l, object, i);
      dg_avm1_own_set(v, object, "length", lv_num(new_length));
    }
    return true;
  }
  if (!strcmp(name, "join") || !strcmp(name, "toString")) {
    const char *sep = n ? an_string(l, a[0]) : ",";
    size_t sep_n = strlen(sep), used = 0, capacity = 64;
    char *buf = malloc(capacity);
    if (!buf) {
      lv_fail(an_values(l), "AVM1 string allocation failed");
      return true;
    }
    buf[0] = 0;
    for (unsigned i = 0; i < length; i++) {
      lv_t item = an_get_index(l, object, i);
      const char *s =
          (!item || lv_type(item) == LV_AVM_NULL) ? "" : an_string(l, item);
      size_t z = strlen(s), need = used + (i ? sep_n : 0) + z + 1;
      if (need > 1024 * 1024) {
        free(buf);
        lv_fail(an_values(l), "AVM1 string exceeds 1 MiB");
        return true;
      }
      if (need > capacity) {
        capacity = need * 2;
        char *next = realloc(buf, capacity);
        if (!next) {
          free(buf);
          lv_fail(an_values(l), "AVM1 string allocation failed");
          return true;
        }
        buf = next;
      }
      if (i) {
        memcpy(buf + used, sep, sep_n);
        used += sep_n;
      }
      memcpy(buf + used, s, z + 1);
      used += z;
    }
    *out = an_text(l, buf);
    free(buf);
    return true;
  }
  if (!strcmp(name, "concat")) {
    *out = an_array(l, 0, NULL);
    unsigned at = 0;
    for (unsigned k = 0; k <= n; k++) {
      lv_t item = k ? a[k - 1] : object;
      bool array = an_kind(l, item) == AN_ARRAY_KIND;
      unsigned count = array ? an_length(l, item) : 1;
      if (count > 65536 - at) {
        lv_fail(an_values(l), "AVM1 array exceeds 65536 elements");
        return true;
      }
      for (unsigned j = 0; j < count; j++)
        an_set_index(l, *out, at++, array ? an_get_index(l, item, j) : item);
    }
    dg_avm1_own_set(v, *out, "length", lv_num(at));
    return true;
  }
  return false;
}
// String positions use ToInteger, without the 32-bit wrap of bitwise APIs.
static int an_string_index(dg_avm1_library_t *l, lv_t value) {
  double n = an_number(l, value);
  if (isnan(n))
    return 0;
  if (n >= INT_MAX)
    return INT_MAX;
  if (n <= INT_MIN)
    return INT_MIN;
  return (int)trunc(n);
}
static bool an_string_call(dg_avm1_library_t *l, const char *name, lv_t object,
                           unsigned n, const lv_t *a, lv_t *out) {
  lv_t primitive = an_hidden(l, object, AN_PRIMITIVE);
  if (!strcmp(name, "valueOf") || !strcmp(name, "toString")) {
    *out = dg_avm1_string(l->vm, primitive ? primitive : object);
    return true;
  }
  unsigned size = 0;
  uint16_t *source = an_units(l, primitive ? primitive : object, &size);
  if (!source)
    return true;
  int start = n ? an_string_index(l, a[0]) : 0;
  bool handled = true;
  if (!strcmp(name, "charAt") || !strcmp(name, "charCodeAt")) {
    if (start < 0 || (unsigned)start >= size)
      *out = !strcmp(name, "charAt") ? an_text(l, "") : lv_num(NAN);
    else
      *out = !strcmp(name, "charAt") ? an_units_text(l, source + start, 1)
                                     : lv_num(source[start]);
  } else if (!strcmp(name, "indexOf") || !strcmp(name, "lastIndexOf")) {
    unsigned length = 0;
    uint16_t *needle = an_units(l, n ? a[0] : 0, &length);
    bool reverse = !strcmp(name, "lastIndexOf");
    int from = n > 1 ? an_string_index(l, a[1]) : (reverse ? (int)size : 0),
        found = -1;
    if (from < 0)
      from = 0;
    if ((unsigned)from > size)
      from = (int)size;
    if (needle)
      for (int i = from; i >= 0 && (unsigned)i <= size; i += reverse ? -1 : 1)
        if (length <= size - (unsigned)i &&
            !memcmp(source + i, needle, length * sizeof(*source))) {
          found = i;
          break;
        }
    free(needle);
    *out = lv_num(found);
  } else if (!strcmp(name, "substring") || !strcmp(name, "subString") ||
             !strcmp(name, "substr") || !strcmp(name, "slice")) {
    bool substr = !strcmp(name, "substr"), slice = !strcmp(name, "slice");
    int64_t first = start,
            last = n > 1 ? an_string_index(l, a[1]) : (int64_t)size;
    if ((substr || slice) && first < 0)
      first += size;
    if (first < 0)
      first = 0;
    if (first > size)
      first = size;
    if (substr) {
      if (last < 0)
        last = 0;
      last += first;
    } else if (slice && last < 0)
      last += size;
    if (last < 0)
      last = 0;
    if (last > size)
      last = size;
    if (last < first) {
      if (slice)
        last = first;
      else {
        int64_t swap = first;
        first = last;
        last = swap;
      }
    }
    *out = an_units_text(l, source + first, (unsigned)(last - first));
  } else if (!strcmp(name, "toLowerCase") || !strcmp(name, "toUpperCase"))
    *out = an_units_case(l, source, size, !strcmp(name, "toLowerCase"));
  else
    handled = false;
  free(source);
  return handled;
}
static bool an_replace_text(dg_avm1_library_t *l, lv_t object, bool selection,
                            unsigned argc, const lv_t *args) {
  if (argc < (selection ? 1u : 3u) ||
      (selection && l->roots[AN_FOCUS] != object))
    return true;
  unsigned size = 0, added = 0;
  uint16_t *old = an_units(l, dg_avm1_get(l->vm, object, "text"), &size);
  uint16_t *insert = an_units(l, args[selection ? 0 : 2], &added);
  if (!old || !insert) {
    free(old);
    free(insert);
    return true;
  }
  int64_t begin = selection ? (int64_t)l->selection_begin : an_int(l, args[0]);
  int64_t end = selection ? (int64_t)l->selection_end : an_int(l, args[1]);
  if (begin < 0)
    begin = 0;
  if (end < 0)
    end = 0;
  if (begin > size)
    begin = size;
  if (end > size)
    end = size;
  if (end < begin)
    end = begin;
  size_t length = size - (size_t)(end - begin) + added;
  if (length > 1024 * 1024) {
    lv_fail(an_values(l), "AVM1 text replacement exceeds 1 MiB");
    free(old);
    free(insert);
    return true;
  }
  uint16_t *result = malloc((length + 1) * sizeof(*result));
  if (!result)
    lv_fail(an_values(l), "AVM1 text replacement allocation failed");
  else {
    memcpy(result, old, (size_t)begin * sizeof(*result));
    memcpy(result + begin, insert, added * sizeof(*result));
    memcpy(result + begin + added, old + end,
           (size - (size_t)end) * sizeof(*result));
    dg_avm1_set(l->vm, object, "text",
                an_units_text(l, result, (unsigned)length));
    if (selection)
      l->selection_begin = l->selection_end = l->selection_caret =
          (unsigned)begin + added;
    free(result);
  }
  free(old);
  free(insert);
  return true;
}
static bool an_call(void *context, dg_avm1_t *v, const char *name, lv_t object,
                    unsigned n, const lv_t *a, lv_t *out) {
  dg_avm1_library_t *l = context;
  lv_runtime_t *r = an_values(l);
  *out = 0;
  if (!strcmp(name, "TextField.replaceText") ||
      !strcmp(name, "TextField.replaceSel"))
    return an_replace_text(l, object, !strcmp(name, "TextField.replaceSel"), n,
                           a);
  if (!strncmp(name, "Math.", 5))
    return an_math(l, name + 5, n, a, out);
  if (!strncmp(name, "Array.", 6))
    return an_array_call(l, name + 6, object, n, a, out);
  if (!strncmp(name, "String.", 7))
    return an_string_call(l, name + 7, object, n, a, out);
  if (!strcmp(name, "Object")) {
    *out = n && lv_type(a[0]) == LV_PROPLIST
               ? a[0]
               : dg_avm1_object(v, l->roots[AN_OBJECT]);
    return true;
  }
  if (!strcmp(name, "MovieClip") || !strcmp(name, "TextField")) {
    // AS1 prototype inheritance constructs a display-class prototype before
    // any display-list instance exists (for example new MovieClip()).
    *out = dg_avm1_object(
        v, l->roots[!strcmp(name, "MovieClip") ? AN_MOVIECLIP : AN_TEXTFIELD]);
    return true;
  }
  if (!strcmp(name, "Array")) {
    if (n == 1 && lv_type(a[0]) == LV_NUMBER) {
      double len = an_number(l, a[0]);
      if (len < 0 || len > 65536 || !isfinite(len) || len != trunc(len)) {
        lv_fail(r, "AVM1 invalid array length");
        return true;
      }
      *out = an_array(l, (unsigned)len, NULL);
    } else
      *out = an_array(l, n, a);
    return true;
  }
  if (!strcmp(name, "String")) {
    *out = n ? dg_avm1_string(v, a[0]) : an_text(l, "");
    if (an_kind(l, object) == AN_STRING_KIND) {
      an_hide(l, object, AN_PRIMITIVE, *out);
      *out = object;
    }
    return true;
  }
  if (!strcmp(name, "Number")) {
    *out = lv_num(n ? an_number(l, a[0]) : 0);
    if (an_kind(l, object) == AN_NUMBER_KIND) {
      an_hide(l, object, AN_PRIMITIVE, *out);
      *out = object;
    }
    return true;
  }
  if (!strcmp(name, "Boolean")) {
    *out = dg_avm1_boolean(n && dg_avm1_truth(v, a[0]));
    if (an_kind(l, object) == AN_BOOLEAN_KIND) {
      an_hide(l, object, AN_PRIMITIVE, *out);
      *out = object;
    }
    return true;
  }
  if (!strcmp(name, "TextFormat")) {
    static const char *keys[] = {
        "font",        "size",   "color",  "bold",  "italic",
        "underline",   "url",    "target", "align", "leftMargin",
        "rightMargin", "indent", "leading"};
    *out = dg_avm1_object(v, l->roots[AN_TEXTFORMAT]);
    for (unsigned i = 0; i < n && i < sizeof(keys) / sizeof(*keys); i++)
      dg_avm1_set(v, *out, keys[i], a[i]);
    return true;
  }
  if (!strcmp(name, "Color")) {
    *out = dg_avm1_object(v, l->roots[AN_COLOR]);
    lv_t target = n ? a[0] : 0;
    if (lv_type(target) == LV_STRING)
      target = an_target(l, v, object, lv_cstr(r, target));
    an_hide(l, *out, AN_KIND, lv_num(AN_COLOR_KIND));
    an_hide(l, *out, AN_TARGET, target);
    return true;
  }
  if (!strncmp(name, "Color.", 6)) {
    lv_t target = an_hidden(l, object, AN_TARGET);
    return l->downstream.call &&
           l->downstream.call(l->context, v, name, target, n, a, out);
  }
  if (!strcmp(name, "Object.valueOf")) {
    lv_t x = an_hidden(l, object, AN_PRIMITIVE);
    *out = x ? x : object;
    return true;
  }
  if (!strcmp(name, "Object.toString")) {
    *out = an_text(l, "[object Object]");
    return true;
  }
  if (!strcmp(name, "Number.valueOf") || !strcmp(name, "Boolean.valueOf")) {
    lv_t x = an_hidden(l, object, AN_PRIMITIVE);
    *out = x ? x : object;
    return true;
  }
  if (!strcmp(name, "Number.toString") || !strcmp(name, "Boolean.toString")) {
    lv_t x = an_hidden(l, object, AN_PRIMITIVE);
    *out = dg_avm1_string(v, x ? x : object);
    return true;
  }
  if (!strcmp(name, "Object.hasOwnProperty") ||
      !strcmp(name, "Object.isPropertyEnumerable")) {
    lv_t value = 0;
    bool found = n && dg_avm1_own_get(v, object, an_string(l, a[0]), &value);
    if (found && !strcmp(name, "Object.isPropertyEnumerable")) {
      lv_t keys = dg_avm1_keys(v, object, false);
      found = false;
      for (unsigned i = 1; i <= lv_count(r, keys); i++)
        if (!strcmp(lv_cstr(r, lv_at(r, keys, i)), an_string(l, a[0]))) {
          found = true;
          break;
        }
    }
    *out = dg_avm1_boolean(found);
    return true;
  }
  if (!strcmp(name, "Object.isPrototypeOf")) {
    lv_t p = n ? dg_avm1_prototype(v, a[0]) : 0;
    unsigned depth = 0;
    while (p && p != object && depth++ < 256)
      p = dg_avm1_prototype(v, p);
    *out = dg_avm1_boolean(p == object);
    return true;
  }
  if (!strcmp(name, "Object.registerClass")) {
    if (n >= 2) {
      const char *key = an_string(l, a[0]);
      if (lv_type(a[1]) == LV_AVM_NULL || !a[1])
        dg_avm1_delete(v, l->roots[AN_CLASSES], key);
      else
        dg_avm1_own_set(v, l->roots[AN_CLASSES], key, a[1]);
    }
    *out = dg_avm1_boolean(n >= 2);
    return true;
  }
  if (!strcmp(name, "ASSetPropFlags")) {
    if (n < 3 || lv_type(a[0]) != LV_PROPLIST)
      return true;
    unsigned set = (unsigned)an_int(l, a[2]),
             clear = n > 3 ? (unsigned)an_int(l, a[3]) : 0;
    if (!a[1] || lv_type(a[1]) == LV_AVM_NULL)
      dg_avm1_flags(v, a[0], NULL, set, clear);
    else if (an_kind(l, a[1]) == AN_ARRAY_KIND) {
      unsigned count = an_length(l, a[1]);
      for (unsigned i = 0; i < count; i++)
        dg_avm1_flags(v, a[0], an_string(l, an_get_index(l, a[1], i)), set,
                      clear);
    } else {
      const char *text = an_string(l, a[1]);
      char key[256];
      do {
        const char *end = strchr(text, ',');
        size_t len = end ? (size_t)(end - text) : strlen(text);
        if (len >= sizeof(key)) {
          lv_fail(r, "AVM1 property name exceeds 255 bytes");
          return true;
        }
        memcpy(key, text, len);
        key[len] = 0;
        dg_avm1_flags(v, a[0], key, set, clear);
        if (!end)
          break;
        text = end + 1;
      } while (*text);
    }
    return true;
  }
  if (!strcmp(name, "Object.watch") || !strcmp(name, "Object.unwatch")) {
    if (!n) {
      *out = dg_avm1_boolean(false);
      return true;
    }
    const char *key = an_string(l, a[0]);
    unsigned free_slot = AN_WATCHES, found = AN_WATCHES;
    for (unsigned i = 0; i < AN_WATCHES; i++) {
      lv_t *w = &l->roots[AN_WATCH_ROOT + i * 4];
      if (!w[0]) {
        if (free_slot == AN_WATCHES)
          free_slot = i;
      } else if (w[0] == object && !strcmp(lv_cstr(r, w[1]), key)) {
        found = i;
        break;
      }
    }
    if (!strcmp(name, "Object.unwatch")) {
      if (found < AN_WATCHES)
        memset(&l->roots[AN_WATCH_ROOT + found * 4], 0, 4 * sizeof(lv_t));
      *out = dg_avm1_boolean(found < AN_WATCHES);
      return true;
    }
    if (n < 2 || !dg_avm1_callable(v, a[1])) {
      *out = dg_avm1_boolean(false);
      return true;
    }
    unsigned slot = found < AN_WATCHES ? found : free_slot;
    if (slot == AN_WATCHES) {
      lv_fail(r, "AVM1 watch limit (64) exceeded");
      return true;
    }
    lv_t *w = &l->roots[AN_WATCH_ROOT + slot * 4];
    w[0] = object;
    w[1] = an_text(l, key);
    w[2] = a[1];
    w[3] = n > 2 ? a[2] : 0;
    *out = dg_avm1_boolean(true);
    return true;
  }
  if (!strcmp(name, "addListener") || !strcmp(name, "removeListener")) {
    lv_t list = an_listener_list(l, object);
    unsigned length = lv_type(list) == LV_LIST ? lv_count(r, list) : 0;
    if (!n) {
      *out = dg_avm1_boolean(false);
      return true;
    }
    unsigned found = length;
    for (unsigned i = 0; i < length; i++)
      if (lv_at(r, list, i + 1) == a[0]) {
        found = i;
        break;
      }
    if (!strcmp(name, "addListener")) {
      if (found == length) {
        if (length == 64) {
          lv_fail(r, "AVM1 listener limit (64) exceeded");
          return true;
        }
        lv_set_at(r, list, length + 1, a[0]);
      }
      *out = dg_avm1_boolean(true);
    } else {
      if (found < length) {
        lv_t copy[64];
        for (unsigned i = 0, j = 0; i < length; i++)
          if (i != found)
            copy[j++] = lv_at(r, list, i + 1);
        an_hide(l, object, AN_LISTENERS, lv_list(r, length - 1, copy, false));
      }
      *out = dg_avm1_boolean(found < length);
    }
    return true;
  }
  if (!strcmp(name, "setInterval")) {
    bool method = n > 1 && !dg_avm1_callable(v, a[0]);
    unsigned begin = method ? 3 : 2;
    if (n < begin || (method && lv_type(a[1]) != LV_STRING) ||
        (!method && !dg_avm1_callable(v, a[0]))) {
      lv_fail(r, "AVM1 invalid setInterval arguments");
      return true;
    }
    double ms = an_number(l, a[begin - 1]);
    if (!isfinite(ms) || ms < 1)
      ms = 1;
    if (ms > UINT32_MAX)
      ms = UINT32_MAX;
    unsigned i = 0;
    while (i < AN_TIMERS && l->timers[i].id)
      i++;
    if (i == AN_TIMERS) {
      lv_fail(r, "AVM1 interval limit (32) exceeded");
      return true;
    }
    lv_t *t = &l->roots[AN_TIMER_ROOT + i * 4];
    t[0] = method ? a[0] : dg_avm1_globals(v);
    t[1] = method ? a[1] : a[0];
    t[2] = lv_list(r, n - begin, a + begin, false);
    t[3] = dg_avm1_boolean(method);
    uint32_t id = ++l->next_timer;
    if (!id)
      id = ++l->next_timer;
    l->timers[i] =
        (an_timer_t){l->clock + (uint32_t)ms, id, (uint32_t)ms, false};
    *out = lv_num(id);
    return true;
  }
  if (!strcmp(name, "clearInterval")) {
    uint32_t id = n ? (uint32_t)an_int(l, a[0]) : 0;
    for (unsigned i = 0; i < AN_TIMERS; i++)
      if (id && l->timers[i].id == id) {
        memset(&l->timers[i], 0, sizeof(l->timers[i]));
        memset(&l->roots[AN_TIMER_ROOT + i * 4], 0, 4 * sizeof(lv_t));
      }
    return true;
  }
  if (!strcmp(name, "getTimer")) {
    *out = lv_num((uint32_t)l->clock);
    return true;
  }
  if (!strcmp(name, "Key.getCode")) {
    *out = lv_num(l->key);
    return true;
  }
  if (!strcmp(name, "Key.getAscii")) {
    *out = lv_num(l->ascii);
    return true;
  }
  if (!strcmp(name, "Key.isDown")) {
    int code = n ? an_int(l, a[0]) : -1;
    *out = dg_avm1_boolean(code >= 0 && code < 256 && l->keys[code]);
    return true;
  }
  if (!strcmp(name, "Selection.setSelection")) {
    if (l->roots[AN_FOCUS] && n >= 2) {
      unsigned size = 0;
      uint16_t *units =
          an_units(l, dg_avm1_get(v, l->roots[AN_FOCUS], "text"), &size);
      free(units);
      int begin = an_int(l, a[0]), end = an_int(l, a[1]);
      if (begin < 0)
        begin = 0;
      if (end < 0)
        end = 0;
      if ((unsigned)begin > size)
        begin = (int)size;
      if ((unsigned)end > size)
        end = (int)size;
      l->selection_caret = (unsigned)end;
      l->selection_begin = (unsigned)(begin < end ? begin : end);
      l->selection_end = (unsigned)(begin < end ? end : begin);
    }
    return true;
  }
  if (!strcmp(name, "Selection.getBeginIndex") ||
      !strcmp(name, "Selection.getEndIndex") ||
      !strcmp(name, "Selection.getCaretIndex")) {
    *out = lv_num(
        !l->roots[AN_FOCUS]                        ? -1
        : !strcmp(name, "Selection.getBeginIndex") ? (int)l->selection_begin
        : !strcmp(name, "Selection.getEndIndex")   ? (int)l->selection_end
                                                   : (int)l->selection_caret);
    return true;
  }
  if (!strcmp(name, "Selection.getFocus")) {
    *out = l->roots[AN_FOCUS] ? dg_avm1_get(v, l->roots[AN_FOCUS], "_target")
                              : dg_avm1_null();
    return true;
  }
  if (!strcmp(name, "Selection.setFocus")) {
    lv_t next = n ? a[0] : 0;
    if (lv_type(next) == LV_STRING)
      next = an_target(l, v, object, lv_cstr(r, next));
    if (lv_type(next) == LV_AVM_NULL)
      next = 0;
    if (next && lv_type(next) != LV_PROPLIST) {
      *out = dg_avm1_boolean(false);
      return true;
    }
    lv_t old = l->roots[AN_FOCUS];
    l->roots[AN_FOCUS] = next;
    if (old != next) {
      l->selection_begin = l->selection_end = l->selection_caret = 0;
      lv_t a1[] = {next};
      dg_avm1_library_event(l, old, "onKillFocus", 1, a1);
      a1[0] = old;
      dg_avm1_library_event(l, next, "onSetFocus", 1, a1);
      lv_t args[] = {old, next};
      an_broadcast(l, l->roots[AN_SELECTION], "onSetFocus", 2, args);
    }
    *out = dg_avm1_boolean(true);
    return true;
  }
  if (!strcmp(name, "isNaN")) {
    *out = dg_avm1_boolean(!n || isnan(an_number(l, a[0])));
    return true;
  }
  if (!strcmp(name, "isFinite")) {
    *out = dg_avm1_boolean(n && isfinite(an_number(l, a[0])));
    return true;
  }
  if (!strcmp(name, "parseInt") || !strcmp(name, "parseFloat")) {
    const char *s = n ? an_string(l, a[0]) : "";
    char *end = NULL;
    double result;
    if (!strcmp(name, "parseInt")) {
      int radix = n > 1 ? an_int(l, a[1]) : 0;
      if (radix && (radix < 2 || radix > 36)) {
        *out = lv_num(NAN);
        return true;
      }
      result = (double)strtoll(s, &end, radix);
    } else
      result = strtod(s, &end);
    *out = lv_num(end == s ? NAN : result);
    return true;
  }
  if (!strcmp(name, "updateAfterEvent"))
    return true; // Host renders after service.
  if (!strcmp(name, "trace")) {
    an_trace(l, n ? an_string(l, a[0]) : "undefined");
    return true;
  }
  return l->downstream.call &&
         l->downstream.call(l->context, v, name, object, n, a, out);
}
dg_avm1_host_t dg_avm1_library_host(void) {
  return (dg_avm1_host_t){an_get, an_set, an_call, an_target, an_trace};
}
dg_avm1_library_t *dg_avm1_library_create(dg_avm1_host_t downstream,
                                          void *context) {
  dg_avm1_library_t *l = calloc(1, sizeof(*l));
  if (!l)
    return NULL;
  l->downstream = downstream;
  l->context = context;
  l->random = 0x6d2b79f5;
  return l;
}
static void an_method(dg_avm1_library_t *l, lv_t target, const char *property,
                      const char *native) {
  dg_avm1_own_set(l->vm, target, property, dg_avm1_native(l->vm, native));
}
static void an_methods(dg_avm1_library_t *l, lv_t target, const char *prefix,
                       const char *const *names, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    char name[96];
    snprintf(name, sizeof(name), "%s%s%s", prefix, *prefix ? "." : "",
             names[i]);
    an_method(l, target, names[i], name);
  }
}
#define AN_METHODS(l, o, p, ...)                                               \
  do {                                                                         \
    const char *names[] = {__VA_ARGS__};                                       \
    an_methods(l, o, p, names, sizeof(names) / sizeof(*names));                \
  } while (0)
bool dg_avm1_library_bind(dg_avm1_library_t *l, dg_avm1_t *v) {
  if (!l || l->vm || !v)
    return false;
  l->vm = v;
  lv_runtime_t *r = an_values(l);
  l->span = (lv_root_span_t){r->external_roots, l->roots, AN_ROOTS};
  r->external_roots = &l->span;
  l->roots[AN_OBJECT] = dg_avm1_object(v, 0);
  for (unsigned i = AN_ARRAY; i <= AN_CLASSES; i++)
    l->roots[i] = dg_avm1_object(v, l->roots[AN_OBJECT]);
  an_hide(l, l->roots[AN_ARRAY], AN_KIND, lv_num(AN_ARRAY_KIND));
  dg_avm1_own_set(v, l->roots[AN_ARRAY], "length", lv_num(0));
  an_hide(l, l->roots[AN_STRING], AN_KIND, lv_num(AN_STRING_KIND));
  an_hide(l, l->roots[AN_NUMBER], AN_KIND, lv_num(AN_NUMBER_KIND));
  an_hide(l, l->roots[AN_BOOLEAN], AN_KIND, lv_num(AN_BOOLEAN_KIND));
  an_hide(l, l->roots[AN_STRING], AN_PRIMITIVE, an_text(l, ""));
  an_hide(l, l->roots[AN_NUMBER], AN_PRIMITIVE, lv_num(0));
  an_hide(l, l->roots[AN_BOOLEAN], AN_PRIMITIVE, dg_avm1_boolean(false));
  static const char *constructors[] = {"Object",    "Array",      "String",
                                       "Number",    "Boolean",    "MovieClip",
                                       "TextField", "TextFormat", "Color"};
  lv_t global = dg_avm1_globals(v);
  for (unsigned i = 0; i < sizeof(constructors) / sizeof(*constructors); i++) {
    lv_t constructor = dg_avm1_native(v, constructors[i]);
    dg_avm1_own_set(v, constructor, "prototype", l->roots[i]);
    dg_avm1_own_set(v, l->roots[i], "constructor", constructor);
    dg_avm1_own_set(v, global, constructors[i], constructor);
  }
  AN_METHODS(l, l->roots[AN_OBJECT], "Object", "toString", "valueOf",
             "hasOwnProperty", "isPropertyEnumerable", "isPrototypeOf", "watch",
             "unwatch");
  an_method(l, dg_avm1_get(v, global, "Object"), "registerClass",
            "Object.registerClass");
  AN_METHODS(l, l->roots[AN_ARRAY], "Array", "push", "pop", "shift", "unshift",
             "slice", "splice", "join", "concat", "reverse", "toString");
  AN_METHODS(l, l->roots[AN_STRING], "String", "toString", "valueOf",
             "substring", "subString", "substr", "slice", "charAt",
             "charCodeAt", "indexOf", "lastIndexOf", "toLowerCase",
             "toUpperCase");
  AN_METHODS(l, l->roots[AN_NUMBER], "Number", "toString", "valueOf");
  AN_METHODS(l, l->roots[AN_BOOLEAN], "Boolean", "toString", "valueOf");
  AN_METHODS(l, l->roots[AN_MOVIECLIP], "MovieClip", "play", "stop",
             "gotoAndPlay", "gotoAndStop", "nextFrame", "prevFrame",
             "attachMovie", "createEmptyMovieClip", "createTextField",
             "duplicateMovieClip", "removeMovieClip", "getNextHighestDepth",
             "getDepth", "getInstanceAtDepth", "swapDepths", "getBounds",
             "hitTest", "localToGlobal", "globalToLocal", "startDrag",
             "stopDrag", "lineStyle", "moveTo", "lineTo", "beginFill",
             "endFill", "clear");
  AN_METHODS(l, l->roots[AN_TEXTFIELD], "TextField", "setTextFormat",
             "getTextFormat", "setNewTextFormat", "getNewTextFormat",
             "removeTextField", "replaceSel", "replaceText", "getDepth");
  AN_METHODS(l, l->roots[AN_COLOR], "Color", "setRGB", "getRGB", "setTransform",
             "getTransform");
  AN_METHODS(l, l->roots[AN_MATH], "Math", "abs", "floor", "ceil", "round",
             "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
             "pow", "exp", "log", "min", "max", "random");
  const char *constants[] = {"E",      "LN2", "LN10",    "LOG2E",
                             "LOG10E", "PI",  "SQRT1_2", "SQRT2"};
  double vals[] = {2.718281828459045,  0.6931471805599453, 2.302585092994046,
                   1.4426950408889634, 0.4342944819032518, 3.141592653589793,
                   0.7071067811865476, 1.4142135623730951};
  for (unsigned i = 0; i < 8; i++)
    dg_avm1_own_set(v, l->roots[AN_MATH], constants[i], lv_num(vals[i]));
  const char *singletons[] = {"Math", "Key", "Mouse", "Selection"};
  for (unsigned i = 0; i < 4; i++)
    dg_avm1_own_set(v, global, singletons[i], l->roots[AN_MATH + i]);
  for (unsigned i = AN_KEY; i <= AN_SELECTION; i++) {
    an_method(l, l->roots[i], "addListener", "addListener");
    an_method(l, l->roots[i], "removeListener", "removeListener");
  }
  AN_METHODS(l, l->roots[AN_KEY], "Key", "getCode", "getAscii", "isDown");
  const char *key_names[] = {"BACKSPACE", "TAB",    "ENTER", "SHIFT", "CONTROL",
                             "CAPSLOCK",  "ESCAPE", "SPACE", "PGUP",  "PGDN",
                             "END",       "HOME",   "LEFT",  "UP",    "RIGHT",
                             "DOWN",      "INSERT", "DELETE"};
  const unsigned key_codes[] = {8,  9,  13, 16, 17, 20, 27, 32, 33,
                                34, 35, 36, 37, 38, 39, 40, 45, 46};
  for (unsigned i = 0; i < sizeof(key_codes) / sizeof(*key_codes); i++)
    dg_avm1_own_set(v, l->roots[AN_KEY], key_names[i], lv_num(key_codes[i]));
  AN_METHODS(l, l->roots[AN_SELECTION], "Selection", "getFocus", "setFocus",
             "setSelection", "getBeginIndex", "getEndIndex", "getCaretIndex");
  AN_METHODS(l, global, "", "setInterval", "clearInterval", "getTimer",
             "parseInt", "parseFloat", "isNaN", "isFinite", "updateAfterEvent",
             "trace", "getURL", "ASSetPropFlags");
  dg_avm1_own_set(v, global, "NaN", lv_num(NAN));
  dg_avm1_own_set(v, global, "Infinity", lv_num(INFINITY));
  dg_avm1_own_set(v, global, "undefined", 0);
  for (unsigned i = AN_OBJECT; i <= AN_SELECTION; i++)
    dg_avm1_flags(v, l->roots[i], NULL, 1, 0);
  for (unsigned i = 0; i < sizeof(constructors) / sizeof(*constructors); i++)
    dg_avm1_flags(v, dg_avm1_get(v, global, constructors[i]), NULL, 1, 0);
  dg_avm1_flags(v, global, NULL, 1, 0);
  return !r->failed;
}
#undef AN_METHODS
lv_t dg_avm1_library_prototype(dg_avm1_library_t *l, const char *name) {
  const char *names[] = {"Object",    "Array",      "String",
                         "Number",    "Boolean",    "MovieClip",
                         "TextField", "TextFormat", "Color"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
    if (!strcmp(name, names[i]))
      return l->roots[i];
  return 0;
}
lv_t dg_avm1_library_class(dg_avm1_library_t *l, const char *name) {
  return dg_avm1_get(l->vm, l->roots[AN_CLASSES], name);
}
void dg_avm1_library_service(dg_avm1_library_t *l, uint64_t clock) {
  if (!l || !l->vm)
    return;
  if (clock < l->clock) {
    lv_fail(an_values(l), "AVM1 service clock moved backwards");
    return;
  }
  l->clock = clock;
  for (unsigned i = 0; i < AN_TIMERS && !an_values(l)->failed; i++) {
    an_timer_t *timer = &l->timers[i];
    if (!timer->id || timer->busy || timer->due > clock)
      continue;
    uint32_t id = timer->id;
    lv_t *t = &l->roots[AN_TIMER_ROOT + i * 4];
    lv_t fn = dg_avm1_truth(l->vm, t[3])
                  ? dg_avm1_get(l->vm, t[0], lv_cstr(an_values(l), t[1]))
                  : t[1];
    lv_t args[64], out = 0;
    unsigned n = lv_count(an_values(l), t[2]);
    if (n > 64) {
      lv_fail(an_values(l), "AVM1 timer argument limit exceeded");
      return;
    }
    for (unsigned j = 0; j < n; j++)
      args[j] = lv_at(an_values(l), t[2], j + 1);
    timer->busy = true;
    timer->due = clock + timer->delay;
    if (dg_avm1_callable(l->vm, fn))
      dg_avm1_invoke(l->vm, fn, t[0], n, args, AN_BUDGET, &out);
    if (timer->id == id)
      timer->busy = false;
  }
}
void dg_avm1_library_key(dg_avm1_library_t *l, unsigned code, unsigned ascii,
                         bool down) {
  if (!l || !l->vm)
    return;
  l->key = code;
  l->ascii = ascii;
  if (code < 256)
    l->keys[code] = down;
  an_broadcast(l, l->roots[AN_KEY], down ? "onKeyDown" : "onKeyUp", 0, NULL);
}
void dg_avm1_library_pointer(dg_avm1_library_t *l, int x, int y,
                             unsigned buttons) {
  if (!l || !l->vm)
    return;
  bool moved = x != l->mouse_x || y != l->mouse_y;
  unsigned changed = l->buttons ^ buttons;
  l->mouse_x = x;
  l->mouse_y = y;
  l->buttons = buttons;
  if (moved)
    an_broadcast(l, l->roots[AN_MOUSE], "onMouseMove", 0, NULL);
  if (changed & 1)
    an_broadcast(l, l->roots[AN_MOUSE],
                 buttons & 1 ? "onMouseDown" : "onMouseUp", 0, NULL);
}
void dg_avm1_library_dispose(dg_avm1_library_t *l) {
  if (!l)
    return;
  if (l->vm) {
    lv_root_span_t **p = &an_values(l)->external_roots;
    while (*p && *p != &l->span)
      p = &(*p)->next;
    if (*p)
      *p = l->span.next;
  }
  free(l);
}
