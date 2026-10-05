#include "lingo_runtime.h"
#include "lingo_bytecode.h"
#include "lingo_names.inc"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forced inline, not merely static: at -Os a helper this small is still
// emitted as a real call, and it is the most-called function in the runtime —
// every name comparison and every prefilter byte goes through it.
__attribute__((always_inline)) static inline unsigned char
fold_char(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? c + 32u : c;
}
static bool equal_text(const char *a, const char *b) {
  for (;;) {
    unsigned char x=(unsigned char)*a++, y=(unsigned char)*b++;
    if (x != y) {
      // Identifiers and property names overwhelmingly contain ASCII. Keep
      // libc's high-byte behavior without calling it for every mismatched key.
      unsigned folded_x=x<128 ? (x>='A'&&x<='Z' ? x+32u : x) : (unsigned)tolower(x);
      unsigned folded_y=y<128 ? (y>='A'&&y<='Z' ? y+32u : y) : (unsigned)tolower(y);
      if (folded_x != folded_y) return false;
    }
    if (!x || !y) return x==y;
  }
}
void lv_fail(lv_runtime_t *r, const char *message) {
  if (r->failed)
    return;
  r->failed = true;
  if (r->depth) {
    lv_frame_t *f = &r->frames[r->depth - 1];
    snprintf(r->error, sizeof(r->error), "%s:%u:%s:%u: %s", f->movie->name,
             f->handler->member, f->handler->name, f->line, message);
  } else
    snprintf(r->error, sizeof(r->error), "%s", message);
}
void lv_script_fail(lv_runtime_t *r, const char *message) {
  if (r->failed)
    return;
  lv_fail(r, message);
  r->script_error = true;
}
bool lv_recover_script(lv_runtime_t *r) {
  if (!r->failed || !r->script_error)
    return false;
  // The original projector shows a script alert and discards every call
  // context; the next top-level event remains runnable, like abort().
  snprintf(r->last_script_error, sizeof(r->last_script_error), "%s", r->error);
  r->script_error_count++;
  r->failed = r->script_error = r->aborted = r->yielded = false;
  r->error[0] = 0;
  r->depth = 0;
  r->atomic_depth = 0;
  r->arithmetic_depth = 0;
  r->result = (lv_t){0};
  return true;
}
unsigned lb_name_id(const char *name) {
  // The table is lower-case ASCII in strcmp order; fold the query the way
  // Lingo compares names and binary-search it. Generated handlers bind
  // their names' ids, so this runs a few thousand times per hour of play.
  char folded[64];
  unsigned n = 0;
  for (; name[n] && n < sizeof(folded) - 1; n++) {
    unsigned char c = (unsigned char)name[n];
    folded[n] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  if (name[n]) return LB_NAME_COUNT; // longer than any name in the table
  folded[n] = 0;
  unsigned low = 0, high = LB_NAME_COUNT;
  while (low < high) {
    unsigned mid = (low + high) / 2;
    int order = strcmp(folded, lb_name_text[mid]);
    if (!order) return mid;
    if (order < 0)
      high = mid;
    else
      low = mid + 1;
  }
  return LB_NAME_COUNT;
}
static void index_globals(lv_runtime_t *);
void lv_init(lv_runtime_t *r, lv_services_t services, void *context,
             const char *const *names, unsigned count, uint32_t seed) {
  memset(r, 0, sizeof(*r));
  r->services = services;
  r->context = context;
  r->global_names = names;
  r->global_count = count;
  index_globals(r);
  r->random_state = seed ? seed : 1;
  r->item_delimiter = ',';
#if LV_ALLOCATION_CHAIN
  r->allocation_first = r->allocation_last = LV_OBJECTS;
#endif
  if (count > LV_GLOBALS)
    lv_fail(r, "too many globals");
}
lv_t lv_num(double number) {
  // Integral values in the 32-bit range are integers, as Director's are;
  // anything else, including an integral value beyond that range, is a
  // float. Negative zero is the integer zero, as it is in Director.
  if (number >= -2147483648.0 && number <= 2147483647.0) {
    int32_t i = (int32_t)number;
    if ((double)i == number) return lv_make(LV_NUMBER, i);
  }
  return lv_float(number);
}
static bool is_object(lv_t value) {
  unsigned type = lv_type(value);
  return (type >= LV_STRING && type <= LV_PROPLIST && type != LV_SYMBOL) ||
         type == LV_INSTANCE;
}
static lv_object_t *object(lv_runtime_t *r, lv_t value) {
  if (!is_object(value)) {
    // Scripts index void or scalars; the original raises an alert and plays on.
    lv_script_fail(r, "operation on a non-object value");
    return NULL;
  }
  if (lv_id(value) < 0 || lv_id(value) >= LV_OBJECTS ||
      r->objects[lv_id(value)].type != lv_type(value)) {
    lv_fail(r, "invalid heap reference");
    return NULL;
  }
  return &r->objects[lv_id(value)];
}
static unsigned bytes_per(lv_object_t *obj) {
  return obj->type >= LV_LIST ? sizeof(lv_t) : 1;
}
static uint32_t aligned(uint32_t n) { return (n + 7u) & ~7u; }
#if LV_ALLOCATION_CHAIN
static void allocation_remove(lv_runtime_t *r, unsigned id) {
  lv_object_t *o = &r->objects[id];
  if (o->previous < LV_OBJECTS) r->objects[o->previous].next = o->next;
  else r->allocation_first = o->next;
  if (o->next < LV_OBJECTS) r->objects[o->next].previous = o->previous;
  else r->allocation_last = o->previous;
}
static void allocation_append(lv_runtime_t *r, unsigned id) {
  lv_object_t *o = &r->objects[id];
  o->previous = r->allocation_last;
  o->next = LV_OBJECTS;
  if (r->allocation_last < LV_OBJECTS) r->objects[r->allocation_last].next = id;
  else r->allocation_first = id;
  r->allocation_last = id;
}
#endif
#if DG_CAP_EMERGENCY_COLLECT
// Insert into the allocation chain in heap-offset order, keeping the
// linear-compaction invariant when an object lands in a freed hole. Only the
// emergency collector frees one, so only Director 7 and later need this.
static void allocation_insert(lv_runtime_t *r, unsigned id) {
  lv_object_t *o = &r->objects[id];
  unsigned after = LV_OBJECTS;
  for (unsigned i = r->allocation_first; i < LV_OBJECTS;
       i = r->objects[i].next) {
    if (r->objects[i].offset > o->offset) break;
    after = i;
  }
  if (after == LV_OBJECTS) {
    o->previous = LV_OBJECTS;
    o->next = r->allocation_first;
    if (r->allocation_first < LV_OBJECTS)
      r->objects[r->allocation_first].previous = id;
    else
      r->allocation_last = id;
    r->allocation_first = id;
  } else {
    o->previous = (uint16_t)after;
    o->next = r->objects[after].next;
    r->objects[after].next = (uint16_t)id;
    if (o->next < LV_OBJECTS) r->objects[o->next].previous = (uint16_t)id;
    else r->allocation_last = (uint16_t)id;
  }
}
static bool emergency_collect(lv_runtime_t *r);
static uint32_t emergency_room(lv_runtime_t *r, uint32_t bytes);
#endif
static lv_t allocate(lv_runtime_t *r, unsigned type, unsigned count) {
  unsigned unit = type >= LV_LIST ? sizeof(lv_t) : 1;
  if (count > LV_HEAP_BYTES / unit) {
    lv_fail(r, "native value heap exhausted");
    return (lv_t){0};
  }
  uint32_t bytes = aligned(count * unit);
  uint32_t offset = r->heap_used;
  bool tail = bytes <= LV_HEAP_BYTES - r->heap_used;
  if (!tail) {
#if DG_CAP_EMERGENCY_COLLECT
    // Between-steps collection could not cover this step's need: free old
    // garbage in place (nothing moves) and retry the bump path, then
    // first-fit into a freed hole.
    emergency_collect(r);
    offset = r->heap_used;
    tail = bytes <= LV_HEAP_BYTES - r->heap_used;
    if (!tail) {
      offset = emergency_room(r, bytes);
      if (offset == UINT32_MAX) {
        lv_fail(r, "native value heap exhausted");
        return (lv_t){0};
      }
    }
#else
    lv_fail(r, "native value heap exhausted");
    return (lv_t){0};
#endif
  }
  // Collection is the only operation that frees handles. Between collections
  // the lowest free handle can only move forward, preserving allocation order
  // without rescanning the occupied prefix for every temporary string.
  unsigned handle = LV_OBJECTS;
  for (unsigned i = r->object_hint; i < LV_OBJECTS; i++)
    if (!r->objects[i].type) { handle = i; break; }
#if DG_CAP_EMERGENCY_COLLECT
  if (handle == LV_OBJECTS && emergency_collect(r))
    for (unsigned i = r->object_hint; i < LV_OBJECTS; i++)
      if (!r->objects[i].type) { handle = i; break; }
#endif
  if (handle == LV_OBJECTS) {
    lv_fail(r, "native object handles exhausted");
    return (lv_t){0};
  }
  if (tail)
    offset = r->heap_used; // an emergency pass may have lowered the bump top
  r->objects[handle] = (lv_object_t){.type = type,
                                     .offset = offset,
                                     .count = count,
                                     .capacity = count};
  r->objects[handle].birth = ++r->allocation_serial;
#if DG_CAP_EMERGENCY_COLLECT
  if (tail) allocation_append(r, handle);
  else allocation_insert(r, handle);
#elif LV_ALLOCATION_CHAIN
  // Below Director 7 a request that does not fit the bump top fails outright,
  // so every object that exists was appended at the top.
  allocation_append(r, handle);
#endif
  r->object_count++;
  r->object_hint = handle + 1;
  if (r->object_limit <= handle) r->object_limit = handle + 1;
  if (offset + bytes > r->heap_used) {
    // The bump top covers every object, including one placed past a
    // recomputed top after an emergency pass.
    r->heap_used = offset + bytes;
    if (r->heap_used > r->heap_high_water)
      r->heap_high_water = r->heap_used;
  }
  r->allocations_since_gc++;
  return lv_make(type, (int32_t)handle);
}
// The empty table a hand-written program links against; symbols.c, which
// the converter writes per game, defines the real one.
#ifdef DIRECTOR64_PACKAGE
// A package-loaded runtime reads the corpus symbols its game package brought
// (runtime/package/package.c); the generated table is not linked.
static lb_symbol_table_t package_symbols;
void lv_use_symbols(lb_symbol_table_t table) { package_symbols = table; }
#define lb_symbols package_symbols
#else
__attribute__((weak)) const lb_symbol_table_t lb_symbols = {NULL, 0, NULL, 0};
#endif
lv_t lv_symbol(lv_runtime_t *r, const char *text) {
  if (r->symbol_for == text && r->symbol != LV_NO_SYMBOL)
    return lv_make(LV_SYMBOL, (int32_t)r->symbol);
  uint32_t hash = lv_text_hash(text);
  if (lb_symbols.bucket_count) {
    unsigned bucket = hash & (lb_symbols.bucket_count - 1);
    unsigned end = lb_symbols.buckets[bucket + 1];
    for (unsigned id = lb_symbols.buckets[bucket]; id < end; id++)
      if (equal_text(lb_symbols.text[id], text))
        return lv_make(LV_SYMBOL, (int32_t)id);
  }
  // Interned behind the converter's ids. Symbols are never collected: the
  // table is bounded, and a program that spells more than it holds fails
  // rather than aliasing two of them.
  for (unsigned probe = hash & (LV_DYNAMIC_SYMBOL_SLOTS - 1);;
       probe = (probe + 1) & (LV_DYNAMIC_SYMBOL_SLOTS - 1)) {
    unsigned slot = r->dynamic_symbol_slot[probe];
    if (!slot) {
      size_t n = strlen(text) + 1;
      if (r->dynamic_symbol_count >= LV_DYNAMIC_SYMBOLS ||
          n > LV_SYMBOL_BYTES - r->symbol_text_used) {
        lv_fail(r, "symbol table exhausted");
        return (lv_t){0};
      }
      unsigned index = r->dynamic_symbol_count++;
      r->dynamic_symbol_offset[index] = (uint16_t)r->symbol_text_used;
      memcpy(r->symbol_text + r->symbol_text_used, text, n);
      r->symbol_text_used += (unsigned)n;
      r->dynamic_symbol_slot[probe] = (uint16_t)(index + 1);
      return lv_make(LV_SYMBOL, (int32_t)(lb_symbols.count + index));
    }
    if (equal_text(r->symbol_text + r->dynamic_symbol_offset[slot - 1], text))
      return lv_make(LV_SYMBOL, (int32_t)(lb_symbols.count + slot - 1));
  }
}
const char *lv_symbol_text(const lv_runtime_t *r, lv_t value) {
  unsigned id = (unsigned)lv_id(value);
  if (id < lb_symbols.count) return lb_symbols.text[id];
  id -= lb_symbols.count;
  return id < r->dynamic_symbol_count ? r->symbol_text + r->dynamic_symbol_offset[id] : "";
}
unsigned lv_symbol_id(lv_runtime_t *r, const char *name) {
  if (r->symbol_for == name && r->symbol != LV_NO_SYMBOL) return r->symbol;
  lv_t value = lv_symbol(r, name);
  return lv_type(value) == LV_SYMBOL ? (unsigned)lv_id(value) : LV_NO_SYMBOL;
}
lv_t lv_text(lv_runtime_t *r, const char *text, bool symbol) {
  if (symbol) return lv_symbol(r, text);
  size_t length = strlen(text);
  if (length >= LV_HEAP_BYTES) {
    lv_fail(r, "string exceeds heap");
    return (lv_t){0};
  }
  lv_t value = allocate(r, LV_STRING, (unsigned)length + 1);
  if (!r->failed)
    memcpy(r->heap + r->objects[lv_id(value)].offset, text, length + 1);
  return value;
}
lv_t lv_bytes(lv_runtime_t *r, const void *data, unsigned size) {
  if (size >= LV_HEAP_BYTES) {lv_fail(r, "string exceeds heap");return (lv_t){0};}
  lv_t value = allocate(r, LV_STRING, size + 1);
  if (!r->failed) {
    char *p = (char *)r->heap + r->objects[lv_id(value)].offset;
    memcpy(p, data, size); p[size] = 0;
  }
  return value;
}
static unsigned string_length(lv_runtime_t *r, lv_t value) {
  if (lv_type(value) == LV_VOID) return 0;
  if (lv_type(value) == LV_SYMBOL) return (unsigned)strlen(lv_symbol_text(r, value));
  lv_object_t *o = object(r, value);
  return o && o->count ? o->count - 1 : 0;
}
const char *lv_cstr(lv_runtime_t *r, lv_t value) {
  if (lv_type(value) == LV_VOID)
    return "";
  if (lv_type(value) == LV_SYMBOL)
    return lv_symbol_text(r, value);
  if (lv_type(value) != LV_STRING) {
    lv_script_fail(r, "expected string/symbol");
    return "";
  }
  lv_object_t *obj = object(r, value);
  return obj ? (const char *)r->heap + obj->offset : "";
}
static bool parse_number(const char *s, double *out) {
  char *end;
  errno = 0;
  double parsed = strtod(s, &end);
  if (end == s || errno == ERANGE)
    return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    end++;
  if (*end)
    return false;
  *out = parsed;
  return true;
}
double lv_number(lv_runtime_t *r, lv_t value) {
  if (lv_type(value) == LV_NUMBER)
    return lv_numeric(value);
  if (lv_type(value) == LV_VOID)
    return 0;
  if (lv_type(value) == LV_MEMBER || lv_type(value) == LV_FIELD)
    return (uint32_t)lv_id(value) & 65535u;
  if (lv_type(value) >= LV_SPRITE)
    return lv_id(value);
  if (lv_type(value) == LV_STRING) {
    // Lingo coerces numeric text in arithmetic: "3" + 1 is 4.
    double parsed;
    if (parse_number(lv_cstr(r, value), &parsed))
      return parsed;
  }
  if (lv_type(value) == LV_SYMBOL) {
    const char *s = lv_cstr(r, value);
    if (equal_text(s, "true"))
      return 1;
    if (equal_text(s, "false"))
      return 0;
  }
  lv_script_fail(r, "non-numeric value in arithmetic");
  return 0;
}
static bool integral_operand(lv_runtime_t *r, lv_t v) {
  if (lv_type(v) == LV_STRING) {
    double parsed;
    const char *s = lv_cstr(r, v);
    return parse_number(s, &parsed) && parsed == trunc(parsed) &&
           !strchr(s, '.') && !strchr(s, 'e') && !strchr(s, 'E');
  }
  // Numbers carry a float flag; other references keep their legacy marking.
  if (lv_type(v) == LV_NUMBER) return !lv_is_float(v);
  return !lv_id(v);
}
int32_t lv_integer(lv_runtime_t *r, lv_t value) {
  double n = lv_number(r, value);
  if (!isfinite(n) || n < INT32_MIN || n > INT32_MAX) {
    lv_script_fail(r, "integer out of range");
    return 0;
  }
  return (int32_t)n;
}
bool lv_truth(lv_runtime_t *r, lv_t value) {
  if (lv_type(value) == LV_VOID)
    return false;
  if (lv_type(value) == LV_NUMBER)
    return lv_numeric(value) != 0;
  if (lv_type(value) == LV_SYMBOL)
    return !equal_text(lv_cstr(r, value), "false");
  if (lv_type(value) == LV_STRING)
    return *lv_cstr(r, value) != 0;
  return true;
}
#if DG_D10
lv_t lv_byte_list(lv_runtime_t *r, const unsigned char *data, unsigned size) {
  // Terminated like a string so text conversions stay in bounds; the count
  // excludes the terminator, which is the convention for byte storage.
  lv_t value = allocate(r, LV_BYTES, size + 1);
  if (r->failed)
    return (lv_t){0};
  uint8_t *bytes = r->heap + r->objects[lv_id(value)].offset;
  memcpy(bytes, data, size);
  bytes[size] = 0;
  return value;
}
#endif
lv_t lv_list(lv_runtime_t *r, unsigned count, const lv_t *items, bool props) {
  lv_t value = allocate(r, props ? LV_PROPLIST : LV_LIST, count);
  if (!r->failed && count)
    memcpy(r->heap + r->objects[lv_id(value)].offset, items, count * sizeof(lv_t));
  return value;
}
lv_t lv_point(lv_runtime_t *r, double x, double y) {
  lv_t values[] = {lv_num(x), lv_num(y)};
  lv_t result = lv_list(r, 2, values, false);
  if (!r->failed) r->objects[lv_id(result)].geometry = 2;
  return result;
}
lv_t lv_rect(lv_runtime_t *r, double left, double top, double right, double bottom) {
  lv_t values[] = {lv_num(left), lv_num(top), lv_num(right), lv_num(bottom)};
  lv_t result = lv_list(r, 4, values, false);
  if (!r->failed) r->objects[lv_id(result)].geometry = 4;
  return result;
}
unsigned lv_count(lv_runtime_t *r, lv_t value) {
#if DG_D10
  if (lv_type(value) == LV_GLOBALS_VIEW) return r->global_count;
#endif
  if (lv_type(value) == LV_SYMBOL) return string_length(r, value);
  lv_object_t *obj = object(r, value);
  if (!obj)
    return 0;
  return obj->type == LV_PROPLIST ? obj->count / 2
         : obj->type >= LV_LIST   ? obj->count
                                  : obj->count - 1;
}
lv_t lv_at(lv_runtime_t *r, lv_t value, unsigned index) {
  lv_object_t *obj = object(r, value);
  if (obj && obj->type == LV_BYTES) {
    if (!index || index >= obj->count) {
      lv_script_fail(r, "list index out of range");
      return (lv_t){0};
    }
    return lv_num(r->heap[obj->offset + index - 1]);
  }
  if (!obj || obj->type < LV_LIST || !index || index > obj->count) {
    lv_script_fail(r, "list index out of range");
    return (lv_t){0};
  }
  return ((lv_t *)(r->heap + obj->offset))[index - 1];
}
static bool resize(lv_runtime_t *r, lv_object_t *obj, unsigned size) {
  if (size <= obj->capacity) {
    if (size > obj->count)
      memset(r->heap + obj->offset + obj->count * bytes_per(obj), 0,
              (size - obj->count) * bytes_per(obj));
    obj->count = size;
    return true;
  }
  unsigned unit = bytes_per(obj), capacity = size < 16 ? 16 : size * 2;
  if (capacity > LV_HEAP_BYTES / unit ||
      aligned(capacity * unit) > LV_HEAP_BYTES - r->heap_used) {
    // Doubling is an optimisation, not a requirement. Near the ceiling a
    // large list still grows, element by element and compacted on the next
    // collection, instead of failing while half the heap is garbage.
    capacity = size;
    if (capacity > LV_HEAP_BYTES / unit) {
      lv_fail(r, "list growth exceeds heap");
      return false;
    }
    if (aligned(capacity * unit) > LV_HEAP_BYTES - r->heap_used) {
#if DG_CAP_EMERGENCY_COLLECT
      // Free old garbage in place and retry; a first-fit hole serves when
      // the tail still cannot. The resized object itself always survives
      // the pass, whether or not a root can see it.
      obj->birth = r->step_serial;
      emergency_collect(r);
      if (aligned(capacity * unit) > LV_HEAP_BYTES - r->heap_used) {
        uint32_t hole = emergency_room(r, aligned(capacity * unit));
        if (hole == UINT32_MAX) {
          lv_fail(r, "list growth exceeds heap");
          return false;
        }
        memcpy(r->heap + hole, r->heap + obj->offset, obj->count * unit);
        memset(r->heap + hole + obj->count * unit, 0,
               (capacity - obj->count) * unit);
        unsigned id = (unsigned)(obj - r->objects);
        allocation_remove(r, id);
        obj->offset = hole;
        obj->count = size;
        obj->capacity = capacity;
        allocation_insert(r, id);
        if (hole + aligned(capacity * unit) > r->heap_used)
          r->heap_used = hole + aligned(capacity * unit);
        return true;
      }
#else
      lv_fail(r, "list growth exceeds heap");
      return false;
#endif
    }
  }
  memcpy(r->heap + r->heap_used, r->heap + obj->offset, obj->count * unit);
  memset(r->heap + r->heap_used + obj->count * unit, 0,
         (capacity - obj->count) * unit);
  obj->offset = r->heap_used;
#if LV_ALLOCATION_CHAIN
  {
    unsigned id = (unsigned)(obj - r->objects);
    allocation_remove(r, id);
    allocation_append(r, id);
  }
#endif
  obj->count = size;
  obj->capacity = capacity;
  r->heap_used += aligned(capacity * unit);
  if (r->heap_used > r->heap_high_water)
    r->heap_high_water = r->heap_used;
  return true;
}
bool lv_set_at(lv_runtime_t *r, lv_t value, unsigned index, lv_t item) {
  lv_object_t *obj = object(r, value);
  if (!obj || obj->type < LV_LIST || !index ||
      index > LV_HEAP_BYTES / sizeof(lv_t)) {
    lv_fail(r, "invalid list assignment");
    return false;
  }
  if (index > obj->count && !resize(r, obj, index))
    return false;
  ((lv_t *)(r->heap + obj->offset))[index - 1] = item;
  return true;
}
static void mark(lv_runtime_t *r, lv_t value, unsigned depth) {
  if (!is_object(value))
    return;
  lv_object_t *obj = object(r, value);
  if (!obj || obj->mark)
    return;
  obj->mark = 1;
  if (depth > 256) {
    lv_fail(r, "object nesting limit");
    return;
  }
  if (obj->type >= LV_LIST) {
    const lv_t *items = (const lv_t *)(r->heap + obj->offset);
    for (unsigned i = 0; i < obj->count; i++)
      mark(r, items[i], depth + 1);
  }
}
static bool referenced(lv_runtime_t *r, lv_t value, lv_type_t type, int32_t id,
                         uint8_t *seen, unsigned depth) {
  if (lv_type(value) == (uint32_t)type && lv_id(value) == id) return true;
  if (!is_object(value)) return false;
  lv_object_t *o = object(r, value);
  if (!o) return true; // Invalid graph: conservatively retain platform resources.
  unsigned byte = (unsigned)lv_id(value) >> 3;
  uint8_t bit = (uint8_t)(1u << ((unsigned)lv_id(value) & 7u));
  if (seen[byte] & bit) return false;
  seen[byte] |= bit;
  if (depth > 256) {
    lv_fail(r, "resource reference nesting limit");
    return true;
  }
  if (o->type < LV_LIST) return false;
  const lv_t *items = (const lv_t *)(r->heap + o->offset);
  for (unsigned i = 0; i < o->count; i++)
    if (referenced(r, items[i], type, id, seen, depth + 1)) return true;
  return false;
}
bool lv_has_reference(lv_runtime_t *r, lv_type_t type, int32_t id) {
  // Trace without moving or allocating: services may call this while a native
  // expression is atomic. Unreachable heap garbage must not retain FileIO slots.
  uint8_t seen[(LV_OBJECTS + 7) / 8] = {0};
#define REFERENCED(value) referenced(r, value, type, id, seen, 0)
  for (unsigned i = 0; i < r->global_count; i++)
    if (REFERENCED(r->globals[i])) return true;
  for (unsigned i = 0; i < LV_ROOTS; i++)
    if (REFERENCED(r->roots[i])) return true;
  for (unsigned i = 0; i < r->script_count; i++)
    if (REFERENCED(r->script_objects[i])) return true;
  if (REFERENCED(r->result) || REFERENCED(r->the_result)) return true;
  for (unsigned i = 0; i < r->depth; i++) {
    lv_frame_t *f = &r->frames[i];
    if (REFERENCED(f->self)) return true;
    for (unsigned j = 0; j < f->handler->locals; j++)
      if (REFERENCED(f->locals[j])) return true;
    for (unsigned j = 0; j < f->temp_count; j++)
      if (REFERENCED(f->temps[j])) return true;
  }
#undef REFERENCED
  return false;
}
#if DG_EXTENDED && !DG_D7_UP
#endif
static void mark_roots(lv_runtime_t *r) {
  for (unsigned i = 0; i < r->global_count; i++)
    mark(r, r->globals[i], 0);
  for (unsigned i = 0; i < LV_ROOTS; i++)
    mark(r, r->roots[i], 0);
  for (unsigned i = 0; i < r->script_count; i++)
    mark(r, r->script_objects[i], 0);
  mark(r, r->result, 0);
  mark(r, r->the_result, 0);
  for (unsigned i = 0; i < r->depth; i++)
    mark(r, r->frames[i].self, 0);
  for (unsigned i = 0; i < r->depth; i++)
    for (unsigned j = 0; j < r->frames[i].handler->locals; j++)
      mark(r, r->frames[i].locals[j], 0);
  for (unsigned i = 0; i < r->depth; i++)
    for (unsigned j = 0; j < r->frames[i].temp_count; j++)
      mark(r, r->frames[i].temps[j], 0);
}
#if DG_CAP_EMERGENCY_COLLECT
// Reclaim old garbage without moving anything, on allocation failure inside
// a step. Ordinary collection runs between interpreter steps because native
// helpers hold raw heap pointers and untabled values; here nothing moves and
// everything born during the current step survives, so those stay valid.
// Only garbage from earlier steps — which nothing can still hold — is freed,
// and the next between-steps collection compacts the holes away.
static bool emergency_collect(lv_runtime_t *r) {
  uint64_t collect_started = r->clock_us ? r->clock_us() : 0;
  r->emergency_passes++;
  for (unsigned i = 0; i < r->object_limit; i++)
    r->objects[i].mark = 0;
  mark_roots(r);
  bool freed = false;
  uint32_t end = 0;
  for (unsigned i = r->allocation_first; i < LV_OBJECTS;) {
    lv_object_t *obj = &r->objects[i];
    unsigned following = obj->next;
    if (!obj->mark &&
        (uint32_t)(obj->birth - r->step_serial) >= 0x80000000u) {
      allocation_remove(r, i);
      obj->type = 0;
      r->object_count--;
      if (r->object_hint > i) r->object_hint = i;
      freed = true;
    } else {
      end = obj->offset + aligned(obj->capacity * bytes_per(obj));
    }
    i = following;
  }
  r->heap_used = end;
  if (r->clock_us)
    r->collect_us += r->clock_us() - collect_started;
  return freed;
}
// First-fit hole between live objects; the tail is the ordinary bump path.
static uint32_t emergency_room(lv_runtime_t *r, uint32_t bytes) {
  uint32_t previous_end = 0;
  for (unsigned i = r->allocation_first; i < LV_OBJECTS;
       i = r->objects[i].next) {
    lv_object_t *obj = &r->objects[i];
    if (obj->offset - previous_end >= bytes)
      return previous_end;
    previous_end = obj->offset + aligned(obj->capacity * bytes_per(obj));
  }
  return UINT32_MAX;
}
#endif
void lv_collect(lv_runtime_t *r) {
  if (r->atomic_depth)
    return;
  uint64_t collect_started = r->clock_us ? r->clock_us() : 0;
  r->collect_passes++;
  for (unsigned i = 0; i < r->object_limit; i++)
    r->objects[i].mark = 0;
  mark_roots(r);
  // Compact in old-offset order, so memmove never overwrites a not-yet-moved
  // object.
  unsigned live_limit = 0, free_hint = r->object_limit;
#if LV_ALLOCATION_CHAIN
  // Allocation and growth maintain physical heap order. Walking that chain
  // makes compaction linear, with no sort or LV_OBJECTS-sized scratch array.
  free_hint = r->object_hint;
  uint32_t next = 0;
  for (unsigned i = r->allocation_first; i < LV_OBJECTS;) {
    lv_object_t *obj = &r->objects[i];
    unsigned following = obj->next;
    if (!obj->mark) {
      allocation_remove(r, i);
      obj->type = 0;
      r->object_count--;
      if (free_hint > i) free_hint = i;
    } else {
      if (live_limit <= i) live_limit = i + 1;
      uint32_t bytes = obj->count * bytes_per(obj);
      memmove(r->heap + next, r->heap + obj->offset, bytes);
      obj->offset = next;
      obj->capacity = obj->count;
      next += aligned(bytes);
    }
    i = following;
  }
#else
  uint16_t order[LV_OBJECTS];
  unsigned count = 0;
  for (unsigned i = 0; i < r->object_limit; i++) {
    lv_object_t *obj = &r->objects[i];
    if (!obj->mark) {
      obj->type = 0;
      if (free_hint > i) free_hint = i;
      continue;
    }
    live_limit = i + 1;
    unsigned n = count++;
    // Handles are allocated in increasing order and compaction rewrites
    // offsets in the order this array already holds, so the scan hands back a
    // list that is close to sorted and insertion costs about one move per
    // object that fell behind.
    while (n && r->objects[order[n - 1]].offset > obj->offset) {
      order[n] = order[n - 1];
      n--;
    }
    order[n] = (uint16_t)i;
  }
  r->object_count = count;
  uint32_t next = 0;
  for (unsigned i = 0; i < count; i++) {
    lv_object_t *obj = &r->objects[order[i]];
    uint32_t bytes = obj->count * bytes_per(obj);
    memmove(r->heap + next, r->heap + obj->offset, bytes);
    obj->offset = next;
    obj->capacity = obj->count;
    next += aligned(bytes);
  }
#endif
  r->heap_used = next;
  r->object_hint = free_hint;
  r->object_limit = live_limit;
  r->allocations_since_gc = 0;
  if (r->clock_us)
    r->collect_us += r->clock_us() - collect_started;
}
static bool same_depth(lv_runtime_t *r, lv_t a, lv_t b, unsigned depth) {
  // Director compares a number with numeric text by value, while two strings
  // still compare as text. Willy's toolbox uses numeric indices and string cases.
  if ((lv_type(a) == LV_NUMBER && lv_type(b) == LV_STRING) ||
      (lv_type(b) == LV_NUMBER && lv_type(a) == LV_STRING)) {
    lv_t number = lv_type(a) == LV_NUMBER ? a : b;
    const char *text = lv_cstr(r, lv_type(a) == LV_STRING ? a : b);
    if (!*text) return lv_numeric(number) == 0;
    char *end;
    double parsed = strtod(text, &end);
    return end != text && !*end && isfinite(parsed) && lv_numeric(number) == parsed;
  }
  if ((lv_type(a) == LV_NUMBER || lv_type(a) == LV_VOID) &&
      (lv_type(b) == LV_NUMBER || lv_type(b) == LV_VOID))
    return lv_number(r, a) == lv_number(r, b);
#if DG_D10
  // Director coerces VOID to the empty string when compared with text:
  // authored guards test `x = EMPTY` on optional parameters.
  if ((lv_type(a) == LV_VOID && lv_type(b) == LV_STRING) ||
      (lv_type(b) == LV_VOID && lv_type(a) == LV_STRING))
    return !*lv_cstr(r, lv_type(a) == LV_STRING ? a : b);
#endif
  if (lv_type(a) != lv_type(b))
    return false;
  if (lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL)
    return equal_text(lv_cstr(r, a), lv_cstr(r, b));
  if (lv_type(a) >= LV_LIST && lv_type(a) <= LV_PROPLIST && lv_id(a) != lv_id(b)) {
    if (depth >= 64) {
      lv_fail(r, "cyclic/deep list comparison");
      return false;
    }
    lv_object_t *ao = object(r, a), *bo = object(r, b);
    if (!ao || !bo || ao->count != bo->count)
      return false;
    for (unsigned i = 1; i <= ao->count; i++)
      if (!same_depth(r, lv_at(r, a, i), lv_at(r, b, i), depth + 1))
        return false;
    return true;
  }
  return lv_id(a) == lv_id(b);
}
static bool same(lv_runtime_t *r, lv_t a, lv_t b) {
  return same_depth(r, a, b, 0);
}
static lv_t duplicate(lv_runtime_t *r, lv_t value, uint16_t *copies,
                       unsigned depth) {
  // Datum::clone deep-copies list containers but retains script object identity.
  // GENMODUL clones its list of sound instances before advancing/removing them.
  if (lv_type(value) != LV_LIST && lv_type(value) != LV_PROPLIST) return value;
  lv_object_t *o = object(r, value);
  if (!o || depth > 128) {
    lv_fail(r, "duplicate nesting limit");
    return (lv_t){0};
  }
  if (copies[lv_id(value)])
    return lv_make(lv_type(value), (int32_t)copies[lv_id(value)] - 1);
  lv_t copy = allocate(r, lv_type(value), o->count);
  if (r->failed) return (lv_t){0};
  copies[lv_id(value)] = (uint16_t)(lv_id(copy) + 1);
  r->objects[lv_id(copy)].geometry = o->geometry;
  r->objects[lv_id(copy)].sorted = o->sorted;
  r->objects[lv_id(copy)].script = o->script;
  for (unsigned i = 1; i <= o->count && !r->failed; i++)
    lv_set_at(r, copy, i, duplicate(r, lv_at(r, value, i), copies, depth + 1));
  return copy;
}
static bool format_value(lv_runtime_t *r, lv_t v, char *out, size_t capacity,
                         unsigned depth) {
  if (!capacity || depth > 64)
    return false;
  if (r->services.format &&
      (lv_type(v) == LV_MEMBER || lv_type(v) == LV_FIELD || lv_type(v) == LV_CASTLIB))
    return r->services.format(r, v, out, capacity);
  if (lv_type(v) == LV_STRING || lv_type(v) == LV_SYMBOL) {
    const char *s = lv_cstr(r, v);
    size_t n = strlen(s);
    if (n >= capacity)
      return false;
    memcpy(out, s, n + 1);
    return true;
  }
  if (lv_type(v) == LV_LIST || lv_type(v) == LV_PROPLIST || lv_type(v) == LV_BYTES) {
    if (capacity < 3)
      return false;
    *out++ = '[';
    capacity--;
    lv_object_t *obj = object(r, v);
    if (!obj)
      return false;
    // Byte storage keeps a terminator the list protocol never shows.
    unsigned count = lv_type(v) == LV_BYTES ? lv_count(r, v) : obj->count;
    if (!count && lv_type(v) == LV_PROPLIST) {
      *out++ = ':';
      capacity--;
    }
    for (unsigned i = 1; i <= count; i++) {
      if (i > 1) {
        if (capacity < 3)
          return false;
        *out++ = lv_type(v) == LV_PROPLIST && !(i & 1) ? ':' : ',';
        capacity--;
      }
      lv_t item = lv_at(r, v, i);
      bool quoted = lv_type(item) == LV_STRING;
      if (quoted || lv_type(item) == LV_SYMBOL) {
        if (capacity < 3)
          return false;
        *out++ = quoted ? '"' : '#';
        capacity--;
      }
      if (!format_value(r, item, out, capacity, depth + 1))
        return false;
      size_t length = strlen(out);
      out += length;
      capacity -= length;
      if (quoted) {
        if (capacity < 3)
          return false;
        *out++ = '"';
        capacity--;
      }
    }
    if (capacity < 2)
      return false;
    *out++ = ']';
    *out = 0;
    return true;
  }
  if (lv_type(v) == LV_INSTANCE) {
    // Named by allocation order, which the program decides, rather than by
    // handle, which the collector decides: two builds that behave the same
    // print the same transcript whatever their heap layout.
    lv_object_t *obj = object(r, v);
    int n = snprintf(out, capacity, "<instance %u>", (unsigned)(obj ? obj->birth : 0));
    return n >= 0 && (size_t)n < capacity;
  }
  int n = snprintf(out, capacity, "%.15g", lv_number(r, v));
  return n >= 0 && (size_t)n < capacity;
}
bool lv_format(lv_runtime_t *r, lv_t value, char *out, size_t capacity) {
  return format_value(r, value, out, capacity, 0);
}
static lv_t string_value(lv_runtime_t *r, lv_t value) {
  if (!lv_format(r, value, r->text_scratch, sizeof(r->text_scratch))) {
    lv_fail(r, "serialized value exceeds budget");
    return (lv_t){0};
  }
  return lv_text(r, r->text_scratch, false);
}
// Operators arrive as ids resolved at build time (lingo_bytecode.h), so the
// tests below are integer compares; the order is the order the string chain
// tested them in, which matters where an operand type could satisfy more than
// one branch.
static const char *const binary_names[LB_BIN_COUNT] = {
    "=", "<>", "and", "or", "&", "&&", "contains", "starts", "<", ">",
    "<=", ">=", "within", "intersects", "+", "-", "*", "/", "mod", "^"};
static lv_t binary_op(lv_runtime_t *r, unsigned op, lv_t a, lv_t b);
static lv_t binary_guarded(lv_runtime_t *r, unsigned op, lv_t a, lv_t b);
// The numeric tail of a binary operator: the coerced operands, and whether
// both were integral, are all it needs. The bytecode loop reaches it directly
// for two numbers, where nothing earlier in the chain could apply.
static inline lv_t numeric_binary(lv_runtime_t *r, unsigned op, double x, double y,
                                  bool integral) {
  double result = 0;
  switch (op) {
  case LB_BIN_ADD: result = x + y; break;
  case LB_BIN_SUB: result = x - y; break;
  case LB_BIN_MUL: result = x * y; break;
  case LB_BIN_DIV:
    if (!y)
      lv_script_fail(r, "division by zero");
    else
      result = integral ? trunc(x / y) : x / y;
    break;
  case LB_BIN_MOD:
    if (!y)
      lv_script_fail(r, "modulo by zero");
    else
      result = fmod(x, y);
    break;
  case LB_BIN_POW: result = pow(x, y); break;
  case LB_BIN_LT: result = x < y; break;
  case LB_BIN_GT: result = x > y; break;
  case LB_BIN_LE: result = x <= y; break;
  case LB_BIN_GE: result = x >= y; break;
  default: lv_fail(r, "unsupported binary operator"); break;
  }
  if (!isfinite(result))
    lv_fail(r, "non-finite arithmetic");
  // A float result stays a float even when it is integral.
  return integral ? lv_num(result) : lv_float(result);
}
static lv_t binary_op(lv_runtime_t *r, unsigned op, lv_t a, lv_t b) {
  if (op == LB_BIN_EQ)
    return lv_num(same(r, a, b));
  if (op == LB_BIN_NE)
    return lv_num(!same(r, a, b));
  if (op == LB_BIN_AND)
    return lv_num(lv_truth(r, a) && lv_truth(r, b));
  if (op == LB_BIN_OR)
    return lv_num(lv_truth(r, a) || lv_truth(r, b));
  if (op == LB_BIN_CONCAT || op == LB_BIN_CONCAT_SPACE) {
    if ((lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL) &&
        (lv_type(b) == LV_STRING || lv_type(b) == LV_SYMBOL)) {
      unsigned left = string_length(r, a), right = string_length(r, b), space = op == LB_BIN_CONCAT_SPACE;
      lv_t value = allocate(r, LV_STRING, left + right + space + 1);
      if (!r->failed) {
        char *p = (char *)r->heap + r->objects[lv_id(value)].offset;
        memcpy(p, lv_cstr(r, a), left);
        if (space) p[left] = ' ';
        memcpy(p + left + space, lv_cstr(r, b), right);
        p[left + right + space] = 0;
      }
      return value;
    }
    char *joined = r->text_scratch;
    size_t capacity = sizeof(r->text_scratch);
    if (!format_value(r, a, joined, capacity, 0)) {
      lv_fail(r, "concatenation exceeds budget");
      return (lv_t){0};
    }
    size_t n = strlen(joined);
    if (op == LB_BIN_CONCAT_SPACE) {
      if (n + 1 >= capacity) {
        lv_fail(r, "concatenation overflow");
        return (lv_t){0};
      }
      joined[n++] = ' ';
    }
    if (!format_value(r, b, joined + n, capacity - n, 0)) {
      lv_fail(r, "concatenation overflow");
      return (lv_t){0};
    }
    return lv_text(r, joined, false);
  }
  if (op == LB_BIN_CONTAINS || op == LB_BIN_STARTS) {
    // Lingo coerces number operands to their text form: 123 contains "2".
    char left_text[48], right_text[48];
    const char *left = lv_type(a) == LV_NUMBER &&
                               format_value(r, a, left_text, sizeof(left_text), 0)
                           ? left_text
                           : lv_cstr(r, a);
    const char *right = lv_type(b) == LV_NUMBER &&
                                format_value(r, b, right_text, sizeof(right_text), 0)
                            ? right_text
                            : lv_cstr(r, b);
    size_t length = strlen(right);
    do {
      size_t i = 0;
      while (i < length && left[i] &&
             tolower((unsigned char)left[i]) ==
                 tolower((unsigned char)right[i]))
        i++;
      if (i == length)
        return lv_num(1);
      if (op == LB_BIN_STARTS)
        break;
    } while (*left++);
    return lv_num(0);
  }
  if ((lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL) &&
      (lv_type(b) == LV_STRING || lv_type(b) == LV_SYMBOL)) {
    const unsigned char *left = (const unsigned char *)lv_cstr(r, a),
                        *right = (const unsigned char *)lv_cstr(r, b);
    while (*left && *right && tolower(*left) == tolower(*right)) {
      left++;
      right++;
    }
    int order = tolower(*left) - tolower(*right);
    if (op == LB_BIN_LT)
      return lv_num(order < 0);
    if (op == LB_BIN_GT)
      return lv_num(order > 0);
    if (op == LB_BIN_LE)
      return lv_num(order <= 0);
    if (op == LB_BIN_GE)
      return lv_num(order >= 0);
  }
  if ((lv_type(a) == LV_LIST || lv_type(b) == LV_LIST ||
       lv_type(a) == LV_PROPLIST || lv_type(b) == LV_PROPLIST) &&
      (op == LB_BIN_LT || op == LB_BIN_GT || op == LB_BIN_LE || op == LB_BIN_GE)) {
    // Two lists compare element by element over the shorter one and hold
    // only if every pair does (ScummVM compareArrays); a property list takes
    // part through its values. Director 6 stopped comparing a list with a
    // single value, which is then false; before it, every element had to hold.
    bool a_container = lv_type(a) == LV_LIST || lv_type(a) == LV_PROPLIST;
    bool b_container = lv_type(b) == LV_LIST || lv_type(b) == LV_PROPLIST;
#if !DG_D5
    if (!a_container || !b_container) return lv_num(0);
#endif
    unsigned na = a_container ? lv_count(r, a) : 0, nb = b_container ? lv_count(r, b) : 0;
    unsigned count = a_container && b_container ? (na < nb ? na : nb) : (na > nb ? na : nb);
    for (unsigned i = 1; i <= count && !r->failed; i++) {
      lv_t left = lv_type(a) == LV_PROPLIST ? lv_at(r, a, i * 2) : a_container ? lv_at(r, a, i) : a;
      lv_t right = lv_type(b) == LV_PROPLIST ? lv_at(r, b, i * 2) : b_container ? lv_at(r, b, i) : b;
      if (!lv_truth(r, binary_op(r, op, left, right))) return lv_num(0);
    }
    return lv_num(1);
  }
  if (op == LB_BIN_WITHIN || op == LB_BIN_INTERSECTS) {
    lv_t args[2] = {a, b}, result = {0};
    bool yield = false;
    if (!r->services.call ||
        !r->services.call(r, binary_names[op], 2, args, &result, &yield))
      lv_fail(r, "missing collision service");
    return result;
  }
  if ((lv_type(a) == LV_LIST || lv_type(b) == LV_LIST ||
       lv_type(a) == LV_PROPLIST || lv_type(b) == LV_PROPLIST) &&
      (op == LB_BIN_ADD || op == LB_BIN_SUB || op == LB_BIN_MUL ||
       op == LB_BIN_DIV || op == LB_BIN_MOD || op == LB_BIN_POW)) {
    // Element-wise Lingo list arithmetic. A property list on the left keeps
    // its keys and maps the values; on the right it contributes only values
    // (the junk-pile scripts add integers to property lists).
    bool a_container = lv_type(a) == LV_LIST || lv_type(a) == LV_PROPLIST;
    bool b_container = lv_type(b) == LV_LIST || lv_type(b) == LV_PROPLIST;
    unsigned na = a_container ? lv_count(r, a) : UINT32_MAX;
    unsigned nb = b_container ? lv_count(r, b) : UINT32_MAX;
    unsigned count = na < nb ? na : nb;
    bool props = lv_type(a) == LV_PROPLIST;
    lv_t mapped = allocate(r, props ? LV_PROPLIST : LV_LIST, props ? count * 2 : count);
    if (r->failed)
      return (lv_t){0};
    // Point/rect arithmetic returns the same geometric type.
    if (!props) {
      lv_object_t *shape = lv_type(a) == LV_LIST    ? object(r, a)
                           : lv_type(b) == LV_LIST ? object(r, b)
                                               : NULL;
      if (shape && shape->geometry == count)
        r->objects[lv_id(mapped)].geometry = shape->geometry;
    }
    for (unsigned i = 1; i <= count && !r->failed; i++) {
      lv_t left = lv_type(a) == LV_PROPLIST ? lv_at(r, a, i * 2)
                  : lv_type(a) == LV_LIST   ? lv_at(r, a, i)
                                        : a;
      lv_t right = lv_type(b) == LV_PROPLIST ? lv_at(r, b, i * 2)
                   : lv_type(b) == LV_LIST   ? lv_at(r, b, i)
                                         : b;
      if (props && !r->failed)
        lv_set_at(r, mapped, i * 2 - 1, lv_at(r, a, i * 2 - 1));
      lv_set_at(r, mapped, props ? i * 2 : i, binary_guarded(r, op, left, right));
    }
    return mapped;
  }
  double x = lv_number(r, a), y = lv_number(r, b);
  bool integral = integral_operand(r, a) && integral_operand(r, b);
  return numeric_binary(r, op, x, y, integral);
}
static lv_t binary_guarded(lv_runtime_t *r, unsigned op, lv_t a, lv_t b) {
  if (r->arithmetic_depth >= 64) {
    lv_script_fail(r, "recursive list arithmetic");
    return (lv_t){0};
  }
  r->arithmetic_depth++;
  lv_t result = binary_op(r, op, a, b);
  r->arithmetic_depth--;
  return result;
}
// The string form remains for runtime callers that spell an operator out.
lv_t lv_binary(lv_runtime_t *r, const char *op, lv_t a, lv_t b) {
  for (unsigned id = 0; id < LB_BIN_COUNT; id++)
    if (!strcmp(op, binary_names[id])) return binary_guarded(r, id, a, b);
  lv_fail(r, "unsupported binary operator");
  return (lv_t){0};
}
static lv_t unary_op(lv_runtime_t *r, unsigned op, lv_t value) {
  if (op == LB_UN_NOT)
    return lv_num(!lv_truth(r, value));
  return lv_num(op == LB_UN_NEG ? -lv_number(r, value) : lv_number(r, value));
}
lv_t lv_unary(lv_runtime_t *r, const char *op, lv_t value) {
  return unary_op(r, !strcmp(op, "not") ? LB_UN_NOT : !strcmp(op, "-") ? LB_UN_NEG : LB_UN_COUNT,
                  value);
}
#define LV_GLOBAL_SLOTS (sizeof(((lv_runtime_t *)0)->global_slots) / sizeof(uint16_t))
static void index_globals(lv_runtime_t *r) {
  memset(r->global_slots, 0, sizeof(r->global_slots));
  for (unsigned i = 0; i < r->global_count && i < LV_GLOBAL_SLOTS / 2; i++) {
    unsigned probe = lv_text_hash(r->global_names[i]) & (LV_GLOBAL_SLOTS - 1);
    while (r->global_slots[probe]) probe = (probe + 1) & (LV_GLOBAL_SLOTS - 1);
    r->global_slots[probe] = (uint16_t)(i + 1);
  }
}
int lv_global_id(lv_runtime_t *r, const char *name) {
  if (r->global_count > LV_GLOBAL_SLOTS / 2) {
    for (unsigned i = 0; i < r->global_count; i++)
      if (equal_text(name, r->global_names[i]))
        return (int)i;
    return -1;
  }
  for (unsigned probe = lv_text_hash(name) & (LV_GLOBAL_SLOTS - 1);;
       probe = (probe + 1) & (LV_GLOBAL_SLOTS - 1)) {
    unsigned slot = r->global_slots[probe];
    if (!slot) return -1;
    if (equal_text(name, r->global_names[slot - 1])) return (int)(slot - 1);
  }
}
static bool property_key(lv_runtime_t *r, lv_t a, lv_t b) {
  // Symbols are interned, so two are the same key exactly when their ids
  // are; a string key against a symbol or string is compared by text.
  if (lv_type(a) == LV_SYMBOL && lv_type(b) == LV_SYMBOL)
    return lv_id(a) == lv_id(b);
  if ((lv_type(a) == LV_STRING || lv_type(a) == LV_SYMBOL) &&
      (lv_type(b) == LV_STRING || lv_type(b) == LV_SYMBOL)) {
    if (lv_type(a) == lv_type(b) && lv_id(a) == lv_id(b))
      return true;
    return equal_text(lv_cstr(r, a), lv_cstr(r, b));
  }
  if ((lv_type(a) == LV_SYMBOL && lv_type(b) == LV_NUMBER) ||
      (lv_type(a) == LV_NUMBER && lv_type(b) == LV_SYMBOL)) {
    // Mucklas keys its inventory card offsets by card count and reads them
    // with antalKort.string.symbol, so #4 has to find the key 4.
    lv_t number = lv_type(a) == LV_NUMBER ? a : b;
    const char *text = lv_cstr(r, lv_type(a) == LV_SYMBOL ? a : b);
    char *end;
    double parsed = strtod(text, &end);
    return *text && end != text && !*end && isfinite(parsed) && lv_numeric(number) == parsed;
  }
  return same(r, a, b);
}
static unsigned prop_index(lv_runtime_t *r, lv_t owner, lv_t key) {
  lv_object_t *o = object(r, owner);
  if (!o)
    return 0;
  for (unsigned i = 1; i < o->count; i += 2)
    if (property_key(r, lv_at(r, owner, i), key))
      return i + 1;
  return 0;
}
// The pair index of the property named `name` in an instance or property
// list, else 0. Keys are symbols in all but the rare string-keyed lists, so
// the walk is one word compare per pair over the pairs themselves; a string
// key is compared by text, as Director does. The name's symbol id comes
// bound from the generated handler (op_bind) or is interned once.
static unsigned named_property_index(lv_runtime_t *r, lv_t owner,
                                     const char *name) {
  lv_object_t *o = object(r, owner);
  if (!o) return 0;
  unsigned id = lv_symbol_id(r, name);
  if (id == LV_NO_SYMBOL) return 0;
  const lv_t key = lv_make(LV_SYMBOL, (int32_t)id);
  const lv_t *items = (const lv_t *)(r->heap + o->offset);
  for (unsigned i = 0; i + 1 < o->count; i += 2) {
    if (items[i] == key) return i + 2;
    if (lv_type(items[i]) == LV_STRING) {
      lv_object_t *k = object(r, items[i]);
      if (k && equal_text((const char *)r->heap + k->offset, name)) return i + 2;
    }
  }
  return 0;
}
static lv_t singleton(lv_runtime_t *r, lv_t script) {
  unsigned slot = ((uint32_t)lv_id(script) * 2654435761u >> 26) & 63;
  unsigned remembered = r->singleton_memo[slot].index;
  if (r->singleton_memo[slot].script == lv_id(script) && remembered < r->script_count) {
    lv_t value = r->script_objects[remembered];
    if (r->objects[lv_id(value)].script == lv_id(script)) return value;
  }
  for (unsigned i = 0; i < r->script_count; i++) {
    lv_t value = r->script_objects[i];
    if (r->objects[lv_id(value)].script == lv_id(script)) {
      r->singleton_memo[slot].script = lv_id(script);
      r->singleton_memo[slot].index = (uint16_t)i;
      return value;
    }
  }
  if (r->script_count == LV_SCRIPTS) {
    lv_fail(r, "script property table exhausted");
    return (lv_t){0};
  }
  lv_t value = lv_instance(r, script);
  if (!r->failed)
    r->script_objects[r->script_count++] = value;
  return value;
}
lv_t lv_instance(lv_runtime_t *r, lv_t script) {
  if (lv_type(script) != LV_SCRIPT && lv_type(script) != LV_MEMBER) {
    lv_fail(r, "script reference required for instance");
    return (lv_t){0};
  }
  lv_t value = allocate(r, LV_INSTANCE, 0);
  if (!r->failed) {
    r->objects[lv_id(value)].script = lv_id(script);
    // Director clones the script context's current properties at construction.
    // Referenced list/object values retain their identities in that property copy.
    for (unsigned i = 0; i < r->script_count; i++) {
      lv_t source = r->script_objects[i];
      lv_object_t *o = &r->objects[lv_id(source)];
      if (o->script != lv_id(script)) continue;
      unsigned count = o->count;
      for (unsigned j = 1; j <= count && !r->failed; j++)
        lv_set_at(r, value, j, lv_at(r, source, j));
      break;
    }
  }
  return value;
}
lv_t lv_index_get(lv_runtime_t *r, lv_t owner, lv_t key) {
  if (lv_type(owner) == LV_SCRIPT)
    owner = singleton(r, owner);
#if DG_D10
  if (lv_type(owner) == LV_GLOBALS_VIEW) {
    if (lv_type(key) == LV_NUMBER) {
      unsigned index = (unsigned)lv_integer(r, key);
      return index && index <= r->global_count ? r->globals[index - 1]
                                               : (lv_t){0};
    }
    int id = lv_global_id(r, lv_cstr(r, key));
    return id >= 0 ? r->globals[id] : (lv_t){0};
  }
#endif
  if (lv_type(owner) == LV_PROPLIST && lv_type(key) == LV_NUMBER) {
    unsigned index = (unsigned)lv_integer(r, key);
    if (!index || index > lv_count(r, owner)) {
      lv_script_fail(r, "property list index out of range");
      return (lv_t){0};
    }
    return lv_at(r, owner, index * 2);
  }
  if (lv_type(owner) == LV_PROPLIST || lv_type(owner) == LV_INSTANCE) {
    unsigned index = prop_index(r, owner, key);
    return index ? lv_at(r, owner, index) : (lv_t){0};
  }
  return lv_at(r, owner, (unsigned)lv_integer(r, key));
}
void lv_index_set(lv_runtime_t *r, lv_t owner, lv_t key, lv_t value) {
  if (lv_type(owner) == LV_SCRIPT)
    owner = singleton(r, owner);
#if DG_D10
  if (lv_type(owner) == LV_GLOBALS_VIEW) {
    int id = lv_type(key) == LV_NUMBER ? (int)lv_integer(r, key) - 1
                                   : lv_global_id(r, lv_cstr(r, key));
    // A name without a compiled global slot has no reader; the write drops.
    if (id >= 0 && (unsigned)id < r->global_count) r->globals[id] = value;
    return;
  }
#endif
  if (lv_type(owner) == LV_PROPLIST && lv_type(key) == LV_NUMBER) {
    unsigned index = (unsigned)lv_integer(r, key);
    if (!index || index > lv_count(r, owner))
      lv_script_fail(r, "property list index out of range");
    else lv_set_at(r, owner, index * 2, value);
    return;
  }
  if (lv_type(owner) == LV_PROPLIST || lv_type(owner) == LV_INSTANCE) {
    unsigned index = prop_index(r, owner, key);
    if (!index) {
      lv_object_t *o = object(r, owner);
      if (!o)
        return;
      index = o->count + 2;
      lv_set_at(r, owner, index - 1, key);
    }
    lv_set_at(r, owner, index, value);
    return;
  }
  lv_set_at(r, owner, (unsigned)lv_integer(r, key), value);
}
static lv_t script_identity(lv_runtime_t *r, const lv_movie_t *movie,
                             const lv_handler_t *handler) {
  if (r->services.script)
    return r->services.script(r, movie, handler);
  // Host-generated fixtures lack Director's packed archive/cast/member IDs.
  // Hash their stable source identity instead of persisting overlay addresses.
  uint32_t id = 2166136261u;
  const char *parts[] = {movie->name, handler->cast ? handler->cast : ""};
  for (unsigned i = 0; i < 2; i++) {
    for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++)
      id = (id ^ (unsigned)tolower(*p)) * 16777619u;
    id = (id ^ 255u) * 16777619u;
  }
  id = (id ^ handler->member) * 16777619u;
  return lv_make(LV_SCRIPT, (int32_t)id);
}
lv_t lv_self_get(lv_runtime_t *r, lv_frame_t *f, const char *property) {
  if (lv_type(f->self) == LV_VOID)
    f->self = script_identity(r, f->movie, f->handler);
  return lv_get(r, f, property, f->self);
}
void lv_self_set(lv_runtime_t *r, lv_frame_t *f, const char *property,
                 lv_t value) {
  if (lv_type(f->self) == LV_VOID)
    f->self = script_identity(r, f->movie, f->handler);
  lv_set(r, f, property, f->self, value);
}
// Materialize a declared script property as VOID when absent, so the
// property-list protocol over instances counts declarations as Director does.
void lv_self_declare(lv_runtime_t *r, lv_frame_t *f, const char *property) {
  if (lv_type(f->self) == LV_VOID)
    f->self = script_identity(r, f->movie, f->handler);
  lv_t owner = f->self;
  if (lv_type(owner) == LV_SCRIPT) owner = singleton(r, owner);
  if (lv_type(owner) != LV_INSTANCE && lv_type(owner) != LV_PROPLIST) return;
  lv_t key = lv_text(r, property, true);
  if (!prop_index(r, owner, key)) lv_index_set(r, owner, key, (lv_t){0});
}
static unsigned geometry_index(lv_object_t *o, const char *property) {
  if (o->geometry == 2) {
    if (equal_text(property, "loch")) return 1;
    if (equal_text(property, "locv")) return 2;
  }
  if (o->geometry == 4) {
    const char *names[] = {"left", "top", "right", "bottom"};
    for (unsigned i = 0; i < 4; i++)
      if (equal_text(property, names[i])) return i + 1;
  }
  return 0;
}
static bool builtin(lv_runtime_t *, const char *, unsigned, const lv_t *,
                     lv_t *, bool *);
static unsigned inherited_property(lv_runtime_t *r, lv_t *owner, const char *name) {
  lv_t current = *owner;
  for (unsigned depth = 0; depth < LV_OBJECTS; depth++) {
    unsigned index = named_property_index(r, current, name);
    if (index) {*owner = current;return index;}
    if (lv_type(current) != LV_INSTANCE) return 0;
    index = named_property_index(r, current, "ancestor");
    if (!index) return 0;
    current = lv_at(r, current, index);
    if (lv_type(current) != LV_INSTANCE) return 0;
  }
  lv_fail(r, "cyclic script ancestor properties");
  return 0;
}
bool lv_has_property(lv_runtime_t *r, lv_t owner, const char *property) {
  return inherited_property(r, &owner, property) != 0;
}
// A named-property test that rejects on the first byte. Every literal below
// is lowercase, so one folded byte compare answers nearly all of them, and at
// -Os each equal_text against a literal is a real call: a `the ticks` read
// walked fourteen of them before reaching the host's getter, which made
// lv_get the single most expensive function in a station tick.
lv_t lv_get(lv_runtime_t *r, lv_frame_t *f, const char *property, lv_t owner) {
  unsigned ln = lv_name_id(r, property);
  (void)f;
  if (lv_type(owner) == LV_VOID && (ln == LN_ITEMDELIMITER)) {
    char text[2] = {(char)r->item_delimiter, 0};
    return lv_text(r, text, false);
  }
  if (lv_type(owner) == LV_INSTANCE && (ln == LN_SCRIPT)) {
    lv_object_t *o = object(r, owner);
    return o ? lv_make(LV_SCRIPT, o->script) : (lv_t){0};
  }
  // The recovered D8 genLjudModul uses options.ilk to distinguish a sound
  // options dictionary from its enclosing playlist. Treat this type query as
  // its unary builtin before the dictionary's ordinary named properties.
  if (lv_type(owner) == LV_PROPLIST && (ln == LN_ILK)) {
    lv_t result = {0};
    bool yield = false;
    builtin(r, "ilk", 1, &owner, &result, &yield);
    return result;
  }
  if (lv_type(owner) == LV_PROPLIST || lv_type(owner) == LV_INSTANCE ||
      lv_type(owner) == LV_SCRIPT) {
    if (lv_type(owner) == LV_PROPLIST && (ln == LN_COUNT))
      return lv_num(lv_count(r, owner));
    if (lv_type(owner) == LV_SCRIPT) owner = singleton(r, owner);
    unsigned index = inherited_property(r, &owner, property);
#if DG_D10
    // Director objects answer the property-list protocol over their own
    // property pairs; a real property of the same name wins above.
    if (!index && lv_type(owner) == LV_INSTANCE && (ln == LN_COUNT)) {
      lv_object_t *o = object(r, owner);
      return lv_num(o ? o->count / 2 : 0);
    }
#endif
    return index ? lv_at(r, owner, index) : (lv_t){0};
  }
  if (lv_type(owner) == LV_LIST) {
    lv_object_t *o = object(r, owner);
    unsigned index = o ? geometry_index(o, property) : 0;
    if (index) return lv_at(r, owner, index);
    if (ln == LN_COUNT) return lv_num(lv_count(r, owner));
    if (o && o->geometry == 4 &&
        ((ln == LN_WIDTH) || (ln == LN_HEIGHT))) {
      unsigned start = (ln == LN_WIDTH) ? 1u : 2u;
      return lv_binary(r, "-", lv_at(r, owner, start + 2),
                       lv_at(r, owner, start));
    }
  }
  if ((lv_type(owner) == LV_STRING || lv_type(owner) == LV_SYMBOL ||
       lv_type(owner) == LV_BYTES) &&
      ((ln == LN_LENGTH) || (ln == LN_COUNT)))
    return lv_num(lv_count(r, owner));
  // The recovered D8 source uses scalar/list one-argument functions in dot
  // form (value.ilk, n.string, n.integerP); see getObjectProp's builtin fallback.
  if (lv_type(owner) == LV_VOID || lv_type(owner) == LV_NUMBER || lv_type(owner) == LV_STRING ||
      lv_type(owner) == LV_SYMBOL || lv_type(owner) == LV_LIST) {
    const char *properties[] = {"ilk", "integerp", "floatp", "stringp", "symbolp",
                                "voidp", "listp", "string", "symbol", "integer", "float"};
    for (unsigned i = 0; i < sizeof(properties) / sizeof(*properties); i++)
      if (fold_char((unsigned char)property[0]) ==
              (unsigned char)properties[i][0] &&
          equal_text(property, properties[i])) {
        lv_t result = {0};
        bool yield = false;
        if (builtin(r, properties[i], 1, &owner, &result, &yield)) return result;
      }
  }
  if (lv_type(owner) == LV_VOID && (ln == LN_RESULT))
    return r->the_result;
  if (lv_type(owner) == LV_VOID && (ln == LN_MAXINTEGER))
    return lv_num(2147483647);
  if (r->services.get)
    return r->services.get(r, property, owner);
  lv_fail(r, "missing property getter");
  return (lv_t){0};
}
void lv_set(lv_runtime_t *r, lv_frame_t *f, const char *property, lv_t owner,
            lv_t value) {
  unsigned ln = lv_name_id(r, property);
  (void)f;
  if (lv_type(owner) == LV_VOID && (ln == LN_ITEMDELIMITER)) {
    r->item_delimiter = (unsigned char)*lv_cstr(r, value);
    return;
  }
  if (lv_type(owner) == LV_PROPLIST || lv_type(owner) == LV_INSTANCE ||
      lv_type(owner) == LV_SCRIPT) {
    if (lv_type(owner) == LV_SCRIPT) owner = singleton(r, owner);
    unsigned index = inherited_property(r, &owner, property);
    if (!index) {
      lv_object_t *o = object(r, owner);
      if (!o) return;
      index = o->count + 2;
      lv_set_at(r, owner, index - 1, lv_text(r, property, true));
    }
    lv_set_at(r, owner, index, value);
    return;
  }
  if (lv_type(owner) == LV_LIST) {
    lv_object_t *o = object(r, owner);
    unsigned index = o ? geometry_index(o, property) : 0;
    if (index) {
      lv_set_at(r, owner, index, value);
      return;
    }
  }
  if (r->services.set)
    r->services.set(r, property, owner, value);
  else
    lv_fail(r, "missing property setter");
}
#undef PROPERTY_IS
lv_t lv_reference(lv_runtime_t *r, const char *kind, lv_t number,
                  lv_t library) {
  if (!strcmp(kind, "sprite"))
    return lv_make(LV_SPRITE, lv_integer(r, number));
  if (!strcmp(kind, "sound"))
    return lv_make(LV_SOUND, lv_integer(r, number));
  if (r->services.reference)
    return r->services.reference(r, kind, number, library);
  lv_fail(r, "missing cast/member resolver");
  return (lv_t){0};
}
// Director chunks are byte strings in the recovered Western game media. The
// delimiter and -30000 last-chunk sentinel follow Director's native chunk refs.
static bool chunk_unit(lv_runtime_t *r, const char *kind) {
  if (!strcmp(kind, "char") || !strcmp(kind, "word") ||
      !strcmp(kind, "item") || !strcmp(kind, "line")) return true;
  lv_fail(r, "unsupported string chunk unit");
  return false;
}
unsigned lv_chunk_count(lv_runtime_t *r, const char *kind, lv_t source) {
  if (lv_type(source) == LV_FIELD || lv_type(source) == LV_MEMBER)
    source = lv_get(r, NULL, "text", source);
  if (!chunk_unit(r, kind)) return 0;
  const unsigned char *s = (const unsigned char *)lv_cstr(r, source);
  if (!strcmp(kind, "char")) return string_length(r, source);
  unsigned count = 0;
  if (!strcmp(kind, "word")) {
    bool word = false;
    for (; *s; s++) {
      if (!isspace(*s) && !word) count++;
      word = !isspace(*s);
    }
    return count;
  }
  unsigned char delimiter = !strcmp(kind, "item") ? r->item_delimiter : '\r';
  if (!*s) return 0;
  count = 1;
  for (; *s; s++) if (*s == delimiter) count++;
  return count;
}
static bool chunk_bounds(lv_runtime_t *r, const char *kind, lv_t source,
                          lv_t first, lv_t last, size_t *start, size_t *end) {
  if (!chunk_unit(r, kind)) return false;
  const char *s = lv_cstr(r, source);
  size_t len = string_length(r, source);
  int a = lv_integer(r, first), b = lv_integer(r, last);
  if (a == -30000) a = b = (int)lv_chunk_count(r, kind, source);
  if (a < 1) { *start = 0; *end = len; return true; }
  if (b < 1) b = a;
  *start = *end = len;
  if (!strcmp(kind, "char")) {
    if ((size_t)a > len) return false;
    *start = (size_t)a - 1;
    *end = (size_t)b < len ? (size_t)b : len;
    if (*end < *start) *end = *start;
    return true;
  }
  bool words = !strcmp(kind, "word"), found = false;
  char delimiter = !strcmp(kind, "item") ? (char)r->item_delimiter : '\r';
  size_t pos = 0;
  for (int index = 1; ; index++) {
    if (words) while (pos < len && isspace((unsigned char)s[pos])) pos++;
    if (words && pos == len) break;
    if (index == a) { *start = pos; found = true; }
    while (pos < len && (words ? !isspace((unsigned char)s[pos]) :
                                        s[pos] != delimiter)) pos++;
    if (index == b) { *end = pos; break; }
    if (pos == len) break;
    if (!words) pos++;
  }
  if (*end < *start) *end = *start;
  return found;
}
lv_t lv_chunk(lv_runtime_t *r, const char *kind, lv_t source, lv_t first,
              lv_t last) {
  if (lv_type(source) == LV_FIELD || lv_type(source) == LV_MEMBER)
    source = lv_get(r, NULL, "text", source);
  size_t start = 0, end = 0;
  if (!chunk_bounds(r, kind, source, first, last, &start, &end))
    return lv_text(r, "", false);
  unsigned count = (unsigned)(end - start);
  lv_t result = allocate(r, LV_STRING, count + 1);
  if (!r->failed) {
    char *p = (char *)r->heap + r->objects[lv_id(result)].offset;
    memcpy(p, lv_cstr(r, source) + start, count);
    p[count] = 0;
  }
  return result;
}
static lv_t replace_chunk(lv_runtime_t *r, lv_t source, size_t start,
                            size_t end, lv_t value) {
  const char *s = lv_cstr(r, source), *v = lv_cstr(r, value);
  size_t len = strlen(s), n = strlen(v);
  if (start > end || end > len || len - (end - start) + n >= LV_TEXT_BYTES) {
    lv_fail(r, "invalid chunk assignment range");
    return (lv_t){0};
  }
  char *buffer = r->text_scratch;
  memcpy(buffer, s, start);
  memcpy(buffer + start, v, n);
  memcpy(buffer + start + n, s + end, len - end + 1);
  return lv_text(r, buffer, false);
}
lv_t lv_chunk_set(lv_runtime_t *r, const char *kind, lv_t source, lv_t first,
                  lv_t last, lv_t value) {
  if (lv_type(source) == LV_FIELD || lv_type(source) == LV_MEMBER)
    source = lv_get(r, NULL, "text", source);
  size_t start = 0, end = 0;
  (void)chunk_bounds(r, kind, source, first, last, &start, &end);
  if (r->failed) return (lv_t){0};
  if (lv_type(value) != LV_STRING && lv_type(value) != LV_SYMBOL)
    value = string_value(r, value);
  return replace_chunk(r, source, start, end, value);
}
lv_t lv_chunk_delete(lv_runtime_t *r, const char *kind, lv_t source,
                      lv_t first, lv_t last) {
  if (lv_type(source) == LV_FIELD || lv_type(source) == LV_MEMBER)
    source = lv_get(r, NULL, "text", source);
  size_t start = 0, end = 0;
  if (!chunk_bounds(r, kind, source, first, last, &start, &end)) return source;
  const char *s = lv_cstr(r, source);
  size_t len = strlen(s);
  if (!strcmp(kind, "word"))
    while (end < len && isspace((unsigned char)s[end])) end++;
  else if (!strcmp(kind, "item") || !strcmp(kind, "line")) {
    char delimiter = !strcmp(kind, "item") ? (char)r->item_delimiter : '\r';
    if (start && s[start - 1] == delimiter) start--;
    else if (end < len && s[end] == delimiter) end++;
  }
  return replace_chunk(r, source, start, end, (lv_t){0});
}
uint32_t lv_text_hash(const char *name) {
  uint32_t digest = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    digest = (digest ^ fold_char(*p)) * 16777619u;
  return digest;
}
// cast == NULL keeps lv_find semantics (member selects, kind for movie
// scripts); a non-NULL cast additionally requires the handler's cast name.
// The integer and kind tests come first: they read the entry's own line,
// while the name comparison reads the string it points to.
__attribute__((always_inline)) static inline bool
handler_matches(const lv_handler_t *h, const char *name, unsigned member,
                const char *cast) {
  if (cast) {
    // Cast-qualified starts match on the member id alone, as the event
    // broadcast always has.
    if (h->member != member) return false;
  } else if (member ? h->member != member
                    : strcmp(h->kind, "MovieScript") != 0) {
    return false;
  }
  if (!equal_text(h->name, name)) return false;
  return !cast || equal_text(h->cast ? h->cast : "", cast);
}
const lv_handler_t *lv_scan(const lv_movie_t *movie, const char *name,
                            unsigned member, const char *cast) {
  if (movie->handler_buckets) {
    unsigned bucket = lv_text_hash(name) & (movie->bucket_count - 1);
    unsigned end = movie->handler_buckets[bucket + 1];
    for (unsigned i = movie->handler_buckets[bucket]; i < end; i++) {
      const lv_handler_t *h = &movie->handlers[movie->handler_order[i]];
      if (handler_matches(h, name, member, cast)) return h;
    }
    return NULL;
  }
  // Two folded characters: single-character filtering leaves the common
  // event pairs (enterframe/exitframe) colliding on `e`.
  const unsigned char f0 = fold_char((unsigned char)name[0]);
  const unsigned char f1 = f0 ? fold_char((unsigned char)name[1]) : 0;
  for (unsigned i = 0; i < movie->count; i++) {
    const lv_handler_t *h = &movie->handlers[i];
    if (fold_char((unsigned char)h->name[0]) != f0 ||
        (f0 && fold_char((unsigned char)h->name[1]) != f1))
      continue;
    if (handler_matches(h, name, member, cast)) return h;
  }
  return NULL;
}
const lv_handler_t *lv_find(lv_runtime_t *r, const lv_movie_t *movie,
                            const char *name, unsigned member) {
  (void)r; // the index made the per-runtime lookup cache redundant
  if (!movie)
    return NULL;
  return lv_scan(movie, name, member, NULL);
}
// Opens a frame for handler with the argument sequence [*lead, args...]
// copied into its leading locals; lead is optional. Callers that prepend a
// receiver pass it here instead of assembling the sequence in a stack
// array: an LV_LOCALS-sized array in every method call pushed each callee's
// frame 768 bytes further down the C stack, which the console's
// direct-mapped cache paid for (2026-09-26 miss profile).
static bool push_with(lv_runtime_t *r, const lv_movie_t *movie,
                      const lv_handler_t *handler, const lv_t *lead,
                      unsigned argc, const lv_t *args) {
  unsigned total = argc + (lead != NULL);
  lv_t *base = r->stack;
  if (r->depth) {
    const lv_frame_t *caller = &r->frames[r->depth - 1];
    base = caller->temps + (caller->temp_count > caller->temp_extent
                                ? caller->temp_count : caller->temp_extent);
  }
  unsigned room = (unsigned)(r->stack + LV_STACK_SLOTS - base);
  if (r->depth >= LV_FRAMES || total > LV_LOCALS ||
      handler->locals > LV_LOCALS || handler->arguments > handler->locals ||
      handler->locals >= room) {
    lv_script_fail(r, "native call stack exhausted");
    return false;
  }
  if (!r->depth) r->aborted = false;
  lv_frame_t *frame = &r->frames[r->depth++];
  // Generated handlers read only their declared locals. Continuation temps
  // are written before temp_count exposes them to the collector.
  memset(frame, 0, sizeof(*frame));
  frame->locals = base;
  frame->temps = base + handler->locals;
  room -= handler->locals;
  frame->temp_limit = room < LV_LOCALS ? room : LV_LOCALS;
  memset(frame->locals, 0, handler->locals * sizeof(lv_t));
  frame->movie = movie;
  frame->handler = handler;
  frame->pc = handler->entry;
  if (handler->property_count)
    frame->self = script_identity(r, movie, handler);
  unsigned n = 0;
  if (lead && n < handler->arguments) frame->locals[n++] = *lead;
  for (unsigned i = 0; i < argc && n < handler->arguments; i++)
    frame->locals[n++] = args[i];
  r->result = (lv_t){0};
  return true;
}
static bool push(lv_runtime_t *r, const lv_movie_t *movie,
                 const lv_handler_t *handler, unsigned argc, const lv_t *args) {
  return push_with(r, movie, handler, NULL, argc, args);
}
bool lv_start(lv_runtime_t *r, const lv_movie_t *movie, const char *name,
              unsigned member, unsigned argc, const lv_t *args) {
  const lv_handler_t *handler = lv_find(r, movie, name, member);
  if (!handler)
    return false;
  return push(r, movie, handler, argc, args);
}
bool lv_start_cast_args(lv_runtime_t *r, const lv_movie_t *movie,
                        const char *name, unsigned member, const char *cast,
                        unsigned argc, const lv_t *args) {
  if (!movie || argc >= LV_LOCALS - 1)
    return false;
  const lv_handler_t *h = lv_scan(movie, name, member, cast);
  if (h) {
#if DG_D10
      // A frame behavior's event handlers receive the behavior as `me`;
      // its state lives on the script's singleton across the span.
      if (h->arguments) {
        lv_t me = script_identity(r, movie, h);
        if (!push_with(r, movie, h, &me, argc, args))
          return false;
        r->frames[r->depth - 1].self = me;
        return true;
      }
#endif
    return push(r, movie, h, argc, args);
  }
  return false;
}
bool lv_start_cast(lv_runtime_t *r, const lv_movie_t *movie, const char *name,
                   unsigned member, const char *cast) {
  return lv_start_cast_args(r, movie, name, member, cast, 0, NULL);
}
// The game constructs preference/high-score data accesses with do(). Accept
// only assignment between declared data slots and literal data; never interpret
// source handlers, expressions, or commands from strings.
static bool global_expression(lv_runtime_t *r, const char *text, int *index) {
  const char *prefix = "the globals[";
  for (unsigned i = 0; prefix[i]; i++)
    if (!text[i] || tolower((unsigned char)text[i]) != prefix[i]) return false;
  const char *start = text + strlen(prefix), *end = strchr(start, ']');
  if (!end || end[1]) return false;
  if (*start == '#') start++;
  if (end - start >= 2 && *start == '"' && end[-1] == '"') { start++; end--; }
  if (end <= start || (size_t)(end - start) >= 128) return false;
  char key[128];
  memcpy(key, start, (size_t)(end - start));
  key[end - start] = 0;
  *index = lv_global_id(r, key);
  if (*index < 0) lv_fail(r, "unknown dynamic global");
  return true;
}
static bool declared_slot(lv_runtime_t *r, const char *name, lv_t *value,
                           bool write) {
  lv_frame_t *f = r->depth ? &r->frames[r->depth - 1] : NULL;
  if (f) {
    for (unsigned i = 0; f->handler->local_names && i < f->handler->locals; i++)
      if (equal_text(name, f->handler->local_names[i])) {
        if (write) f->locals[i] = *value; else *value = f->locals[i];
        return true;
      }
    for (unsigned i = 0; i < f->handler->property_count; i++)
      if (equal_text(name, f->handler->property_names[i])) {
        if (write) lv_self_set(r, f, name, *value);
        else *value = lv_self_get(r, f, name);
        return true;
      }
  }
  int index = lv_global_id(r, name);
  if (index >= 0) {
    if (write) r->globals[index] = *value; else *value = r->globals[index];
    return true;
  }
  return false;
}
static bool data_assignment(lv_runtime_t *r, const char *text) {
  const char *equals = strchr(text, '=');
  if (!equals || equals == text || (size_t)(equals - text) >= 160) return false;
  char target[160];
  memcpy(target, text, (size_t)(equals - text));
  target[equals - text] = 0;
  const char *source = equals + 1;
  int from = -1, to = -1;
  bool write_global = global_expression(r, target, &to);
  bool read_global = global_expression(r, source, &from);
  if (!write_global && !read_global) return false;
  if (r->failed) return true;
  lv_t value = {0};
  if (read_global) value = r->globals[from];
  else if (!declared_slot(r, source, &value, false)) {
    value = lv_literal(r, source);
    if (lv_type(value) == LV_VOID && !equal_text(source, "void")) {
      lv_fail(r, "dynamic global requires literal data");
      return true;
    }
  }
  if (write_global) r->globals[to] = value;
  else if (!declared_slot(r, target, &value, true))
    lv_fail(r, "dynamic data target is not declared");
  return true;
}
// The builtin chain compares one name against ~100 literals, and the target
// builds at -Os, so each link is a real strcmp call. strcmp already requires
// the first characters to match, so testing that inline only skips the call.
static bool builtin(lv_runtime_t *r, const char *name, unsigned argc,
                    const lv_t *args, lv_t *result, bool *yield) {
  unsigned ln = lv_name_id(r, name);
#define ARITY(n)                                                               \
  do {                                                                         \
    if (argc != (n)) {                                                         \
      lv_fail(r, "builtin argument count");                                    \
      return true;                                                             \
    }                                                                          \
  } while (0)
#define ARG(n) lv_number(r, args[n])
  if (ln == LN_NOTHING) { ARITY(0); return true; }
  if (ln == LN_ABORT) {
    ARITY(0);
    r->aborted = true;
    return true;
  }
  if (ln == LN_CLEARGLOBALS) {
    ARITY(0);
    // Script and factory contexts survive Director clearGlobals; other values
    // become undefined (VOID in the generated fixed-name global table).
    for (unsigned i = 0; i < r->global_count; i++)
      if (lv_type(r->globals[i]) != LV_SCRIPT && lv_type(r->globals[i]) != LV_INSTANCE)
        r->globals[i] = (lv_t){0};
    return true;
  }
  if ((ln == LN_LIST) || (ln == LN_POINT) ||
      (ln == LN_RECT)) {
    if (ln == LN_POINT) { ARITY(2); }
    if ((ln == LN_RECT) && argc == 2) {
      if (lv_type(args[0]) != LV_LIST || lv_type(args[1]) != LV_LIST ||
          lv_count(r, args[0]) != 2 || lv_count(r, args[1]) != 2) {
        lv_fail(r, "rect requires two points or four coordinates");
        return true;
      }
      lv_t values[] = {lv_at(r, args[0], 1), lv_at(r, args[0], 2),
                       lv_at(r, args[1], 1), lv_at(r, args[1], 2)};
      *result = lv_list(r, 4, values, false);
      if (!r->failed) r->objects[lv_id(*result)].geometry = 4;
      return true;
    }
    if (ln == LN_RECT) { ARITY(4); }
    *result = lv_list(r, argc, args, false);
    if (!r->failed && (ln != LN_LIST))
      r->objects[lv_id(*result)].geometry = (uint8_t)argc;
    return true;
  }
  if (ln == LN_RGB) {
    uint32_t packed = 0;
    // Recovered GENMODUL score/reward sprites use the three-component form.
    // Macromedia's Lingo Dictionary, pp. 146/554, also documents rgb("0033FF").
    if (argc == 1 && lv_type(args[0]) == LV_STRING) {
      const char *hex = lv_cstr(r, args[0]);
      if (*hex == '#') hex++;
      if (strlen(hex) != 6) {
        lv_fail(r, "rgb requires six hexadecimal digits");
        return true;
      }
      for (unsigned i = 0; i < 6; i++) {
        unsigned char digit = (unsigned char)tolower((unsigned char)hex[i]);
        if (!isdigit(digit) && !(digit >= 'a' && digit <= 'f')) {
          lv_fail(r, "rgb requires six hexadecimal digits");
          return true;
        }
        packed = (packed << 4) | (digit <= '9' ? digit - '0' : digit - 'a' + 10);
      }
    } else {
      ARITY(3);
      for (unsigned i = 0; i < 3; i++) {
        int32_t channel = lv_integer(r, args[i]);
        if (channel < 0 || channel > 255) {
          lv_fail(r, "rgb component outside 0..255");
          return true;
        }
        packed = (packed << 8) | (unsigned)channel;
      }
    }
    *result = lv_make(LV_COLOR, (int32_t)packed);
    return true;
  }
  if (ln == LN_CASTLIB) {
    ARITY(1);
    *result = lv_reference(r, name, args[0], (lv_t){0});
    return true;
  }
  if ((ln == LN_SCRIPT) || (ln == LN_MEMBER) ||
      (ln == LN_FIELD)) {
    if (argc < 1 || argc > 2) {
      lv_fail(r, "reference argument count");
      return true;
    }
    *result = lv_reference(r, name, args[0], argc == 2 ? args[1] : (lv_t){0});
    return true;
  }
  if (ln == LN_XTRA) goto service;
  if (ln == LN_DUPLICATE) {
    ARITY(1);
    if (lv_type(args[0]) != LV_LIST && lv_type(args[0]) != LV_PROPLIST) {
      *result = args[0];
      return true;
    }
    lv_object_t *source = object(r, args[0]);
    if (!source) return true;
    const lv_t *items = (const lv_t *)(r->heap + source->offset);
    bool flat = true;
    for (unsigned i = 0; i < source->count; i++)
      if (lv_type(items[i]) == LV_LIST || lv_type(items[i]) == LV_PROPLIST) {
        flat = false; break;
      }
    if (flat) {
      // GENMODUL duplicates its sound-instance list on every update. Scalars
      // and script objects retain identity, so this case needs no handle map.
      *result = lv_list(r, source->count, items, lv_type(args[0]) == LV_PROPLIST);
      if (!r->failed) {
        lv_object_t *copy = &r->objects[lv_id(*result)];
        copy->geometry = source->geometry;
        copy->sorted = source->sorted;
        copy->script = source->script;
      }
      return true;
    }
    // This map preserves aliases and cycles when duplicating nested lists.
    // It is static: the exercises duplicate their task records on scenes
    // whose cast overlays leave the system allocator no headroom, and the
    // builtin runs to completion, so one map serves every call.
    _Static_assert(LV_OBJECTS < UINT16_MAX, "duplicate handle map width");
    static uint16_t copies[LV_OBJECTS];
    memset(copies, 0, sizeof(copies));
    *result = duplicate(r, args[0], copies, 0);
    return true;
  }
  if ((ln == LN_INSIDE) || (ln == LN_INTERSECT) ||
      (ln == LN_INTERSECTS) || (ln == LN_UNION)) {
    ARITY(2);
    if (lv_type(args[0]) != LV_LIST || lv_type(args[1]) != LV_LIST) {
      if (ln == LN_INSIDE) {
        // An uninitialized rect (the yard hover scripts test against EMPTY
        // before the first enterFrame) contains nothing; the reference
        // returns FALSE instead of raising.
        *result = lv_num(0);
        return true;
      }
      goto service;
    }
    unsigned left_count = lv_count(r, args[0]);
    if (lv_count(r, args[1]) != 4 ||
        (left_count != 2 && left_count != 4)) {
      if (ln == LN_INSIDE) {
        *result = lv_num(0);
        return true;
      }
      lv_fail(r, "geometry arguments");
      return true;
    }
    double a[4] = {0}, b[4];
    for (unsigned i = 0; i < left_count; i++) a[i] = lv_number(r, lv_at(r, args[0], i + 1));
    for (unsigned i = 0; i < 4; i++) b[i] = lv_number(r, lv_at(r, args[1], i + 1));
    if (ln == LN_INSIDE)
      *result = lv_num(left_count == 2 ? a[0] >= b[0] && a[0] < b[2] && a[1] >= b[1] && a[1] < b[3] :
          a[0] >= b[0] && a[1] >= b[1] && a[2] <= b[2] && a[3] <= b[3]);
    else if (left_count != 4) lv_fail(r, "rectangle required");
    else if (ln == LN_INTERSECTS)
      *result = lv_num(a[0] < b[2] && a[2] > b[0] && a[1] < b[3] && a[3] > b[1]);
    else {
      bool join = (ln == LN_UNION);
      lv_t values[4];
      for (unsigned i = 0; i < 4; i++)
        values[i] = lv_num((join == (i < 2)) ? fmin(a[i], b[i]) : fmax(a[i], b[i]));
      if (!join && (lv_numeric(values[2]) < lv_numeric(values[0]) || lv_numeric(values[3]) < lv_numeric(values[1])))
        memset(values, 0, sizeof(values));
      *result = lv_list(r, 4, values, false);
      if (!r->failed) r->objects[lv_id(*result)].geometry = 4;
    }
    return true;
  }
  if (ln == LN_PI) {
    ARITY(0);
    *result = lv_float(3.141592653589793);
    return true;
  }
  if (ln == LN_GETAT) {
    ARITY(2);
    unsigned index = (unsigned)lv_integer(r, args[1]);
    *result = lv_at(r, args[0], lv_type(args[0]) == LV_PROPLIST ? index * 2 : index);
    return true;
  }
  if (ln == LN_SETAT) {
    ARITY(3);
    unsigned index = (unsigned)lv_integer(r, args[1]);
    if (lv_type(args[0]) == LV_PROPLIST && index > lv_count(r, args[0]))
      lv_script_fail(r, "property list index out of range");
    else
      lv_set_at(r, args[0], lv_type(args[0]) == LV_PROPLIST ? index * 2 : index, args[2]);
    return true;
  }
  if ((ln == LN_ADD) || (ln == LN_APPEND)) {
    ARITY(2);
    lv_object_t *o = object(r, args[0]);
    if (o && o->type == LV_LIST && o->sorted && (ln == LN_ADD)) {
      unsigned at = o->count;
      while (at && lv_truth(r, lv_binary(r, "<", args[1], lv_at(r, args[0], at)))) at--;
      unsigned count = o->count;
      if (resize(r, o, count + 1)) {
        lv_t *items = (lv_t *)(r->heap + o->offset);
        memmove(items + at + 1, items + at, (count - at) * sizeof(*items));
        items[at] = args[1];
      }
      return true;
    }
    lv_set_at(r, args[0], lv_count(r, args[0]) + 1, args[1]);
    return true;
  }
  if (ln == LN_SORT) {
    ARITY(1);
    lv_object_t *o = object(r, args[0]);
    if (!o || (o->type != LV_LIST && o->type != LV_PROPLIST)) {
      lv_fail(r, "sort requires a list");
      return true;
    }
    unsigned stride = o->type == LV_PROPLIST ? 2 : 1;
    for (unsigned i = stride; i < o->count; i += stride) {
      unsigned j = i;
      while (j && lv_truth(r, lv_binary(r, "<", lv_at(r, args[0], j + 1),
                                             lv_at(r, args[0], j - stride + 1)))) {
        lv_t *items = (lv_t *)(r->heap + o->offset);
        for (unsigned k = 0; k < stride; k++) {
          lv_t swap = items[j + k]; items[j + k] = items[j - stride + k]; items[j - stride + k] = swap;
        }
        j -= stride;
      }
    }
    o->sorted = 1;
    return true;
  }
  if (ln == LN_ADDAT) {
    ARITY(3);
    lv_object_t *o = object(r, args[0]);
    unsigned index = (unsigned)lv_integer(r, args[1]);
    if (!o || o->type != LV_LIST || !index || index > o->count + 1)
      lv_fail(r, "invalid addAt");
    else {
      unsigned count = o->count;
      if (resize(r, o, count + 1)) {
        lv_t *items = (lv_t *)(r->heap + o->offset);
        memmove(items + index, items + index - 1,
                (count - index + 1) * sizeof(*items));
        items[index - 1] = args[2];
      }
    }
    return true;
  }
  if (ln == LN_ADDPROP) {
    ARITY(3);
    lv_object_t *o = object(r, args[0]);
    if (!o || o->type != LV_PROPLIST) lv_fail(r, "addProp requires property list");
    else {
      unsigned count = o->count;
      lv_set_at(r, args[0], count + 1, args[1]);
      lv_set_at(r, args[0], count + 2, args[2]);
    }
    return true;
  }
  if (ln == LN_GETPROPAT) {
    ARITY(2);
    unsigned index = (unsigned)lv_integer(r, args[1]);
#if DG_D10
    if (lv_type(args[0]) == LV_GLOBALS_VIEW) {
      if (!index || index > r->global_count) lv_fail(r, "invalid getPropAt");
      else *result = lv_text(r, r->global_names[index - 1], true);
      return true;
    }
#endif
#if DG_D10
    // Instances expose their property pairs through the same protocol.
    bool prop_like = lv_type(args[0]) == LV_PROPLIST || lv_type(args[0]) == LV_INSTANCE;
    lv_object_t *o = object(r, args[0]);
    unsigned pairs = o ? o->count / 2 : 0;
#else
    bool prop_like = lv_type(args[0]) == LV_PROPLIST;
    unsigned pairs = lv_count(r, args[0]);
#endif
    if (!prop_like || !index || index > pairs) lv_fail(r, "invalid getPropAt");
    else *result = lv_at(r, args[0], index * 2 - 1);
    return true;
  }
  if ((ln == LN_GETONE) || (ln == LN_GETPOS) ||
      (ln == LN_FINDPOS) || (ln == LN_FINDPOSNEAR) ||
      (ln == LN_DELETEONE) || (ln == LN_DELETEPROP)) {
    ARITY(2);
    lv_object_t *o = object(r, args[0]);
    if (!o || (o->type != LV_LIST && o->type != LV_PROPLIST)) {
      lv_fail(r, "list search requires list");
      return true;
    }
    unsigned stride = o->type == LV_PROPLIST ? 2u : 1u;
    bool key = stride == 2 && ((ln == LN_FINDPOS) ||
                               (ln == LN_DELETEPROP));
    *result = lv_num(0);
    for (unsigned i = 1; i <= o->count / stride; i++) {
      lv_t value = lv_at(r, args[0], i * stride - (key ? 1u : 0u));
      if (!property_key(r, value, args[1])) continue;
      *result = (ln == LN_GETONE) && stride == 2 ?
                    lv_at(r, args[0], i * stride - 1) : lv_num(i);
      if ((ln == LN_DELETEONE) || (ln == LN_DELETEPROP)) {
        lv_t *items = (lv_t *)(r->heap + o->offset);
        memmove(items + (i - 1) * stride, items + i * stride,
                (o->count - i * stride) * sizeof(*items));
        o->count -= stride;
      }
      break;
    }
    return true;
  }
  if (ln == LN_GETLAST) {
    ARITY(1);
    unsigned count = lv_count(r, args[0]);
    *result = count ? lv_at(r, args[0], count * (lv_type(args[0]) == LV_PROPLIST ? 2u : 1u)) : (lv_t){0};
    return true;
  }
  if (ln == LN_CLEARPROPS) {
    ARITY(1);
    lv_t target = lv_type(args[0]) == LV_SCRIPT ? singleton(r, args[0]) : args[0];
    lv_object_t *o = object(r, target);
    if (o && (o->type == LV_INSTANCE || o->type == LV_PROPLIST)) o->count = 0;
    else lv_fail(r, "clearProps requires property object");
    return true;
  }
  if (ln == LN_DELETEAT) {
    ARITY(2);
    lv_object_t *o = object(r, args[0]);
    unsigned i = (unsigned)lv_integer(r, args[1]);
    unsigned stride = lv_type(args[0]) == LV_PROPLIST ? 2u : 1u;
    if (!o || (o->type != LV_LIST && o->type != LV_PROPLIST) ||
        i < 1 || i > o->count / stride)
      lv_fail(r, "invalid deleteAt");
    else {
      lv_t *v = (lv_t *)(r->heap + o->offset);
      memmove(v + (i - 1) * stride, v + i * stride,
              (o->count - i * stride) * sizeof(*v));
      o->count -= stride;
    }
    return true;
  }
  if ((ln == LN_LENGTH) && argc == 1 &&
      (lv_type(args[0]) == LV_VOID || lv_type(args[0]) == LV_NUMBER)) {
    *result = lv_num(0); return true;
  }
  if ((ln == LN_COUNT) || (ln == LN_COUNT_CHARS) ||
      (ln == LN_COUNT_WORDS) || (ln == LN_COUNT_ITEMS) ||
      (ln == LN_COUNT_LINES) || (ln == LN_LENGTH)) {
    ARITY(1);
    const char *kind = (ln == LN_COUNT_WORDS) ? "word" :
                       (ln == LN_COUNT_ITEMS) ? "item" :
                       (ln == LN_COUNT_LINES) ? "line" : NULL;
    *result = lv_num(kind ? lv_chunk_count(r, kind, args[0]) :
                           lv_count(r, args[0]));
    return true;
  }
  if ((ln == LN_GETPROP) || (ln == LN_GETAPROP) ||
      (ln == LN_SETAPROP) || (ln == LN_SETPROP)) {
    bool set = (ln == LN_SETAPROP) || (ln == LN_SETPROP);
    if (argc != (set ? 3u : 2u)) {
      lv_fail(r, "property list arguments");
      return true;
    }
#if DG_D10
    if (lv_type(args[0]) == LV_SPRITE && r->services.call) {
      lv_t value = {0};
      bool sprite_yield = false;
      if (r->services.call(r, set ? "sprite_setaprop" : "sprite_getaprop",
                           argc, args, &value, &sprite_yield)) {
        if (!set) *result = value;
        return true;
      }
    }
#endif
    if (lv_type(args[0]) == LV_SCRIPT || lv_type(args[0]) == LV_INSTANCE
#if DG_D10
        || lv_type(args[0]) == LV_GLOBALS_VIEW
#endif
    ) {
      if (set) lv_index_set(r, args[0], args[1], args[2]);
      else *result = lv_index_get(r, args[0], args[1]);
      return true;
    }
    lv_object_t *o = object(r, args[0]);
    if (o && o->type == LV_LIST) {
      unsigned i = (unsigned)lv_integer(r, args[1]);
      if (set)
        lv_set_at(r, args[0], i, args[2]);
      else
        *result = lv_at(r, args[0], i);
      return true;
    }
    if (!o || o->type != LV_PROPLIST) {
      lv_fail(r, "expected property list");
      return true;
    }
    for (unsigned i = 1; i < o->count; i += 2)
      if (property_key(r, lv_at(r, args[0], i), args[1])) {
        if (set)
          lv_set_at(r, args[0], i + 1, args[2]);
        else
          *result = lv_at(r, args[0], i + 1);
        return true;
      }
    if (set) {
      unsigned n = o->count;
      lv_set_at(r, args[0], n + 1, args[1]);
      lv_set_at(r, args[0], n + 2, args[2]);
    }
    return true;
  }
  if (ln == LN_RANDOM) {
    ARITY(1);
    double bound = ARG(0);
    if (!isfinite(bound)) {
      lv_fail(r, "invalid random bound");
      return true;
    }
#if DG_CAP_RANDOM16
    // ScummVM b_random uses a 16-bit range for nonpositive bounds. Truncate
    // fractional inputs first, without a potentially overflowing integer cast.
    bound = trunc(bound);
    if (bound <= 0 || bound > 65535)
      bound = 65535;
#endif
    uint32_t x = r->random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->random_state = x;
    if (bound < 1 || bound > 2147483647) {
      lv_fail(r, "invalid random bound");
      return true;
    }
    *result = lv_num(1 + x % (uint32_t)bound);
    return true;
  }
  if ((ln == LN_INTEGER) || (ln == LN_FLOAT) ||
      (ln == LN_ABS) || (ln == LN_SQRT) || (ln == LN_SIN) ||
      (ln == LN_COS) || (ln == LN_ATAN) || (ln == LN_TAN) ||
      (ln == LN_LOG) || (ln == LN_EXP)) {
    ARITY(1);
    if (lv_type(args[0]) == LV_LIST && (ln == LN_FLOAT)) {
      lv_object_t *shape = object(r, args[0]);
      if (shape && shape->geometry) {
        // float(point/rect) maps per coordinate and keeps the geometric type.
        unsigned count = shape->count;
        lv_t mapped = allocate(r, LV_LIST, count);
        if (r->failed)
          return true;
        r->objects[lv_id(mapped)].geometry = r->objects[lv_id(args[0])].geometry;
        for (unsigned i = 1; i <= count && !r->failed; i++) {
          lv_t element = lv_num(lv_number(r, lv_at(r, args[0], i)));
          element = lv_float(lv_numeric(element));
          lv_set_at(r, mapped, i, element);
        }
        *result = mapped;
        return true;
      }
    }
    double x;
    if (lv_type(args[0]) == LV_STRING &&
        ((ln == LN_INTEGER) || (ln == LN_FLOAT))) {
      const char *s = lv_cstr(r, args[0]);
      char *end;
      errno = 0;
      x = strtod(s, &end);
      if (end == s || errno == ERANGE)
        return true;
    } else
      x = ARG(0);
    *result = lv_num((ln == LN_INTEGER) ? round(x)
                     : (ln == LN_ABS)   ? fabs(x)
                     : (ln == LN_SQRT)  ? sqrt(x)
                     : (ln == LN_SIN)   ? sin(x)
                     : (ln == LN_COS)   ? cos(x)
                     : (ln == LN_ATAN)  ? atan(x)
                     : (ln == LN_TAN)   ? tan(x)
                     : (ln == LN_LOG)   ? log(x)
                     : (ln == LN_EXP)   ? exp(x)
                                              : x);
    if (ln == LN_FLOAT)
      *result = lv_float(lv_numeric(*result));
    if (!isfinite(lv_numeric(*result)))
      lv_script_fail(r, "invalid math result");
    return true;
  }
  if (ln == LN_POWER) {
    ARITY(2);
    *result = lv_binary(r, "^", args[0], args[1]);
    return true;
  }
  // Bitwise integer operations: the exercises encode their UTF-8 text with
  // them. Director truncates its operands to 32-bit integers.
  if ((ln == LN_BITOR) || (ln == LN_BITAND) ||
      (ln == LN_BITXOR)) {
    ARITY(2);
    uint32_t left = (uint32_t)(int32_t)lv_number(r, args[0]);
    uint32_t right = (uint32_t)(int32_t)lv_number(r, args[1]);
    *result = lv_num(name[3] == 'o'   ? left | right
                     : name[3] == 'a' ? left & right
                                      : left ^ right);
    return true;
  }
  if (ln == LN_BITNOT) {
    ARITY(1);
    *result = lv_num(~(uint32_t)(int32_t)lv_number(r, args[0]));
    return true;
  }
  if ((ln == LN_MIN) || (ln == LN_MAX)) {
    bool list = argc == 1 && lv_type(args[0]) == LV_LIST;
    unsigned count = list ? lv_count(r, args[0]) : argc;
    if (!count) { *result = lv_num(0); return true; }
    *result = list ? lv_at(r, args[0], 1) : args[0];
    for (unsigned i = 1; i < count; i++) {
      lv_t value = list ? lv_at(r, args[0], i + 1) : args[i];
      if (lv_truth(r, lv_binary(r, (ln == LN_MIN) ? "<" : ">", value, *result)))
        *result = value;
    }
    return true;
  }
  if (ln == LN_STRING) {
    ARITY(1);
    *result = string_value(r, args[0]);
    return true;
  }
  if (ln == LN_NUMTOCHAR) {
    ARITY(1);
    char s[2] = {(char)(unsigned char)lv_integer(r, args[0]), 0};
    *result = lv_bytes(r, s, 1);
    return true;
  }
  if (ln == LN_CHARTONUM) {
    ARITY(1);
    *result = lv_num((unsigned char)*lv_cstr(r, args[0]));
    return true;
  }
  if (ln == LN_OBJECTP) {
    ARITY(1);
    *result = lv_num(lv_type(args[0]) == LV_WINDOW || lv_type(args[0]) == LV_FILE || lv_type(args[0]) == LV_INSTANCE ||
                     lv_type(args[0]) == LV_SCRIPT || lv_type(args[0]) == LV_XTRA ||
                     lv_type(args[0]) == LV_SOUND);
    return true;
  }
  if ((ln == LN_LISTP) || (ln == LN_STRINGP) ||
      (ln == LN_SYMBOLP) || (ln == LN_FLOATP)) {
    ARITY(1);
    *result = lv_num((ln == LN_LISTP) ?
        lv_type(args[0]) == LV_LIST || lv_type(args[0]) == LV_PROPLIST ||
            lv_type(args[0]) == LV_BYTES :
        (ln == LN_STRINGP) ? lv_type(args[0]) == LV_STRING :
        (ln == LN_SYMBOLP) ? lv_type(args[0]) == LV_SYMBOL :
        lv_type(args[0]) == LV_NUMBER && lv_id(args[0]));
    return true;
  }
  if (ln == LN_ILK) {
    if (argc != 1 && argc != 2) {
      lv_fail(r, "ilk argument count");
      return true;
    }
    const char *kind = "void";
    switch (lv_type(args[0])) {
      case LV_NUMBER: kind = lv_is_float(args[0]) ? "float" : "integer"; break;
      case LV_STRING: kind = "string"; break;
      case LV_SYMBOL: kind = "symbol"; break;
      case LV_LIST: {
        lv_object_t *o = object(r, args[0]);
        kind = o && o->geometry == 2 ? "point" : o && o->geometry == 4 ? "rect" : "list";
        break;
      }
      case LV_PROPLIST: kind = "proplist"; break;
      case LV_BYTES: kind = "list"; break;
      case LV_FILE: case LV_INSTANCE: case LV_SCRIPT: case LV_XTRA: kind = "object"; break;
      case LV_SOUND: kind = "sound"; break;
      case LV_MEMBER: case LV_FIELD: kind = "member"; break;
      default: break;
    }
    *result = argc == 2 ? lv_num(equal_text(kind, lv_cstr(r, args[1]))) : lv_text(r, kind, true);
    return true;
  }
  if (ln == LN_VOIDP) {
    ARITY(1);
    *result = lv_num(lv_type(args[0]) == LV_VOID);
    return true;
  }
  if (ln == LN_INTEGERP) {
    ARITY(1);
    *result = lv_num(lv_type(args[0]) == LV_NUMBER && !lv_is_float(args[0]));
    return true;
  }
  if (ln == LN_CHARS) {
    ARITY(3);
    *result = lv_chunk(r, "char", args[0], args[1], args[2]);
    return true;
  }
  if (ln == LN_OFFSET) {
    if (argc == 3) {
      if (lv_type(args[0]) != LV_LIST || lv_count(r, args[0]) != 4) {
        lv_fail(r, "offset requires rectangle");
        return true;
      }
      double h = ARG(1), v = ARG(2);
      *result = lv_rect(r, lv_number(r, lv_at(r, args[0], 1)) + h,
                        lv_number(r, lv_at(r, args[0], 2)) + v,
                        lv_number(r, lv_at(r, args[0], 3)) + h,
                        lv_number(r, lv_at(r, args[0], 4)) + v);
      return true;
    }
    ARITY(2);
    const char *needle = lv_cstr(r, args[0]), *haystack = lv_cstr(r, args[1]);
    size_t length = strlen(needle);
    *result = lv_num(0);
    if (!length) return true;
    for (size_t start = 0; haystack[start]; start++) {
      size_t i = 0;
      while (i < length && haystack[start + i] &&
          tolower((unsigned char)haystack[start + i]) == tolower((unsigned char)needle[i])) i++;
      if (i == length) { *result = lv_num((double)start + 1); break; }
    }
    return true;
  }
  if (ln == LN_VALUE) {
    ARITY(1);
    if (lv_type(args[0]) != LV_STRING) { *result = args[0]; return true; }
    const char *text = lv_cstr(r, args[0]);
    while (isspace((unsigned char)*text)) text++;
    if (!*text) { *result = lv_num(0); return true; }
    // Director evaluates the text in the calling handler's variable frame, so
    // a bare name reads that handler's local first: Findus Weihnachten's
    // recipe door picks one of five local lists with value("recept" & n).
    const char *end = text;
    if (isalpha((unsigned char)*end) || *end == '_')
      while (isalnum((unsigned char)*end) || *end == '_') end++;
    if (end != text && !*end && declared_slot(r, text, result, false)) return true;
    int global = lv_global_id(r, text);
    *result = global >= 0 ? r->globals[global] : lv_literal(r, text);
    return true;
  }
  if (ln == LN_DO) {
    ARITY(1);
    if (data_assignment(r, lv_cstr(r, args[0]))) return true;
    goto service;
  }
  if (ln == LN_SYMBOL) {
    ARITY(1);
    *result = lv_text(r, lv_cstr(r, args[0]), true);
    return true;
  }
service:
  if (r->services.call && r->services.call(r, name, argc, args, result, yield))
    return true;
  return false;
#undef ARG
#undef ARITY
}
static bool selector_identifier(const char **cursor, char *out, size_t capacity) {
  const char *p = *cursor;
  if (!isalpha((unsigned char)*p) && *p != '_') return false;
  size_t length = 0;
  while (isalnum((unsigned char)*p) || *p == '_') {
    if (length + 1 >= capacity) return false;
    out[length++] = (char)tolower((unsigned char)*p++);
  }
  out[length] = 0;
  *cursor = p;
  return true;
}
static bool native_selector(lv_runtime_t *r, const char *source, char *name,
                              size_t capacity, lv_t *receiver, unsigned *argc,
                              lv_t *args) {
  while (isspace((unsigned char)*source)) source++;
  if (!selector_identifier(&source, name, capacity)) return false;
  if (*source == '.') {
    if (!declared_slot(r, name, receiver, false)) return false;
    source++;
    if (!selector_identifier(&source, name, capacity)) return false;
  }
  if (equal_text(name, "do")) return false;
  while (isspace((unsigned char)*source)) source++;
  bool parens = *source == '(';
  if (parens) source++;
  else if (*source && !isalnum((unsigned char)*source) &&
           !strchr("_#\"[+-.", *source)) return false;
  while (isspace((unsigned char)*source)) source++;
  *argc = 0;
  if (!*source && !parens) return true;
  if (*source == ')' && parens) {
    source++;
    while (isspace((unsigned char)*source)) source++;
    return !*source;
  }
  for (;;) {
    if (*argc == LV_LOCALS) return false;
    const char *start = source;
    unsigned brackets = 0;
    bool quoted = false;
    while (*source) {
      char c = *source;
      if (c == '"') quoted = !quoted;
      if (!quoted) {
        if (c == '[') brackets++;
        else if (c == ']') { if (!brackets) return false; brackets--; }
        else if (!brackets && (c == ',' || c == ')')) break;
      }
      source++;
    }
    if (quoted || brackets) return false;
    const char *end = source;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (end <= start || (size_t)(end - start) >= 512) return false;
    char argument[512];
    memcpy(argument, start, (size_t)(end - start));
    argument[end - start] = 0;
    lv_t value = {0};
    if (!declared_slot(r, argument, &value, false)) {
      value = lv_literal(r, argument);
      if (lv_type(value) == LV_VOID && !equal_text(argument, "void")) return false;
    }
    args[(*argc)++] = value;
    if (*source == ',') {
      source++;
      while (isspace((unsigned char)*source)) source++;
      continue;
    }
    if (parens) {
      if (*source != ')') return false;
      source++;
    }
    while (isspace((unsigned char)*source)) source++;
    return !*source;
  }
}
static const lv_handler_t *resolve_method(lv_runtime_t *, lv_frame_t *, lv_t *,
                                          const char *, const lv_movie_t **);
static lv_flow_t invoke(lv_runtime_t *, lv_frame_t *, const char *, unsigned,
                        const lv_t *, bool, bool);
// The rare call shapes keep their buffers in frames of their own, entered
// only when taken: invoke runs for every call, and its frame once held a
// 128-byte selector, an LV_LOCALS value array for do() and another for the
// file objects, 1.6 KB that pushed every callee that far down the C stack.
static __attribute__((noinline)) bool invoke_do(lv_runtime_t *r, lv_frame_t *f,
                                               lv_t source, bool expression,
                                               lv_flow_t *flow) {
  char target[128];
  lv_t receiver = {0}, values[LV_LOCALS];
  unsigned count = 0;
  // HUSET/NY use handler selectors; KP passes declared globals as arguments;
  // NB dispatches a selected method on a declared object. Only this bounded
  // call shape and literal data are accepted, never expressions or bodies.
  if (!native_selector(r, lv_cstr(r, source), target, sizeof(target),
                       &receiver, &count, values))
    return false;
  if (lv_type(receiver) != LV_VOID)
    *flow = expression ? lv_invoke_method_expr(r, f, target, receiver, count, values)
                       : lv_invoke_method(r, f, target, receiver, count, values);
  else
    *flow = invoke(r, f, target, count, values, expression, false);
  return true;
}
// The two file objects are variables in the source, not named handlers.
static __attribute__((noinline)) lv_flow_t invoke_file(lv_runtime_t *r, lv_t receiver,
                                                      unsigned argc, const lv_t *args,
                                                      bool expression) {
  lv_t arguments[LV_LOCALS + 1], value = {0};
  bool yield = false;
  if (argc > LV_LOCALS) {
    lv_fail(r, "file call arguments");
    return LV_RETURN;
  }
  arguments[0] = receiver;
  memcpy(arguments + 1, args, argc * sizeof(lv_t));
  if (!r->services.call || !r->services.call(r, "file_method", argc + 1,
                                             arguments, &value, &yield))
    lv_fail(r, "missing file service");
  r->result = value;
  if (!expression && lv_type(value) != LV_VOID)
    r->the_result = value;
  return yield ? LV_YIELD : LV_CONTINUE;
}
static __attribute__((noinline)) void unimplemented_call(lv_runtime_t *r,
                                                        const char *name) {
  char message[100];
  snprintf(message, sizeof(message), "unimplemented call: %s", name);
  lv_script_fail(r, message);
}
// `pooled`: the name is a movie's pooled name text, whose address is its
// identity, so the site cache below may key on it.
static lv_flow_t invoke(lv_runtime_t *r, lv_frame_t *f, const char *name,
                        unsigned argc, const lv_t *args, bool expression, bool pooled) {
  unsigned ln = lv_name_id(r, name);
  if (r->aborted) return LV_CONTINUE;
  if ((ln == LN_DO) && argc == 1 && lv_type(args[0]) == LV_STRING) {
    lv_flow_t flow;
    if (invoke_do(r, f, args[0], expression, &flow)) return flow;
  }
#if DG_D10
  // Lingo call(#handler, receiver[, args…]) dispatches by name. A receiver
  // without the handler is skipped, and a list dispatches to each element;
  // frames stack in reverse so the first receiver runs first.
  if ((ln == LN_CALL) && argc >= 2 &&
      (lv_type(args[0]) == LV_SYMBOL || lv_type(args[0]) == LV_STRING)) {
    const char *target = lv_cstr(r, args[0]);
    lv_t receivers = args[1];
    unsigned count = lv_type(receivers) == LV_LIST ? lv_count(r, receivers) : 1;
    lv_flow_t flow = LV_CONTINUE;
    for (unsigned i = count; i >= 1; i--) {
      lv_t receiver =
          lv_type(receivers) == LV_LIST ? lv_at(r, receivers, i) : receivers;
      lv_t context = receiver;
      const lv_movie_t *owner = NULL;
      if (lv_type(receiver) != LV_INSTANCE && lv_type(receiver) != LV_SCRIPT) continue;
      if (!resolve_method(r, f, &context, target, &owner)) continue;
      flow = expression
                 ? lv_invoke_method_expr(r, f, target, receiver, argc - 2, args + 2)
                 : lv_invoke_method(r, f, target, receiver, argc - 2, args + 2);
      if (r->failed) break;
    }
    return flow;
  }
#endif
  // Classic Lingo method syntax supplies the receiver as the first argument.
  if (argc && (lv_type(args[0]) == LV_INSTANCE || lv_type(args[0]) == LV_SCRIPT)) {
    const lv_movie_t *owner = NULL;
    lv_t context = args[0];
    if ((ln == LN_NEW) || resolve_method(r, f, &context, name, &owner))
      return expression ? lv_invoke_method_expr(r, f, name, args[0], argc - 1, args + 1) :
                          lv_invoke_method(r, f, name, args[0], argc - 1, args + 1);
  }
  if ((argc == 1 && lv_type(args[0]) == LV_CASTLIB && (ln == LN_SAVE)) ||
      (argc == 2 && lv_type(args[1]) == LV_CASTLIB && (ln == LN_NEW))) {
    lv_t result = {0}; bool yield = false;
    if (!builtin(r, name, argc, args, &result, &yield)) lv_fail(r, "missing mutable cast service");
    r->result = result;
    return yield ? LV_YIELD : LV_CONTINUE;
  }
  if ((ln == LN_NEW) && argc &&
      (lv_type(args[0]) == LV_STRING || lv_type(args[0]) == LV_SYMBOL ||
       lv_type(args[0]) == LV_NUMBER || lv_type(args[0]) == LV_MEMBER)) {
    lv_t script = args[0];
    if (lv_type(script) != LV_SCRIPT)
      script = lv_reference(r, "script", script, (lv_t){0});
    return expression ? lv_invoke_method_expr(r, f, name, script, argc - 1, args + 1) :
                        lv_invoke_method(r, f, name, script, argc - 1, args + 1);
  }
  if ((ln == LN_NEW) && argc && lv_type(args[0]) == LV_XTRA) {
    // A script constructor can itself construct an Xtra. That explicit
    // receiver takes precedence over the caller's implicit script `new`.
    lv_t result = {0};
    bool yield = false;
    if (!builtin(r, name, argc, args, &result, &yield))
      lv_fail(r, "missing Xtra constructor service");
    r->result = result;
    return yield ? LV_YIELD : LV_CONTINUE;
  }
  // Unqualified calls to another handler in the same script retain its `me`
  // context but keep the supplied argument list (LC::call).
  if (lv_type(f->self) == LV_SCRIPT || lv_type(f->self) == LV_INSTANCE) {
    const lv_movie_t *owner = NULL;
    lv_t context = f->self;
    const lv_handler_t *method = resolve_method(r, f, &context, name, &owner);
    if (method) {
      if (push(r, owner, method, argc, args)) {
        r->frames[r->depth - 1].self = context;
        r->frames[r->depth - 1].expression = expression;
      }
      return LV_CALL;
    }
  }
  // The site cache: what this name from this handler resolved to last
  // time, keyed by the pooled name's address, so a hit costs three word
  // compares and no string compare. A frame without a receiver first looks
  // for a local handler of its own cast script, so whether it has one is
  // part of the key; an overlay coming or going retires every entry.
  bool self_void = lv_type(f->self) == LV_VOID;
  lv_call_cache_t *cache = NULL;
  const lv_handler_t *handler = NULL;
  const lv_movie_t *movie = f->movie;
  if (pooled && f->handler) {
    uint32_t hash = (uint32_t)((uintptr_t)name >> 2) * 2654435761u ^
                    (uint32_t)((uintptr_t)f->handler >> 4) * 40503u;
    cache = &r->call_cache[hash % LV_CALL_SLOTS];
    if (cache->valid && cache->name == name && cache->caller == f->handler &&
        cache->self_void == self_void && cache->generation == r->call_generation) {
      if (cache->handler) {
        if (push(r, cache->owner, cache->handler, argc, args))
          r->frames[r->depth - 1].expression = expression;
        return LV_CALL;
      }
      goto unresolved;
    }
  }
  // Frame/cast script events also have local handlers, even when they have no
  // behavior instance. Match both cast and member before looking at globals.
  if (self_void && f->handler && f->movie)
    handler = lv_scan(f->movie, name, f->handler->member,
                      f->handler->cast ? f->handler->cast : "");
  if (!handler) handler = lv_find(r, f->movie, name, 0);
  // External behaviors can call handlers in the currently playing movie.
  if (!handler && r->current && r->current != f->movie) {
    movie = r->current;
    handler = lv_find(r, movie, name, 0);
  }
  for (unsigned i = 0; !handler && i < r->shared_count; i++) {
    movie = r->shared[i];
    handler = lv_find(r, movie, name, 0);
  }
  if (cache) {
    cache->valid = 1;
    cache->name = name;
    cache->caller = f->handler;
    cache->self_void = (uint8_t)self_void;
    cache->generation = r->call_generation;
    cache->owner = movie;
    cache->handler = handler;
  }
  if (handler) {
    if (push(r, movie, handler, argc, args))
      r->frames[r->depth - 1].expression = expression;
    return LV_CALL;
  }
unresolved:;
  lv_t result = {0};
  bool yield = false;
  if (builtin(r, name, argc, args, &result, &yield)) {
    r->result = result;
    return yield ? LV_YIELD : LV_CONTINUE;
  }
  // The two file objects are variables in the source, not named handlers.
  lv_t receiver = {0};
  if (f->handler->local_names)
    for (unsigned i = 0; i < f->handler->locals; i++)
      if (equal_text(f->handler->local_names[i], name))
        receiver = f->locals[i];
  int global = lv_global_id(r, name);
  if (lv_type(receiver) == LV_VOID && global >= 0)
    receiver = r->globals[global];
  if (lv_type(receiver) == LV_FILE)
    return invoke_file(r, receiver, argc, args, expression);
  unimplemented_call(r, name);
  r->result = result;
  return yield ? LV_YIELD : LV_CONTINUE;
}
lv_flow_t lv_invoke(lv_runtime_t *r, lv_frame_t *f, const char *name,
                    unsigned argc, const lv_t *args) {
  return invoke(r, f, name, argc, args, false, false);
}
lv_flow_t lv_invoke_expr(lv_runtime_t *r, lv_frame_t *f, const char *name,
                         unsigned argc, const lv_t *args) {
  return invoke(r, f, name, argc, args, true, false);
}
static const lv_handler_t *resolve_direct_method(lv_runtime_t *r, lv_frame_t *f,
                                          lv_t receiver, const char *name,
                                          const lv_movie_t **movie) {
  lv_t script = receiver;
  if (lv_type(receiver) == LV_INSTANCE) {
    lv_object_t *o = object(r, receiver);
    if (!o) return NULL;
    script = lv_make(LV_SCRIPT, o->script);
  }
  if (lv_type(script) != LV_SCRIPT && lv_type(script) != LV_MEMBER) return NULL;
  if (r->services.resolve) return r->services.resolve(r, script, name, movie);
  const lv_movie_t *candidates[LV_SHARED + 2] = {f ? f->movie : NULL, r->current};
  for (unsigned i = 0; i < r->shared_count && i < LV_SHARED; i++)
    candidates[i + 2] = r->shared[i];
  for (unsigned j = 0; j < LV_SHARED + 2; j++) {
    if (!candidates[j]) continue;
    for (unsigned i = 0; i < candidates[j]->count; i++) {
      const lv_handler_t *h = &candidates[j]->handlers[i];
      if (equal_text(name, h->name) &&
          lv_id(script_identity(r, candidates[j], h)) == lv_id(script)) {
        *movie = candidates[j];
        return h;
      }
    }
  }
  return NULL;
}
static const lv_handler_t *resolve_method(lv_runtime_t *r, lv_frame_t *f,
                                          lv_t *context, const char *name,
                                          const lv_movie_t **movie) {
  // Ancestors are values, never pointers into unloadable movie overlays.
  lv_t receiver = *context;
  for (unsigned depth = 0; depth < LV_OBJECTS; depth++) {
    const lv_handler_t *handler = resolve_direct_method(r, f, receiver, name, movie);
    if (handler) { *context = receiver; return handler; }
    if (lv_type(receiver) != LV_INSTANCE) return NULL;
    // Missing implicit methods are checked before every builtin in a script
    // object. Do not allocate a temporary symbol (and trigger GC) per lookup.
    unsigned index = named_property_index(r, receiver, "ancestor");
    if (!index) return NULL;
    receiver = lv_at(r, receiver, index);
  }
  lv_fail(r, "cyclic script ancestor chain");
  return NULL;
}

static bool push_method(lv_runtime_t *r, const lv_movie_t *movie,
                         const lv_handler_t *handler, lv_t receiver,
                         unsigned argc, const lv_t *args, bool expression) {
  if (argc >= LV_LOCALS) {
    lv_fail(r, "method argument budget");
    return false;
  }
  if (!push_with(r, movie, handler, &receiver, argc, args)) return false;
  lv_frame_t *child = &r->frames[r->depth - 1];
  child->self = receiver;
  child->expression = expression;
  return true;
}
bool lv_start_method(lv_runtime_t *r, lv_t receiver, const char *name,
                      unsigned argc, const lv_t *args) {
  const lv_movie_t *movie = NULL;
  lv_frame_t *f = r->depth ? &r->frames[r->depth - 1] : NULL;
  lv_t context = receiver;
  const lv_handler_t *handler = resolve_method(r, f, &context, name, &movie);
  if (!handler || !push_method(r, movie, handler, receiver, argc, args, false)) return false;
  r->frames[r->depth - 1].self = context;
  return true;
}
// A builtin applied to a receiver, with the receiver prepended to the
// arguments in this frame rather than in invoke_method's, which every
// method call enters (see push_with).
static __attribute__((noinline)) lv_flow_t method_builtin(lv_runtime_t *r, const char *name,
                                                         lv_t receiver, unsigned argc,
                                                         const lv_t *args, bool expression) {
  lv_t arguments[LV_LOCALS + 1], result = {0};
  if (argc > LV_LOCALS) {
    lv_fail(r, "method argument budget");
    return LV_RETURN;
  }
  arguments[0] = receiver;
  if (argc) memcpy(arguments + 1, args, argc * sizeof(lv_t));
  bool yield = false;
  if (builtin(r, name, argc + 1, arguments, &result, &yield)) {
    r->result = result;
    if (!expression && lv_type(result) != LV_VOID) r->the_result = result;
    return yield ? LV_YIELD : LV_CONTINUE;
  }
  char message[100];
  snprintf(message, sizeof(message), "unimplemented method: %s (receiver %u)",
           name, (unsigned)lv_type(receiver));
  lv_script_fail(r, message);
  return LV_RETURN;
}
// `contiguous`, when given, is the receiver followed by the argc arguments
// in one array (the frame's temp stack, as the method ops leave them), so a
// builtin applied to the receiver reads them in place.
static lv_flow_t invoke_method(lv_runtime_t *r, lv_frame_t *f,
                                const char *name, lv_t receiver,
                                unsigned argc, const lv_t *args,
                                bool expression, const lv_t *contiguous) {
  if (r->aborted) return LV_CONTINUE;
  if (lv_type(receiver) == LV_SCRIPT && equal_text(name, "new")) {
    receiver = lv_instance(r, receiver);
    if (r->failed) return LV_RETURN;
  }
  const lv_movie_t *movie = NULL;
  lv_t context = receiver;
  const lv_handler_t *handler = resolve_method(r, f, &context, name, &movie);
  if (handler) {
    if (!push_method(r, movie, handler, receiver, argc, args, expression))
      return LV_RETURN;
    // The original receiver remains the explicit first argument, while
    // unqualified properties belong to the script that supplied the method.
    r->frames[r->depth - 1].self = context;
    return LV_CALL;
  }
  if (lv_type(receiver) == LV_INSTANCE && equal_text(name, "new")) {
    r->result = receiver;
    return LV_CONTINUE;
  }
#if DG_D10
  // object.handler(#name) asks whether the object (or an ancestor)
  // implements the named handler; authored cleanup gates destroy() on it.
  if (equal_text(name, "handler") && argc == 1 &&
      (lv_type(args[0]) == LV_SYMBOL || lv_type(args[0]) == LV_STRING) &&
      (lv_type(receiver) == LV_INSTANCE || lv_type(receiver) == LV_SCRIPT)) {
    const lv_movie_t *probe_movie = NULL;
    lv_t probe_context = receiver;
    r->result = lv_num(resolve_method(r, f, &probe_context,
                                      lv_cstr(r, args[0]), &probe_movie) != NULL);
    if (!expression) r->the_result = r->result;
    return LV_CONTINUE;
  }
#endif
  if (contiguous && contiguous[0] == receiver) {
    lv_t result = {0};
    bool yield = false;
    if (builtin(r, name, argc + 1, contiguous, &result, &yield)) {
      r->result = result;
      if (!expression && lv_type(result) != LV_VOID) r->the_result = result;
      return yield ? LV_YIELD : LV_CONTINUE;
    }
  }
  return method_builtin(r, name, receiver, argc, args, expression);
}
lv_flow_t lv_invoke_method(lv_runtime_t *r, lv_frame_t *f, const char *name,
                            lv_t receiver, unsigned argc, const lv_t *args) {
  return invoke_method(r, f, name, receiver, argc, args, false, NULL);
}
lv_flow_t lv_invoke_method_expr(lv_runtime_t *r, lv_frame_t *f, const char *name,
                                 lv_t receiver, unsigned argc, const lv_t *args) {
  return invoke_method(r, f, name, receiver, argc, args, true, NULL);
}
static void returned(lv_runtime_t *r, const lv_frame_t *frame) {
#if DG_D10
  // Director MX sets `the result` on every handler return, including VOID:
  // authored code reads it after a tell to fetch an optional value and
  // depends on VOID clearing the previous result.
  if (!frame->expression)
    r->the_result = r->result;
#else
  if (!frame->expression && lv_type(r->result) != LV_VOID)
    r->the_result = r->result;
#endif
  r->depth--;
}
static bool abort_run(lv_runtime_t *r) {
  if (!r->aborted) return false;
  // ScummVM b_abort stops execution and execute() discards every call context.
  // The next top-level event remains runnable; this does not quit the movie.
  r->depth = 0;
  r->result = (lv_t){0};
  return true;
}
// One state of the frame's handler: generated handlers run their bytecode,
// hand-written ones their native step function.
static inline lv_flow_t step_frame(lv_runtime_t *r, lv_frame_t *f) {
  return f->handler->code ? lv_vm_step(r, f) : f->handler->step(r, f);
}
// A call to a name the converter knows no handler defines (LB_CALL_BUILTIN):
// the builtin answers first; what it does not answer takes invoke's whole
// path, which is where the file objects and the unimplemented alert live.
static lv_flow_t invoke_builtin(lv_runtime_t *r, lv_frame_t *f, const char *name,
                                unsigned argc, const lv_t *args, bool expression) {
  if (r->aborted) return LV_CONTINUE;
  lv_t result = {0};
  bool yield = false;
  if (builtin(r, name, argc, args, &result, &yield)) {
    r->result = result;
    return yield ? LV_YIELD : LV_CONTINUE;
  }
  return invoke(r, f, name, argc, args, expression, false);
}
// Run whatever an expression call started to completion, atomically.
static lv_t atomic_call(lv_runtime_t *r, unsigned depth, lv_flow_t flow) {
  abort_run(r);
  if (flow == LV_YIELD)
    lv_fail(r, "yielding expression call");
  unsigned steps = 0;
  while (!r->failed && r->depth > depth && steps++ < 65536) {
    lv_frame_t *child = &r->frames[r->depth - 1];
    flow = step_frame(r, child);
    if (abort_run(r)) break;
    if (flow == LV_RETURN)
      returned(r, child);
    else if (flow == LV_YIELD)
      lv_fail(r, "yielding expression handler");
  }
  if (r->depth > depth)
    lv_fail(r, "expression call budget exceeded");
  r->atomic_depth--;
  return r->result;
}
lv_t lv_call_builtin(lv_runtime_t *r, lv_frame_t *f, const char *name, unsigned argc,
                     const lv_t *args) {
  unsigned depth = r->depth;
  r->atomic_depth++;
  return atomic_call(r, depth, invoke_builtin(r, f, name, argc, args, true));
}
lv_t lv_call(lv_runtime_t *r, lv_frame_t *f, const char *name, unsigned argc,
             const lv_t *args) {
  unsigned depth = r->depth;
  r->atomic_depth++;
  return atomic_call(r, depth, lv_invoke_expr(r, f, name, argc, args));
}
// As lv_call, for a name whose address is its identity (a pooled name, a
// string literal): the call goes through the site cache as bytecode does.
lv_t lv_call_pooled(lv_runtime_t *r, lv_frame_t *f, const char *name, unsigned argc,
                    const lv_t *args) {
  unsigned depth = r->depth;
  r->atomic_depth++;
  return atomic_call(r, depth, invoke(r, f, name, argc, args, true, true));
}
lv_t lv_call_method(lv_runtime_t *r, lv_frame_t *f, const char *name,
                     lv_t receiver, unsigned argc, const lv_t *args) {
  unsigned depth = r->depth;
  r->atomic_depth++;
  lv_flow_t flow = lv_invoke_method_expr(r, f, name, receiver, argc, args);
  abort_run(r);
  if (flow == LV_YIELD) lv_fail(r, "yielding expression method");
  unsigned steps = 0;
  while (!r->failed && r->depth > depth && steps++ < 65536) {
    lv_frame_t *child = &r->frames[r->depth - 1];
    flow = step_frame(r, child);
    if (abort_run(r)) break;
    if (flow == LV_RETURN) returned(r, child);
    else if (flow == LV_YIELD) lv_fail(r, "yielding expression method");
  }
  if (r->depth > depth) lv_fail(r, "expression method budget exceeded");
  r->atomic_depth--;
  return r->result;
}
// Synchronous method dispatch that keeps ordinary collection running: the
// behavior lifecycle events (new, beginSprite) fan out over a thousand
// channels, and an atomic span that long exhausts the heap with garbage no
// pass may touch. Callers must hold their values where the collector can
// see them (rooted containers or non-heap references), which the score
// dispatch sites do.
lv_t lv_dispatch_method(lv_runtime_t *r, lv_frame_t *f, const char *name,
                        lv_t receiver, unsigned argc, const lv_t *args) {
  unsigned depth = r->depth;
  lv_flow_t flow = lv_invoke_method_expr(r, f, name, receiver, argc, args);
  abort_run(r);
  if (flow == LV_YIELD) lv_fail(r, "yielding expression method");
  unsigned steps = 0;
  while (!r->failed && r->depth > depth && steps++ < 65536) {
    if (!r->atomic_depth) {
      if (r->allocations_since_gc >= LV_OBJECTS / 2 ||
#if DG_CAP_EMERGENCY_COLLECT
          r->object_count > LV_OBJECTS - 256 ||
#else
          r->object_count > LV_OBJECTS - LV_OBJECTS / 4 ||
#endif
#if DG_D10
          r->heap_used > LV_HEAP_BYTES / 2
#else
          r->heap_used > LV_HEAP_BYTES * 3 / 4
#endif
      )
        lv_collect(r);
#if DG_CAP_EMERGENCY_COLLECT
      r->step_serial = r->allocation_serial;
#endif
    }
    lv_frame_t *child = &r->frames[r->depth - 1];
    flow = step_frame(r, child);
    if (abort_run(r)) break;
    if (flow == LV_RETURN) returned(r, child);
    else if (flow == LV_YIELD) lv_fail(r, "yielding expression method");
  }
  if (r->depth > depth) lv_fail(r, "expression method budget exceeded");
  return r->result;
}
bool lv_run(lv_runtime_t *r, unsigned budget) {
  r->yielded = false;
  while (!r->failed && r->depth && budget--) {
    r->steps++;
    // Only a collection frees handles, so the allocation count is really a
    // handle budget; half the table is what every profile can spare between
    // passes. The uncompacted D5/D6 pass costs tens of milliseconds, and a
    // fixed 256 spent that on 3% of an 8192-entry table.
    if (r->allocations_since_gc >= LV_OBJECTS / 2 ||
#if DG_CAP_EMERGENCY_COLLECT
        r->object_count > LV_OBJECTS - 256 ||
#else
        // Collection cannot run inside a step here — there is no in-place
        // emergency pass below D7 — so a quarter of the table stays spare
        // for whatever one step or atomic span allocates.
        r->object_count > LV_OBJECTS - LV_OBJECTS / 4 ||
#endif
        // Collection runs between steps, so whatever a single step needs must
        // fit in what is left. The D10 rollover-speech lists grow by tens of
        // kilobytes at a time; half the heap is the margin they need.
#if DG_D10
        r->heap_used > LV_HEAP_BYTES / 2
#else
        r->heap_used > LV_HEAP_BYTES * 3 / 4
#endif
    )
      lv_collect(r);
#if DG_CAP_EMERGENCY_COLLECT
    // The step boundary for the emergency in-place collector: everything
    // allocated from here on survives an allocation-failure pass.
    r->step_serial = r->allocation_serial;
#endif
    lv_frame_t *frame = &r->frames[r->depth - 1];
    lv_flow_t flow = step_frame(r, frame);
    if (abort_run(r)) break;
    if (flow == LV_RETURN)
      returned(r, frame);
    if (flow == LV_YIELD) {
      r->yielded = true;
      break;
    }
  }
  return !r->failed;
}
void lv_trace(lv_runtime_t *r, lv_t value) {
  if (r->services.trace)
    r->services.trace(r, value);
}
// Compact expression ops. They run only inside a generated handler's step,
// so the executing frame is the top of the call stack, and its temp array —
// which both collectors already treat as roots — is the expression stack.
// Ops keep their stack delta even after a failure so generated sequences
// stay balanced, and a suspension simply leaves its operands on the stack.
// Forced inline: every generated expression op is itself a call, and without
// this each one paid a second call into the stack helper plus the multiply
// that indexes a frame (lv_frame_t was then about three kilobytes). Pushing was the
// most-executed function in a station tick at nearly four hundred calls per
// tick, almost none of it doing work.
#define LX_INLINE __attribute__((always_inline)) static inline
LX_INLINE lv_frame_t *lx_frame(lv_runtime_t *r) {
  return &r->frames[r->depth - 1];
}
// The op bodies take the executing frame explicitly. Within one state it is
// the top of the call stack and never moves (an expression call runs its
// callees above it and returns to the same depth; an invoke ends the
// state), so the bytecode loop passes the frame it was given instead of
// re-indexing the frame table on every op. Forced inline into that loop,
// an op is its stack traffic and the one service call it makes.
LX_INLINE const char *op_name(const lv_frame_t *f, unsigned index) {
  return f->movie->names[index].text;
}
// Bind the name a service is about to receive to its build-time ids, so the
// dispatch chains compare integers (lv_name_id) and the property protocol
// its symbol (lv_symbol_id). A hand-written movie's records say
// LB_NAME_COUNT and LV_NO_SYMBOL, which is what the searches would find.
LX_INLINE const char *op_bind(lv_runtime_t *r, const lv_frame_t *f, unsigned index) {
  const lv_name_t *name = &f->movie->names[index];
  r->name_id_for = name->text;
  r->name_id = name->name_id;
  r->symbol_for = name->text;
  r->symbol = name->symbol_id;
  return name->text;
}
LX_INLINE void op_push(lv_runtime_t *r, lv_frame_t *f, lv_t value) {
  if (f->temp_count >= f->temp_limit) {
    // Short of the per-frame limit, it is recursion that used the slots up.
    if (f->temp_limit < LV_LOCALS)
      lv_script_fail(r, "native call stack exhausted");
    else
      lv_fail(r, "native expression stack exceeded");
    return;
  }
  f->temps[f->temp_count++] = value;
}
LX_INLINE lv_t op_pop(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->temp_count) {
    lv_fail(r, "native expression stack underflow");
    return (lv_t){0};
  }
  return f->temps[--f->temp_count];
}
// Pop count values but keep them addressable: nothing reuses the storage
// before the consumer copies them, and both collectors still see them
// through the frame while any consumer-triggered pass runs in place.
static lv_t *op_args(lv_runtime_t *r, lv_frame_t *f, unsigned count) {
  static lv_t none;
  if (f->temp_count < count) {
    lv_fail(r, "native expression stack underflow");
    return &none;
  }
  if (f->temp_count > f->temp_extent) f->temp_extent = f->temp_count;
  f->temp_count -= count;
  return count ? f->temps + f->temp_count : &none;
}
// What lv_num returns for an integer, without the fractional test.
LX_INLINE lv_t op_integer(int32_t value) {
  return lv_num(value);
}
LX_INLINE void op_num(lv_runtime_t *r, lv_frame_t *f, int32_t value) {
  op_push(r, f, op_integer(value));
}
LX_INLINE void op_numd(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  op_push(r, f, lv_num(f->movie->doubles[index]));
}
LX_INLINE void op_dbl(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  // Authored float literals stay floats even when integral.
  op_push(r, f, lv_float(f->movie->doubles[index]));
}
LX_INLINE void op_text(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  op_push(r, f, lv_text(r, op_name(f, index), false));
}
LX_INLINE void op_sym(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  // A literal the converter numbered is a constant; a hand-written movie's
  // is interned.
  const lv_name_t *name = &f->movie->names[index];
  if (name->symbol_id != LV_NO_SYMBOL)
    op_push(r, f, lv_make(LV_SYMBOL, (int32_t)name->symbol_id));
  else
    op_push(r, f, lv_text(r, name->text, true));
}
LX_INLINE void op_void(lv_runtime_t *r, lv_frame_t *f) { op_push(r, f, (lv_t){0}); }
LX_INLINE void op_local(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  op_push(r, f, f->locals[index]);
}
LX_INLINE void op_global(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  op_push(r, f, r->globals[index]);
}
LX_INLINE void op_self(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  op_push(r, f, lv_self_get(r, f, op_bind(r, f, index)));
}
// A declared property at the pair the compiler's hint names: the instance's
// key there is the property's symbol (one word compare), so the value is
// the next word, and the named lookup with its ancestor walk never runs.
// Anything else — a script property shadowed by `script`, an instance whose
// pairs were reordered by a script, a property list — takes the named path.
LX_INLINE lv_t *self_slot(lv_runtime_t *r, lv_frame_t *f, unsigned index, unsigned slot) {
  op_bind(r, f, index);
  if (lv_type(f->self) == LV_VOID)
    f->self = script_identity(r, f->movie, f->handler);
  lv_t owner = f->self;
  if (lv_type(owner) == LV_SCRIPT) owner = singleton(r, owner);
  if (lv_type(owner) != LV_INSTANCE || r->symbol == LV_NO_SYMBOL || r->name_id == LN_SCRIPT)
    return NULL;
  lv_object_t *o = object(r, owner);
  if (!o || slot + 1 >= o->count) return NULL;
  lv_t *items = (lv_t *)(r->heap + o->offset);
  if (items[slot] != lv_make(LV_SYMBOL, (int32_t)r->symbol)) return NULL;
  return &items[slot + 1];
}
LX_INLINE void op_self_slot(lv_runtime_t *r, lv_frame_t *f, unsigned index, unsigned slot) {
  lv_t *item = self_slot(r, f, index, slot);
  op_push(r, f, item ? *item : lv_self_get(r, f, f->movie->names[index].text));
}
LX_INLINE void op_the(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  op_push(r, f, lv_get(r, f, op_bind(r, f, index), (lv_t){0}));
}
LX_INLINE void op_get(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t owner = op_pop(r, f);
  op_push(r, f, lv_get(r, f, op_bind(r, f, index), owner));
}
LX_INLINE void op_unary(lv_runtime_t *r, lv_frame_t *f, unsigned op) {
  lv_t value = op_pop(r, f);
  op_push(r, f, unary_op(r, op, value));
}
// Two numbers under an arithmetic or relational operator go straight to the
// numeric tail: nothing earlier in binary_op's chain applies to them. The
// equality and logical operators are the chain's first tests for any operand
// types, so they go straight to their helpers too.
LX_INLINE lv_t binary_fast(lv_runtime_t *r, unsigned op, lv_t a, lv_t b) {
  if (lv_type(a) == LV_NUMBER && lv_type(b) == LV_NUMBER && op >= LB_BIN_LT &&
      op != LB_BIN_WITHIN && op != LB_BIN_INTERSECTS)
    return numeric_binary(r, op, lv_numeric(a), lv_numeric(b), !lv_is_float(a) && !lv_is_float(b));
  switch (op) {
  case LB_BIN_EQ: return lv_num(same(r, a, b));
  case LB_BIN_NE: return lv_num(!same(r, a, b));
  case LB_BIN_AND: return lv_num(lv_truth(r, a) && lv_truth(r, b));
  case LB_BIN_OR: return lv_num(lv_truth(r, a) || lv_truth(r, b));
  default: return binary_guarded(r, op, a, b);
  }
}
LX_INLINE void op_binary(lv_runtime_t *r, lv_frame_t *f, unsigned op) {
  lv_t right = op_pop(r, f), left = op_pop(r, f);
  op_push(r, f, binary_fast(r, op, left, right));
}
LX_INLINE void op_index(lv_runtime_t *r, lv_frame_t *f) {
  lv_t index = op_pop(r, f), owner = op_pop(r, f);
  op_push(r, f, lv_index_get(r, owner, index));
}
LX_INLINE void op_chunk(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t source = op_pop(r, f), last = op_pop(r, f), first = op_pop(r, f);
  op_push(r, f, lv_chunk(r, op_name(f, index), source, first, last));
}
LX_INLINE void op_chunk_count(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t owner = op_pop(r, f);
  op_push(r, f, lv_num(lv_chunk_count(r, op_name(f, index), owner)));
}
LX_INLINE void op_chunk_set(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t last = op_pop(r, f), first = op_pop(r, f), original = op_pop(r, f),
       value = op_pop(r, f);
  op_push(r, f, lv_chunk_set(r, op_name(f, index), original, first, last, value));
}
LX_INLINE void op_chunk_delete(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t last = op_pop(r, f), first = op_pop(r, f), original = op_pop(r, f);
  op_push(r, f, lv_chunk_delete(r, op_name(f, index), original, first, last));
}
LX_INLINE void op_last_chunk(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t owner = op_pop(r, f);
  lv_t count = lv_num(lv_chunk_count(r, op_name(f, index), owner));
  op_push(r, f, lv_chunk(r, op_name(f, index), owner, count, count));
}
LX_INLINE void op_list(lv_runtime_t *r, lv_frame_t *f, unsigned count, unsigned props) {
  lv_t *items = op_args(r, f, count);
  op_push(r, f, lv_list(r, count, items, props));
}
// Append count stacked values to the list that remains on the stack top.
// Long literal lists build in segments so their construction never needs
// more expression slots than one segment.
LX_INLINE void op_list_extend(lv_runtime_t *r, lv_frame_t *f, unsigned count) {
  lv_t *items = op_args(r, f, count);
  if (!f->temp_count) {
    lv_fail(r, "native expression stack underflow");
    return;
  }
  lv_object_t *obj = object(r, f->temps[f->temp_count - 1]);
  if (!obj || obj->type < LV_LIST) {
    lv_fail(r, "native list extension target");
    return;
  }
  unsigned base = obj->count;
  if (resize(r, obj, base + count))
    memcpy(r->heap + obj->offset + base * sizeof(lv_t), items,
           count * sizeof(lv_t));
}
LX_INLINE void op_reference(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t library = op_pop(r, f), number = op_pop(r, f);
  op_push(r, f, lv_reference(r, op_bind(r, f, index), number, library));
}
LX_INLINE void op_call(lv_runtime_t *r, lv_frame_t *f, unsigned index, unsigned argc) {
  lv_t *args = op_args(r, f, argc);
  op_push(r, f, lv_call(r, f, op_bind(r, f, index), argc, args));
}
LX_INLINE void op_call_builtin_expr(lv_runtime_t *r, lv_frame_t *f, unsigned index,
                                    unsigned argc) {
  lv_t *args = op_args(r, f, argc);
  op_push(r, f, lv_call_builtin(r, f, op_bind(r, f, index), argc, args));
}
LX_INLINE lv_flow_t op_call_builtin(lv_runtime_t *r, lv_frame_t *f, unsigned index,
                                    unsigned argc) {
  lv_t *args = op_args(r, f, argc);
  return invoke_builtin(r, f, op_bind(r, f, index), argc, args, false);
}
LX_INLINE void op_result(lv_runtime_t *r, lv_frame_t *f) { op_push(r, f, r->result); }
LX_INLINE void op_trace(lv_runtime_t *r, lv_frame_t *f) { lv_trace(r, op_pop(r, f)); }
LX_INLINE void op_set_local(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  f->locals[index] = op_pop(r, f);
}
LX_INLINE void op_set_global(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  r->globals[index] = op_pop(r, f);
}
LX_INLINE void op_set_self(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t value = op_pop(r, f);
  lv_self_set(r, f, op_bind(r, f, index), value);
}
LX_INLINE void op_set_self_slot(lv_runtime_t *r, lv_frame_t *f, unsigned index, unsigned slot) {
  lv_t value = op_pop(r, f);
  lv_t *item = self_slot(r, f, index, slot);
  if (item)
    *item = value;
  else
    lv_self_set(r, f, f->movie->names[index].text, value);
}
LX_INLINE void op_set_the(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t value = op_pop(r, f);
  lv_set(r, f, op_bind(r, f, index), (lv_t){0}, value);
}
LX_INLINE void op_set(lv_runtime_t *r, lv_frame_t *f, unsigned index) {
  lv_t owner = op_pop(r, f), value = op_pop(r, f);
  lv_set(r, f, op_bind(r, f, index), owner, value);
}
LX_INLINE void op_set_index(lv_runtime_t *r, lv_frame_t *f) {
  lv_t index = op_pop(r, f), owner = op_pop(r, f), value = op_pop(r, f);
  lv_index_set(r, owner, index, value);
}
LX_INLINE lv_flow_t op_branch(lv_runtime_t *r, lv_frame_t *f, unsigned yes, unsigned no) {
  lv_t condition = op_pop(r, f);
  f->pc = lv_truth(r, condition) ? yes : no;
  return LV_CONTINUE;
}
LX_INLINE lv_flow_t op_return(lv_runtime_t *r, lv_frame_t *f) {
  r->result = op_pop(r, f);
  return LV_RETURN;
}
LX_INLINE lv_flow_t op_invoke(lv_runtime_t *r, lv_frame_t *f, unsigned index, unsigned argc) {
  lv_t *args = op_args(r, f, argc);
  return invoke(r, f, op_bind(r, f, index), argc, args, false, true);
}
// A call the converter bound to one of this movie's handlers (see
// LB_INVOKE_LOCAL): the frame's own script defines the name, so the lookup
// invoke would have made ends at that entry whether the frame has a
// receiver or not. Classic method syntax on an explicit receiver still
// resolves by name, as invoke does before any lookup.
LX_INLINE lv_flow_t op_invoke_local(lv_runtime_t *r, lv_frame_t *f, unsigned entry,
                                    unsigned argc, bool own, bool expression) {
  lv_t *args = op_args(r, f, argc);
  if (r->aborted) return LV_CONTINUE;
  const lv_handler_t *handler = &f->movie->handlers[entry];
  if (argc && (lv_type(args[0]) == LV_INSTANCE || lv_type(args[0]) == LV_SCRIPT))
    return invoke(r, f, handler->name, argc, args, expression, false);
  if (push(r, f->movie, handler, argc, args)) {
    lv_frame_t *child = &r->frames[r->depth - 1];
    // Unqualified calls within a script retain its `me` context; a movie
    // script's handler runs without one.
    if (own && lv_type(f->self) != LV_VOID) child->self = f->self;
    child->expression = expression;
  }
  return LV_CALL;
}
LX_INLINE lv_flow_t op_invoke_method(lv_runtime_t *r, lv_frame_t *f, unsigned index,
                                     unsigned argc) {
  lv_t *args = op_args(r, f, argc + 1);
  return invoke_method(r, f, op_bind(r, f, index), args[0], argc, args + 1, false, args);
}
LX_INLINE lv_flow_t op_invoke_expr(lv_runtime_t *r, lv_frame_t *f, unsigned index,
                                   unsigned argc) {
  lv_t *args = op_args(r, f, argc);
  return invoke(r, f, op_bind(r, f, index), argc, args, true, true);
}
LX_INLINE lv_flow_t op_invoke_method_expr(lv_runtime_t *r, lv_frame_t *f, unsigned index,
                                          unsigned argc) {
  lv_t *args = op_args(r, f, argc + 1);
  return invoke_method(r, f, op_bind(r, f, index), args[0], argc, args + 1, true, args);
}
// The public services for hand-written step functions (tests and adapters):
// the same bodies over the top frame.
void lx_num(lv_runtime_t *r, int32_t value) { op_num(r, lx_frame(r), value); }
void lx_numd(lv_runtime_t *r, unsigned index) { op_numd(r, lx_frame(r), index); }
void lx_dbl(lv_runtime_t *r, unsigned index) { op_dbl(r, lx_frame(r), index); }
void lx_text(lv_runtime_t *r, unsigned index) { op_text(r, lx_frame(r), index); }
void lx_sym(lv_runtime_t *r, unsigned index) { op_sym(r, lx_frame(r), index); }
void lx_void(lv_runtime_t *r) { op_void(r, lx_frame(r)); }
void lx_local(lv_runtime_t *r, unsigned index) { op_local(r, lx_frame(r), index); }
void lx_global(lv_runtime_t *r, unsigned index) { op_global(r, lx_frame(r), index); }
void lx_self(lv_runtime_t *r, unsigned index) { op_self(r, lx_frame(r), index); }
void lx_the(lv_runtime_t *r, unsigned index) { op_the(r, lx_frame(r), index); }
void lx_get(lv_runtime_t *r, unsigned index) { op_get(r, lx_frame(r), index); }
void lx_unary(lv_runtime_t *r, unsigned op) { op_unary(r, lx_frame(r), op); }
void lx_binary(lv_runtime_t *r, unsigned op) { op_binary(r, lx_frame(r), op); }
void lx_index(lv_runtime_t *r) { op_index(r, lx_frame(r)); }
void lx_chunk(lv_runtime_t *r, unsigned index) { op_chunk(r, lx_frame(r), index); }
void lx_chunk_count(lv_runtime_t *r, unsigned index) { op_chunk_count(r, lx_frame(r), index); }
void lx_chunk_set(lv_runtime_t *r, unsigned index) { op_chunk_set(r, lx_frame(r), index); }
void lx_chunk_delete(lv_runtime_t *r, unsigned index) {
  op_chunk_delete(r, lx_frame(r), index);
}
void lx_last_chunk(lv_runtime_t *r, unsigned index) { op_last_chunk(r, lx_frame(r), index); }
void lx_list(lv_runtime_t *r, unsigned count, unsigned props) {
  op_list(r, lx_frame(r), count, props);
}
void lx_list_extend(lv_runtime_t *r, unsigned count) { op_list_extend(r, lx_frame(r), count); }
void lx_reference(lv_runtime_t *r, unsigned index) { op_reference(r, lx_frame(r), index); }
void lx_call(lv_runtime_t *r, unsigned index, unsigned argc) {
  op_call(r, lx_frame(r), index, argc);
}
void lx_result(lv_runtime_t *r) { op_result(r, lx_frame(r)); }
void lx_trace(lv_runtime_t *r) { op_trace(r, lx_frame(r)); }
void lx_set_local(lv_runtime_t *r, unsigned index) { op_set_local(r, lx_frame(r), index); }
void lx_set_global(lv_runtime_t *r, unsigned index) { op_set_global(r, lx_frame(r), index); }
void lx_set_self(lv_runtime_t *r, unsigned index) { op_set_self(r, lx_frame(r), index); }
void lx_set_the(lv_runtime_t *r, unsigned index) { op_set_the(r, lx_frame(r), index); }
void lx_set(lv_runtime_t *r, unsigned index) { op_set(r, lx_frame(r), index); }
void lx_set_index(lv_runtime_t *r) { op_set_index(r, lx_frame(r)); }
lv_flow_t lx_branch(lv_runtime_t *r, unsigned yes, unsigned no) {
  return op_branch(r, lx_frame(r), yes, no);
}
lv_flow_t lx_return(lv_runtime_t *r) { return op_return(r, lx_frame(r)); }
lv_flow_t lx_invoke(lv_runtime_t *r, unsigned index, unsigned argc) {
  return op_invoke(r, lx_frame(r), index, argc);
}
lv_flow_t lx_invoke_method(lv_runtime_t *r, unsigned index, unsigned argc) {
  return op_invoke_method(r, lx_frame(r), index, argc);
}
lv_flow_t lx_invoke_expr(lv_runtime_t *r, unsigned index, unsigned argc) {
  return op_invoke_expr(r, lx_frame(r), index, argc);
}
lv_flow_t lx_invoke_method_expr(lv_runtime_t *r, unsigned index,
                                unsigned argc) {
  return op_invoke_method_expr(r, lx_frame(r), index, argc);
}

// `value` is used for data-file fields. Only literals are accepted: this is not
// a runtime source evaluator, and file content cannot invoke native handlers.
static void spaces(const char **p) {
  while (isspace((unsigned char)**p))
    (*p)++;
}
static lv_t literal(lv_runtime_t *r, const char **p, unsigned depth,
                    bool *valid) {
  spaces(p);
  if (depth > 64) {
    *valid = false;
    return (lv_t){0};
  }
  // Geometry constructors are serialized data in Director behavior parameters
  // and game databases. Accept only numeric coordinates, never function calls.
  unsigned geometry = !strncmp(*p, "point(", 6) ? 2 : !strncmp(*p, "rect(", 5) ? 4 : 0;
  if (geometry) {
    *p += geometry == 2 ? 6 : 5;
    lv_t coordinates[4] = {0};
    for (unsigned i = 0; i < geometry; i++) {
      coordinates[i] = literal(r, p, depth + 1, valid);
      spaces(p);
      if (!*valid || lv_type(coordinates[i]) != LV_NUMBER || **p != (i + 1 == geometry ? ')' : ',')) {
        *valid = false;
        return (lv_t){0};
      }
      (*p)++;
    }
    lv_t value = lv_list(r, geometry, coordinates, false);
    if (!r->failed) r->objects[lv_id(value)].geometry = geometry;
    return value;
  }
#if DG_D10
  // Behavior parameters serialize authored member references. Resolve the
  // pair through the movie's cast table; this is data, never a handler call.
  if (!strncmp(*p, "(member ", 8)) {
    *p += 8;
    lv_t number = literal(r, p, depth + 1, valid);
    spaces(p);
    if (*valid && !strncmp(*p, "of castLib ", 11)) {
      *p += 11;
      lv_t library = literal(r, p, depth + 1, valid);
      spaces(p);
      if (*valid && **p == ')' && r->services.reference &&
          (lv_type(number) == LV_NUMBER || lv_type(number) == LV_STRING) &&
          (lv_type(library) == LV_NUMBER || lv_type(library) == LV_STRING)) {
        (*p)++;
        lv_t lib_ref = lv_type(library) == LV_NUMBER
                           ? lv_make(LV_CASTLIB, lv_integer(r, library))
                           : library;
        return r->services.reference(r, "member", number, lib_ref);
      }
    }
    *valid = false;
    return (lv_t){0};
  }
#endif
  if (**p == '[') {
    (*p)++;
    spaces(p);
    bool props = **p == ':';
    if (props)
      (*p)++;
    lv_t list = lv_list(r, 0, NULL, props);
    spaces(p);
    if (**p == ']') {
      (*p)++;
      return list;
    }
    for (unsigned n = 0; n < 1024 && *valid; n++) {
      lv_t item = literal(r, p, depth + 1, valid);
      spaces(p);
      if (!n && **p == ':') {
        list = lv_make(LV_PROPLIST, lv_id(list));
        object(r, lv_make(LV_LIST, lv_id(list)))->type = LV_PROPLIST;
        props = true;
      }
      lv_set_at(r, list, lv_count(r, list) * (props ? 2u : 1u) + 1, item);
      if (props) {
        if (**p != ':') {
          *valid = false;
          break;
        }
        (*p)++;
        item = literal(r, p, depth + 1, valid);
        lv_object_t *o = object(r, list);
        if (o)
          lv_set_at(r, list, o->count + 1, item);
      }
      spaces(p);
      if (**p == ']') {
        (*p)++;
        return list;
      }
      if (**p != ',') {
        *valid = false;
        break;
      }
      (*p)++;
    }
    *valid = false;
    return (lv_t){0};
  }
  if (**p == '"' || **p == '#') {
    bool symbol = *(*p)++ == '#';
    const char *start = *p;
    if (symbol)
      while (isalnum((unsigned char)**p) || **p == '_')
        (*p)++;
    else
      while (**p && **p != '"')
        (*p)++;
    size_t n = (size_t)(*p - start);
    if ((!symbol && **p != '"') || (symbol && !n) || n >= LV_TEXT_BYTES) {
      *valid = false;
      return (lv_t){0};
    }
    lv_t value;
    if (symbol) {
      char spelled[128];
      if (n >= sizeof spelled) {
        *valid = false;
        return (lv_t){0};
      }
      memcpy(spelled, start, n);
      spelled[n] = 0;
      value = lv_symbol(r, spelled);
    } else {
      value = allocate(r, LV_STRING, (unsigned)n + 1);
      if (!r->failed) {
        char *out = (char *)r->heap + r->objects[lv_id(value)].offset;
        memcpy(out, start, n);
        out[n] = 0;
      }
      (*p)++;
    }
    return value;
  }
  const char *atoms[] = {"void", "true", "false"};
  for (unsigned i = 0; i < 3; i++) {
    size_t length = strlen(atoms[i]), matched = 0;
    while (matched < length && (*p)[matched] &&
        tolower((unsigned char)(*p)[matched]) == atoms[i][matched]) matched++;
    if (matched == length && !isalnum((unsigned char)(*p)[length]) &&
        (*p)[length] != '_') {
      *p += length;
      return i == 0 ? (lv_t){0} : lv_num(i == 1);
    }
  }
  const char *start = *p;
  char *end;
  errno = 0;
  double n = strtod(start, &end);
  if (end == start || errno == ERANGE || !isfinite(n)) {
    *valid = false;
    return (lv_t){0};
  }
  // Reject libc's non-Lingo hex, NaN and infinity forms.
  for (const char *q = start; q < end; q++)
    if (!strchr("0123456789+-.eE", *q))
      *valid = false;
  *p = end;
  lv_t value = lv_num(n);
  for (const char *q = start; q < end; q++)
    if (*q == '.' || *q == 'e' || *q == 'E')
      value = lv_float(lv_numeric(value));
  return value;
}
lv_t lv_literal(lv_runtime_t *r, const char *text) {
  bool valid = true;
  lv_t value = literal(r, &text, 0, &valid);
  spaces(&text);
  return valid && !*text ? value : (lv_t){0};
}

// Bytecode execution for generated handlers: one state per call, exactly as
// a native step function ran one `case`, so step accounting, collection
// points and yields land where they did. Operands decode big-endian from
// the handler's code, and every op body is inlined here, so an op costs its
// dispatch, its stack traffic and the one service call it makes.
lv_flow_t lv_vm_step(lv_runtime_t *r, lv_frame_t *f) {
  const uint8_t *code = f->handler->code;
  uint32_t pc = f->pc;
  if (pc >= f->handler->code_size) {
    lv_fail(r, "invalid native continuation");
    return LV_RETURN;
  }
#define U8() (code[pc++])
#define I8() ((int32_t)(int8_t)code[pc++])
#define U16() (pc += 2, (unsigned)(code[pc - 2] << 8 | code[pc - 1]))
#define I32()                                                                  \
  (pc += 4, (int32_t)((uint32_t)code[pc - 4] << 24 | (uint32_t)code[pc - 3] << 16 | \
                      (uint32_t)code[pc - 2] << 8 | (uint32_t)code[pc - 1]))
  for (;;) {
    switch (code[pc++]) {
    case LB_LINE: f->line = U16(); break;
    case LB_NUM8: op_num(r, f, I8()); break;
    case LB_NUM32: op_num(r, f, I32()); break;
    case LB_NUMD: op_numd(r, f, U16()); break;
    case LB_DBL: op_dbl(r, f, U16()); break;
    case LB_TEXT: op_text(r, f, U16()); break;
    case LB_SYM: op_sym(r, f, U16()); break;
    case LB_VOID: op_void(r, f); break;
    case LB_LOCAL: op_local(r, f, U8()); break;
    case LB_GLOBAL: op_global(r, f, U16()); break;
    case LB_SELF: op_self(r, f, U16()); break;
    case LB_THE: op_the(r, f, U16()); break;
    case LB_GET: op_get(r, f, U16()); break;
    case LB_UNARY: op_unary(r, f, U8()); break;
    case LB_BINARY: op_binary(r, f, U8()); break;
    case LB_INDEX: op_index(r, f); break;
    case LB_CHUNK: op_chunk(r, f, U16()); break;
    case LB_CHUNK_COUNT: op_chunk_count(r, f, U16()); break;
    case LB_CHUNK_SET: op_chunk_set(r, f, U16()); break;
    case LB_CHUNK_DELETE: op_chunk_delete(r, f, U16()); break;
    case LB_LAST_CHUNK: op_last_chunk(r, f, U16()); break;
    case LB_LIST: {
      unsigned count = U8(), props = U8();
      op_list(r, f, count, props);
      break;
    }
    case LB_LIST_EXTEND: op_list_extend(r, f, U8()); break;
    case LB_REFERENCE: op_reference(r, f, U16()); break;
    case LB_CALL: {
      unsigned name = U16(), argc = U8();
      op_call(r, f, name, argc);
      break;
    }
    case LB_RESULT: op_result(r, f); break;
    case LB_TRACE: op_trace(r, f); break;
    case LB_SET_LOCAL: op_set_local(r, f, U8()); break;
    case LB_SET_GLOBAL: op_set_global(r, f, U16()); break;
    case LB_SET_SELF: op_set_self(r, f, U16()); break;
    case LB_SELF_SLOT: {
      unsigned name = U16(), slot = U8();
      op_self_slot(r, f, name, slot);
      break;
    }
    case LB_SET_SELF_SLOT: {
      unsigned name = U16(), slot = U8();
      op_set_self_slot(r, f, name, slot);
      break;
    }
    case LB_SET_THE: op_set_the(r, f, U16()); break;
    case LB_SET: op_set(r, f, U16()); break;
    case LB_SET_INDEX: op_set_index(r, f); break;
    case LB_DECLARE: lv_self_declare(r, f, op_name(f, U16())); break;
    case LB_JUMP: f->pc = U16(); return LV_CONTINUE;
    case LB_YIELD: f->pc = U16(); return LV_YIELD;
    case LB_BRANCH: {
      unsigned yes = U16(), no = U16();
      return op_branch(r, f, yes, no);
    }
    case LB_RETURN: return op_return(r, f);
    case LB_RETURN_VOID: r->result = (lv_t){0}; return LV_RETURN;
    case LB_INVOKE: {
      unsigned name = U16(), argc = U8();
      f->pc = U16();
      return op_invoke(r, f, name, argc);
    }
    case LB_INVOKE_METHOD: {
      unsigned name = U16(), argc = U8();
      f->pc = U16();
      return op_invoke_method(r, f, name, argc);
    }
    case LB_INVOKE_EXPR: {
      unsigned name = U16(), argc = U8();
      f->pc = U16();
      return op_invoke_expr(r, f, name, argc);
    }
    case LB_CALL_BUILTIN_EXPR: {
      unsigned name = U16(), argc = U8();
      op_call_builtin_expr(r, f, name, argc);
      break;
    }
    case LB_CALL_BUILTIN: {
      unsigned name = U16(), argc = U8();
      f->pc = U16();
      return op_call_builtin(r, f, name, argc);
    }
    case LB_INVOKE_LOCAL: {
      unsigned entry = U16(), argc = U8(), own = U8();
      f->pc = U16();
      return op_invoke_local(r, f, entry, argc, own != 0, false);
    }
    case LB_INVOKE_LOCAL_EXPR: {
      unsigned entry = U16(), argc = U8(), own = U8();
      f->pc = U16();
      return op_invoke_local(r, f, entry, argc, own != 0, true);
    }
    case LB_INVOKE_METHOD_EXPR: {
      unsigned name = U16(), argc = U8();
      f->pc = U16();
      return op_invoke_method_expr(r, f, name, argc);
    }
    default: lv_fail(r, "invalid bytecode"); return LV_RETURN;
    }
  }
#undef U8
#undef I8
#undef U16
#undef I32
}
