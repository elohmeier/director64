#include "package.h"
#include "lingo_bytecode.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *data;
  size_t length, at;
  bool failed;
  char *error;
  size_t error_size;
} reader_t;

static bool fail(reader_t *r, const char *format, ...) {
  if (!r->failed) {
    va_list args;
    va_start(args, format);
    vsnprintf(r->error, r->error_size, format, args);
    va_end(args);
  }
  r->failed = true;
  return false;
}
static bool need(reader_t *r, size_t bytes) {
  if (r->failed)
    return false;
  if (bytes > r->length - r->at)
    return fail(r, "truncated at byte %zu", r->at);
  return true;
}
static uint8_t u8(reader_t *r) {
  if (!need(r, 1))
    return 0;
  return r->data[r->at++];
}
static uint16_t u16(reader_t *r) {
  if (!need(r, 2))
    return 0;
  uint16_t v = (uint16_t)(r->data[r->at] | r->data[r->at + 1] << 8);
  r->at += 2;
  return v;
}
static uint32_t u32(reader_t *r) {
  if (!need(r, 4))
    return 0;
  const uint8_t *p = r->data + r->at;
  r->at += 4;
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static double f64(reader_t *r) {
  if (!need(r, 8))
    return 0;
  uint64_t bits = 0;
  for (unsigned i = 0; i < 8; i++)
    bits |= (uint64_t)r->data[r->at + i] << (8 * i);
  r->at += 8;
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}
// A count of records at least `minimum` bytes each, bounded by what remains.
static uint32_t count(reader_t *r, size_t minimum, uint32_t limit,
                      const char *what) {
  uint32_t n = u32(r);
  if (r->failed)
    return 0;
  if (n > limit || (minimum && n > (r->length - r->at) / minimum)) {
    fail(r, "%s count %u exceeds the package", what, n);
    return 0;
  }
  return n;
}

typedef struct {
  dg_package_t *package;
  reader_t *reader;
  const char *strings;
  size_t strings_length;
  const uint8_t *code;
  size_t code_length;
} loader_t;

static void *allocate(loader_t *l, size_t count, size_t size) {
  dg_package_t *p = l->package;
  if (l->reader->failed)
    return NULL;
  // Empty tables still get one zeroed record, as the generated C's `{0}`.
  if (!count)
    count = 1;
  if (size && count > SIZE_MAX / size) {
    fail(l->reader, "table too large");
    return NULL;
  }
  if (p->block_count == p->block_capacity) {
    unsigned capacity = p->block_capacity ? p->block_capacity * 2 : 256;
    void **blocks = realloc(p->blocks, capacity * sizeof(*blocks));
    if (!blocks) {
      fail(l->reader, "out of memory");
      return NULL;
    }
    p->blocks = blocks;
    p->block_capacity = capacity;
  }
  void *block = calloc(count, size);
  if (!block) {
    fail(l->reader, "out of memory");
    return NULL;
  }
  p->blocks[p->block_count++] = block;
  return block;
}
static const char *string(loader_t *l) {
  uint32_t offset = u32(l->reader);
  if (l->reader->failed)
    return "";
  if (offset >= l->strings_length) {
    fail(l->reader, "string offset %u outside the pool", offset);
    return "";
  }
  return l->strings + offset;
}
static const char *const *string_list(loader_t *l, unsigned n) {
  const char **list = allocate(l, n, sizeof(*list));
  for (unsigned i = 0; list && i < n; i++)
    list[i] = string(l);
  return list;
}


// Walks a handler's ops so the VM never reads past its code or a pool: every
// operand in bounds, every jump on an op boundary, every index inside the
// table it names, and the code ending on a state terminator.
static bool verify_code(reader_t *r, const lv_handler_t *h, const lv_movie_t *movie,
                        unsigned names, unsigned doubles, unsigned globals) {
  const uint8_t *code = h->code;
  uint32_t size = h->code_size;
  uint8_t *boundary = calloc(size, 1);
  uint16_t *targets = calloc(size, sizeof(*targets));
  unsigned target_count = 0;
  bool ok = boundary && targets;
  uint32_t pc = 0;
  uint8_t last = 0;
#define NEED(n) do { if (pc + (n) > size) { ok = false; break; } } while (0)
#define BE16(at) ((unsigned)code[at] << 8 | code[(at) + 1])
  while (ok && pc < size) {
    boundary[pc] = 1;
    uint8_t op = code[pc++];
    last = op;
    unsigned name = UINT32_MAX, dbl = UINT32_MAX, local = UINT32_MAX,
             global = UINT32_MAX, handler = UINT32_MAX, jumps = 0;
    unsigned jump[2] = {0, 0};
    switch (op) {
    case LB_VOID: case LB_INDEX: case LB_RESULT: case LB_TRACE:
    case LB_SET_INDEX: case LB_RETURN: case LB_RETURN_VOID:
      break;
    case LB_NUM8:
      NEED(1); pc += 1; break;
    case LB_NUM32:
      NEED(4); pc += 4; break;
    case LB_LINE:
      NEED(2); pc += 2; break;
    case LB_NUMD: case LB_DBL:
      NEED(2); dbl = BE16(pc); pc += 2; break;
    case LB_LOCAL: case LB_SET_LOCAL:
      NEED(1); local = code[pc]; pc += 1; break;
    case LB_GLOBAL: case LB_SET_GLOBAL:
      NEED(2); global = BE16(pc); pc += 2; break;
    case LB_UNARY:
      NEED(1); ok = code[pc] < LB_UN_COUNT; pc += 1; break;
    case LB_BINARY:
      NEED(1); ok = code[pc] < LB_BIN_COUNT; pc += 1; break;
    case LB_LIST:
      NEED(2); pc += 2; break;
    case LB_LIST_EXTEND:
      NEED(1); pc += 1; break;
    case LB_TEXT: case LB_SYM: case LB_SELF: case LB_THE: case LB_GET:
    case LB_CHUNK: case LB_CHUNK_COUNT: case LB_CHUNK_SET: case LB_CHUNK_DELETE:
    case LB_LAST_CHUNK: case LB_REFERENCE: case LB_SET_SELF: case LB_SET_THE:
    case LB_SET: case LB_DECLARE:
      NEED(2); name = BE16(pc); pc += 2; break;
    case LB_CALL: case LB_CALL_BUILTIN_EXPR: case LB_SELF_SLOT: case LB_SET_SELF_SLOT:
      NEED(3); name = BE16(pc); pc += 3; break;
    case LB_JUMP: case LB_YIELD:
      NEED(2); jump[jumps++] = BE16(pc); pc += 2; break;
    case LB_BRANCH:
      NEED(4); jump[jumps++] = BE16(pc); jump[jumps++] = BE16(pc + 2); pc += 4; break;
    case LB_INVOKE: case LB_INVOKE_METHOD: case LB_INVOKE_EXPR:
    case LB_INVOKE_METHOD_EXPR: case LB_CALL_BUILTIN:
      NEED(5); name = BE16(pc); jump[jumps++] = BE16(pc + 3); pc += 5; break;
    case LB_INVOKE_LOCAL: case LB_INVOKE_LOCAL_EXPR:
      NEED(6); handler = BE16(pc); jump[jumps++] = BE16(pc + 4); pc += 6; break;
    default:
      ok = false;
    }
    if (!ok)
      break;
    if ((name != UINT32_MAX && name >= names) || (dbl != UINT32_MAX && dbl >= doubles) ||
        (local != UINT32_MAX && local >= h->locals) ||
        (global != UINT32_MAX && global >= globals) ||
        (handler != UINT32_MAX && handler >= movie->count)) {
      ok = false;
      break;
    }
    for (unsigned j = 0; j < jumps; j++) {
      if (jump[j] >= size) { ok = false; break; }
      targets[target_count++ % size] = (uint16_t)jump[j];
    }
  }
#undef NEED
#undef BE16
  ok = ok && pc == size && last >= LB_JUMP && boundary[h->entry];
  for (unsigned i = 0; ok && i < target_count && i < size; i++)
    ok = boundary[targets[i]];
  free(boundary);
  free(targets);
  return ok ? true : fail(r, "handler %s has malformed bytecode", h->name);
}

static bool load_handlers(loader_t *l, lv_movie_t *code, unsigned globals, unsigned pools[2]) {
  reader_t *r = l->reader;
  unsigned handlers = count(r, 40, UINT16_MAX, "handler");
  lv_handler_t *entries = allocate(l, handlers, sizeof(*entries));
  for (unsigned i = 0; entries && i < handlers && !r->failed; i++) {
    lv_handler_t *h = &entries[i];
    h->name = string(l);
    uint32_t member = u32(r);
    if (member > UINT16_MAX)
      return fail(r, "handler member %u", member);
    h->member = (uint16_t)member;
    h->cast = string(l);
    h->kind = string(l);
    h->arguments = u32(r);
    h->locals = u32(r);
    h->entry = u32(r);
    h->property_count = u32(r);
    if (h->locals > LV_LOCALS || h->arguments > LV_LOCALS ||
        h->property_count > UINT16_MAX)
      return fail(r, "handler %s declares %u locals", h->name, h->locals);
    h->local_names = string_list(l, h->locals);
    h->property_names = string_list(l, h->property_count);
    uint32_t offset = u32(r), size = u32(r);
    if (r->failed)
      return false;
    if (!size || offset > l->code_length || size > l->code_length - offset)
      return fail(r, "handler %s code outside the package", h->name);
    if (h->entry >= size)
      return fail(r, "handler %s entry outside its code", h->name);
    h->code = l->code + offset;
    h->code_size = size;
  }
  code->count = handlers;
  code->handlers = entries;
  unsigned names = count(r, 8, UINT16_MAX + 1u, "name");
  if (names) {
    lv_name_t *pool = allocate(l, names, sizeof(*pool));
    for (unsigned i = 0; pool && i < names; i++) {
      pool[i].text = string(l);
      pool[i].name_id = u16(r);
      pool[i].symbol_id = u16(r);
      if (pool[i].name_id > LB_NAME_COUNT)
        return fail(r, "name id %u outside the runtime's vocabulary", pool[i].name_id);
    }
    code->names = pool;
  }
  unsigned doubles = count(r, 8, UINT16_MAX + 1u, "double");
  if (doubles) {
    double *pool = allocate(l, doubles, sizeof(*pool));
    for (unsigned i = 0; pool && i < doubles; i++)
      pool[i] = f64(r);
    code->doubles = pool;
  }
  pools[0] = names;
  pools[1] = doubles;
  unsigned buckets = u32(r);
  if (handlers) {
    if (!buckets || (buckets & (buckets - 1)) || buckets > 65536)
      return fail(r, "handler bucket count %u", buckets);
    uint16_t *order = allocate(l, handlers, sizeof(*order));
    uint16_t *starts = allocate(l, buckets + 1, sizeof(*starts));
    for (unsigned i = 0; order && i < handlers; i++)
      if ((order[i] = u16(r)) >= handlers)
        return fail(r, "handler order outside the table");
    for (unsigned i = 0; starts && i <= buckets; i++)
      if ((starts[i] = u16(r)) > handlers || (i && starts[i] < starts[i - 1]))
        return fail(r, "handler bucket outside the table");
    if (starts && (starts[0] || starts[buckets] != handlers))
      return fail(r, "handler buckets do not cover the table");
    code->handler_order = order;
    code->handler_buckets = starts;
    code->bucket_count = buckets;
  } else if (buckets) {
    return fail(r, "handler buckets without handlers");
  }
  for (unsigned i = 0; !r->failed && i < handlers; i++)
    verify_code(r, &entries[i], code, names, doubles, globals);
  return !r->failed;
}

#if DG_MODERN
static bool load_styles(loader_t *l, dg_text_style_t **out, unsigned *count_out) {
  reader_t *r = l->reader;
  unsigned n = u16(r);
  if (!need(r, (size_t)n * 17))
    return false;
  dg_text_style_t *styles = allocate(l, n, sizeof(*styles));
  for (unsigned i = 0; styles && i < n && !r->failed; i++) {
    dg_text_style_t *s = &styles[i];
    s->font_name = string(l);
    s->font_id = u8(r);
    s->size = u8(r);
    s->align = u8(r);
    s->ascent = (int16_t)u16(r);
    s->descent = (int16_t)u16(r);
    s->leading = (int16_t)u16(r);
    s->line_height = (int16_t)u16(r);
    s->color = u32(r);
#if DG_EXTENDED
    unsigned advances = u16(r);
    if (advances != 0xFFFF) {
      if (!need(r, advances))
        return false;
      uint8_t *a = allocate(l, advances, 1);
      if (a && advances)
        memcpy(a, r->data + r->at, advances);
      r->at += advances;
      s->advances = a;
      s->advance_count = advances;
      s->kerning_count = u32(r);
      unsigned values = count(r, 2, UINT16_MAX * 3u, "kerning value");
      if (!r->failed && values < (size_t)s->kerning_count * 3)
        return fail(r, "kerning table shorter than its pair count");
      int16_t *k = allocate(l, values, sizeof(*k));
      for (unsigned v = 0; k && v < values; v++)
        k[v] = (int16_t)u16(r);
      s->kerning = k;
    }
#endif
  }
  *out = styles;
  *count_out = n;
  return !r->failed;
}
static const dg_text_style_t *style_ref(reader_t *r, const dg_text_style_t *styles,
                                        unsigned count) {
  unsigned index = u16(r);
  if (!index)
    return NULL;
  if (index > count) {
    fail(r, "text style %u outside the table", index);
    return NULL;
  }
  return &styles[index - 1];
}
#endif

#if DG_EXTENDED
// A movie's identical behavior blocks are one block, as the generated C
// writes them: the director tells a continuing span from a new one by the
// block's address (attach_behaviors, d6_queue_cleanup), so a copy per delta
// would end and restart every sprite a frame re-writes.
static const dg_behavior_t *intern_behaviors(loader_t *l, uint32_t *owners, unsigned *owner_count,
                                             const dg_delta_t *deltas, const dg_behavior_t *read,
                                             unsigned count, unsigned index) {
  for (unsigned o = 0; o < *owner_count; o++) {
    const dg_spec_t *s = &deltas[owners[o]].value;
    bool same = s->behavior_count == count;
    for (unsigned k = 0; same && k < count; k++)
      same = s->behaviors[k].script == read[k].script &&
             !strcmp(s->behaviors[k].parameters, read[k].parameters);
    if (same)
      return s->behaviors;
  }
  dg_behavior_t *b = allocate(l, count, sizeof(*b));
  if (!b)
    return NULL;
  memcpy(b, read, count * sizeof(*b));
  owners[(*owner_count)++] = index;
  return b;
}
#endif
static bool load_scene(loader_t *l, dg_movie_t *movie, unsigned movies) {
  reader_t *r = l->reader;
  movie->id = u16(r);
  movie->tempo = u16(r);
#if DG_MODERN
  movie->stage_color = u32(r);
  dg_text_style_t *styles = NULL;
  unsigned style_count = 0;
  if (!load_styles(l, &styles, &style_count))
    return false;
#endif
  unsigned casts = u16(r);
  if (!need(r, (size_t)casts * 8))
    return false;
  dg_cast_t *cast = allocate(l, casts, sizeof(*cast));
  for (unsigned i = 0; cast && i < casts; i++) {
    cast[i].name = string(l);
    cast[i].file = u16(r);
    cast[i].cast = u16(r);
    if (!cast[i].file || cast[i].file > movies)
      return fail(r, "cast file %u outside the package", cast[i].file);
  }
  movie->cast_count = (uint16_t)casts;
  movie->casts = cast;
  unsigned members = count(r, 56, UINT16_MAX, "member");
  dg_member_t *member = allocate(l, members, sizeof(*member));
  for (unsigned i = 0; member && i < members && !r->failed; i++) {
    dg_member_t *m = &member[i];
    m->id = u32(r);
    m->number = u16(r);
    m->cast = u16(r);
    m->type = u16(r);
    m->width = u16(r);
    m->height = u16(r);
    m->reg_x = (int16_t)u16(r);
    m->reg_y = (int16_t)u16(r);
    m->name = string(l);
    m->asset = string(l);
    m->text = string(l);
    m->samples = u32(r);
    m->rate = u32(r);
    m->loop_start = u32(r);
    m->loop_end = u32(r);
    m->shape = u16(r);
    m->pattern = u16(r);
    m->filled = u8(r);
    m->line_width = u8(r);
    m->looping = u8(r);
    m->film_count = u8(r);
    m->film_loop = u8(r);
    m->line_direction = u8(r);
    if (m->film_count)
      m->film_assets = string_list(l, m->film_count);
#if DG_EXTENDED
    m->editable = u8(r) != 0;
    uint32_t text_bytes = u32(r);
    if (text_bytes) {
      // Binary text sits inline, NUL-terminated, in the package's own copy.
      if (!need(r, (size_t)text_bytes + 1) || r->data[r->at + text_bytes])
        return fail(r, "binary member text is not terminated");
      m->text = (const char *)r->data + r->at;
      m->text_bytes = text_bytes;
      r->at += (size_t)text_bytes + 1;
    }
    m->source_bytes = u32(r);
    m->cue_count = u8(r);
    if (m->cue_count) {
      if (!need(r, (size_t)m->cue_count * 8))
        return false;
      dg_cue_t *cues = allocate(l, m->cue_count, sizeof(*cues));
      for (unsigned c = 0; cues && c < m->cue_count; c++) {
        cues[c].milliseconds = u32(r);
        cues[c].name = string(l);
      }
      m->cues = cues;
    }
#endif
#if DG_MODERN
    m->text_style = style_ref(r, styles, style_count);
    m->text_insert_style = style_ref(r, styles, style_count);
#endif
#if DG_D5
    m->video_audio = string(l);
    m->video_flags = u32(r);
    unsigned sounds = count(r, 4, UINT16_MAX, "film sound");
    if (sounds) {
      if (sounds != 2u * m->film_count)
        return fail(r, "film sounds do not pair the film frames");
      uint32_t *pairs = allocate(l, sounds, sizeof(*pairs));
      for (unsigned s = 0; pairs && s < sounds; s++)
        pairs[s] = u32(r);
      m->film_sounds = pairs;
    }
#endif
#if DG_D10
    m->source_xtra = u8(r);
    m->flash_label_count = u8(r);
    m->flash_field_count = u8(r);
    if (m->flash_label_count) {
      if (!need(r, (size_t)m->flash_label_count * 6))
        return false;
      dg_label_t *labels = allocate(l, m->flash_label_count, sizeof(*labels));
      for (unsigned f = 0; labels && f < m->flash_label_count; f++) {
        labels[f].name = string(l);
        labels[f].frame = u16(r);
      }
      m->flash_labels = labels;
    }
    if (m->flash_field_count) {
      if (!need(r, (size_t)m->flash_field_count * 32))
        return false;
      dg_flash_field_t *fields = allocate(l, m->flash_field_count, sizeof(*fields));
      for (unsigned f = 0; fields && f < m->flash_field_count; f++) {
        dg_flash_field_t *x = &fields[f];
        x->name = string(l);
        x->variable = string(l);
        x->text = string(l);
        x->x = (int16_t)u16(r);
        x->y = (int16_t)u16(r);
        x->width = (int16_t)u16(r);
        x->height = (int16_t)u16(r);
        x->margin_left = (int16_t)u16(r);
        x->margin_right = (int16_t)u16(r);
        x->indent = (int16_t)u16(r);
        x->align = u8(r);
        x->word_wrap = u8(r);
        x->multiline = u8(r);
        x->leading = (int8_t)u8(r);
        x->style = style_ref(r, styles, style_count);
      }
      m->flash_fields = fields;
    }
#endif
  }
  movie->member_count = (uint16_t)members;
  movie->members = member;
  if (!need(r, (size_t)members * 6))
    return false;
  dg_member_index_t *index = allocate(l, members, sizeof(*index));
  for (unsigned i = 0; index && i < members; i++) {
    index[i].hash = u32(r);
    index[i].member = u16(r);
    if (index[i].member >= members || (i && index[i].hash < index[i - 1].hash))
      return fail(r, "member index out of order or outside the table");
  }
  movie->member_index = index;
  if (!need(r, 256 * 4))
    return false;
  uint32_t *palette = allocate(l, 256, sizeof(*palette));
  for (unsigned i = 0; palette && i < 256; i++)
    palette[i] = u32(r);
  movie->palette = palette;
  unsigned frames = count(r, 6, UINT16_MAX, "frame");
  dg_frame_t *frame = allocate(l, frames, sizeof(*frame));
  for (unsigned i = 0; frame && i < frames; i++) {
    frame[i].first = u32(r);
    frame[i].count = u16(r);
  }
  unsigned deltas = count(r, 29, UINT32_MAX, "delta");
  for (unsigned i = 0; frame && i < frames; i++)
    if (frame[i].first > deltas || frame[i].count > deltas - frame[i].first)
      return fail(r, "frame %u outside the score", i + 1);
  dg_delta_t *delta = allocate(l, deltas, sizeof(*delta));
#if DG_EXTENDED
  // The deltas whose behavior block is the first of its content.
  uint32_t *owners = allocate(l, deltas, sizeof(*owners));
  unsigned owner_count = 0;
  if (!owners)
    return false;
#endif
  for (unsigned i = 0; delta && i < deltas && !r->failed; i++) {
    dg_delta_t *d = &delta[i];
    d->channel = u16(r);
    d->mask = u16(r);
    if (d->channel >= DG_SCORE_CHANNELS)
      return fail(r, "score channel %u exceeds the profile", d->channel);
    dg_spec_t *s = &d->value;
    s->member = u32(r);
    s->script = u32(r);
    s->x = (int16_t)u16(r);
    s->y = (int16_t)u16(r);
    s->width = (int16_t)u16(r);
    s->height = (int16_t)u16(r);
    s->ink = u8(r);
    s->blend = u8(r);
    s->type = u8(r);
    s->flags = u8(r);
    s->fore = u8(r);
    s->back = u8(r);
    s->thickness = u8(r);
    s->stretch = u8(r);
    s->trails = u8(r);
#if DG_MODERN
    s->loc_z = (int16_t)u16(r);
    s->tempo = u16(r);
    s->delay = u16(r);
    s->fore_rgb = u32(r);
    s->back_rgb = u32(r);
    s->rotation = (int32_t)u32(r);
    s->skew = (int32_t)u32(r);
#endif
#if DG_EXTENDED
    s->behavior_count = u8(r);
    if (s->behavior_count) {
      if (!need(r, (size_t)s->behavior_count * 8))
        return false;
      dg_behavior_t read[UINT8_MAX];
      for (unsigned k = 0; k < s->behavior_count; k++) {
        read[k].script = u32(r);
        read[k].parameters = string(l);
      }
      s->behaviors = intern_behaviors(l, owners, &owner_count, delta, read, s->behavior_count, i);
    }
#endif
  }
  movie->frame_count = (uint16_t)frames;
  movie->frames = frame;
  movie->deltas = delta;
  unsigned labels = u16(r);
  if (!need(r, (size_t)labels * 6))
    return false;
  dg_label_t *label = allocate(l, labels, sizeof(*label));
  for (unsigned i = 0; label && i < labels; i++) {
    label[i].name = string(l);
    label[i].frame = u16(r);
  }
  movie->label_count = (uint16_t)labels;
  movie->labels = label;
  return !r->failed;
}

void dg_package_free(dg_package_t *p) {
  for (unsigned i = 0; i < p->block_count; i++)
    free(p->blocks[i]);
  free(p->blocks);
  free(p->bytes);
  memset(p, 0, sizeof(*p));
}

typedef struct {
  uint32_t offset, length;
  bool present;
} section_t;
enum { META, STRS, CODE, SYMB, GLOB, MOVI, SECTIONS };
static const char tags[SECTIONS][5] = {"META", "STRS", "CODE", "SYMB", "GLOB", "MOVI"};

bool dg_package_load(dg_package_t *p, const uint8_t *data, size_t length,
                     const uint8_t abi[DG_PACKAGE_ABI_BYTES], char *error,
                     size_t error_size) {
  memset(p, 0, sizeof(*p));
  if (error_size)
    error[0] = 0;
  reader_t header = {data, length, 0, false, error, error_size};
  if (length < 32 || memcmp(data, "D64P", 4))
    return fail(&header, "not a Director64 package");
  header.at = 4;
  unsigned format = u16(&header), version = u16(&header), flags = u16(&header);
  unsigned sections = u16(&header);
  if (format != DG_PACKAGE_FORMAT)
    return fail(&header, "package format %u; this runtime reads %u", format,
                DG_PACKAGE_FORMAT);
  if (version != DIRECTOR64_DIRECTOR_VERSION * 100 || flags != DIRECTOR64_EXTENDED_D6)
    return fail(&header, "package profile D%u%s does not match this runtime",
                version / 100, flags ? " extended" : "");
  if (memcmp(data + 12, abi, DG_PACKAGE_ABI_BYTES))
    return fail(&header, "package compiled for a different runtime ABI");
  header.at = 32;
  if (!need(&header, (size_t)sections * 12))
    return false;
  section_t table[SECTIONS] = {{0}};
  for (unsigned i = 0; i < sections; i++) {
    char tag[5] = {0};
    memcpy(tag, data + header.at, 4);
    header.at += 4;
    uint32_t offset = u32(&header), size = u32(&header);
    if (offset < 32 + (size_t)sections * 12 || offset > length ||
        size > length - offset)
      return fail(&header, "section %s outside the package", tag);
    for (unsigned k = 0; k < SECTIONS; k++)
      if (!memcmp(tag, tags[k], 4)) {
        if (table[k].present)
          return fail(&header, "section %s repeated", tag);
        table[k] = (section_t){offset, size, true};
      }
  }
  for (unsigned k = 0; k < SECTIONS; k++)
    if (!table[k].present)
      return fail(&header, "section %s missing", tags[k]);
  p->bytes = malloc(length);
  if (!p->bytes)
    return fail(&header, "out of memory");
  memcpy(p->bytes, data, length);
  p->length = length;
  const uint8_t *bytes = p->bytes;
  if (!table[STRS].length || bytes[table[STRS].offset + table[STRS].length - 1]) {
    dg_package_free(p);
    return fail(&header, "string pool is not terminated");
  }
  reader_t body = {0};
  loader_t l = {p, &body, (const char *)bytes + table[STRS].offset,
                table[STRS].length, bytes + table[CODE].offset,
                table[CODE].length};
  body = (reader_t){bytes + table[SYMB].offset, table[SYMB].length, 0, false,
                    error, error_size};
  unsigned symbols = count(&body, 4, UINT16_MAX, "symbol");
  unsigned buckets = u32(&body);
  if (!body.failed && (!buckets || (buckets & (buckets - 1)) || buckets > 65536))
    fail(&body, "symbol bucket count %u", buckets);
  p->symbols.text = string_list(&l, symbols);
  uint16_t *starts = allocate(&l, buckets + 1, sizeof(*starts));
  for (unsigned i = 0; starts && i <= buckets; i++) {
    uint32_t start = u32(&body);
    if (start > symbols || (i && start < starts[i - 1]))
      fail(&body, "symbol bucket outside the table");
    starts[i] = (uint16_t)start;
  }
  if (!body.failed && (starts[0] || starts[buckets] != symbols))
    fail(&body, "symbol buckets do not cover the table");
  p->symbols = (lb_symbol_table_t){p->symbols.text, symbols, starts, buckets};
  if (!body.failed) {
    body = (reader_t){bytes + table[GLOB].offset, table[GLOB].length, 0, false,
                      error, error_size};
    p->global_count = count(&body, 4, LV_GLOBALS, "global");
    p->globals = (const char **)string_list(&l, p->global_count);
  }
  if (!body.failed) {
    body = (reader_t){bytes + table[MOVI].offset, table[MOVI].length, 0, false,
                      error, error_size};
    // A member reference keeps the file id in its top twelve bits.
    p->movie_count = count(&body, 16, 4095, "movie");
    p->registry = allocate(&l, p->movie_count, sizeof(*p->registry));
    p->pool_counts = allocate(&l, 2 * p->movie_count, sizeof(*p->pool_counts));
    for (unsigned i = 0; p->registry && p->pool_counts && i < p->movie_count && !body.failed; i++) {
      lv_movie_t *code = allocate(&l, 1, sizeof(*code));
      dg_movie_t *movie = allocate(&l, 1, sizeof(*movie));
      if (!code || !movie)
        break;
      code->name = string(&l);
      if (load_handlers(&l, code, p->global_count, &p->pool_counts[2 * i]) && load_scene(&l, movie, p->movie_count)) {
        if (movie->id != i + 1)
          fail(&body, "movie %s has id %u at position %u", code->name,
               movie->id, i + 1);
        movie->code = code;
        p->registry[i] = movie;
      }
    }
    if (!body.failed && body.at != body.length)
      fail(&body, "trailing bytes after the last movie");
  }
  if (body.failed) {
    dg_package_free(p);
    return false;
  }
  // Every member reference the score and cast tables make must resolve to
  // a movie of this package; the runtime indexes by file id.
  for (unsigned i = 0; i < p->movie_count; i++) {
    const dg_movie_t *m = p->registry[i];
    for (unsigned k = 0; k < m->member_count; k++)
      if ((m->members[k].id >> 20) != m->id && (m->members[k].id >> 20) > p->movie_count) {
        dg_package_free(p);
        return fail(&header, "member reference outside the package");
      }
  }
  return true;
}
