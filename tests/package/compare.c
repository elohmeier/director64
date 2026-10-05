// Loader parity for one game: loads its package and compares every field of
// every table with the generated C the console links. A local-media gate
// (the generated tables come from the recovered game); `director64 web`
// builds and runs it.
#include "package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const dg_movie_t *const dg_registry[];
extern const unsigned dg_registry_count;
static const char *const global_names[] = {
#include "globals.inc"
};
static unsigned differences;
static char where[256];

#define SAME(expr) do { if (!(expr)) { differences++; \
  if (differences < 20) fprintf(stderr, "%s: %s\n", where, #expr); } } while (0)
static bool text_equal(const char *a, const char *b) {
  return (!a && !b) || (a && b && !strcmp(a, b));
}
static bool names_equal(const char *const *a, const char *const *b, unsigned n) {
  // Generated C gives an empty list as {NULL}; so does the loader.
  for (unsigned i = 0; i < n; i++)
    if (!text_equal(a[i], b[i]))
      return false;
  return true;
}

#if DG_MODERN
static bool style_equal(const dg_text_style_t *a, const dg_text_style_t *b) {
  if (!a || !b)
    return !a && !b;
  bool same = text_equal(a->font_name, b->font_name) && a->font_id == b->font_id &&
              a->size == b->size && a->align == b->align && a->ascent == b->ascent &&
              a->descent == b->descent && a->leading == b->leading &&
              a->line_height == b->line_height && a->color == b->color;
#if DG_EXTENDED
  same = same && !a->advances == !b->advances && a->advance_count == b->advance_count &&
         a->kerning_count == b->kerning_count;
  if (same && a->advances)
    same = !memcmp(a->advances, b->advances, a->advance_count) &&
           !memcmp(a->kerning, b->kerning, a->kerning_count * 3 * sizeof(int16_t));
#endif
  return same;
}
#endif

static void compare_code(const lv_movie_t *a, const lv_movie_t *b, const unsigned pools[2]) {
  SAME(text_equal(a->name, b->name));
  SAME(a->count == b->count);
  for (unsigned i = 0; i < a->count && i < b->count; i++) {
    const lv_handler_t *x = &a->handlers[i], *y = &b->handlers[i];
    snprintf(where, sizeof(where), "%s handler %u %s", a->name, i, x->name);
    SAME(text_equal(x->name, y->name) && x->member == y->member);
    SAME(text_equal(x->cast, y->cast) && text_equal(x->kind, y->kind));
    SAME(x->arguments == y->arguments && x->locals == y->locals && x->entry == y->entry);
    SAME(x->step == y->step && x->property_count == y->property_count);
    SAME(names_equal(x->local_names, y->local_names, x->locals));
    SAME(names_equal(x->property_names, y->property_names, x->property_count));
    SAME(x->code_size == y->code_size && !memcmp(x->code, y->code, x->code_size));
  }
  snprintf(where, sizeof(where), "%s pools", a->name);
  SAME(!a->names == !b->names && !a->doubles == !b->doubles);
  // The generated arrays carry no length; the package's counts bound both.
  for (unsigned i = 0; a->names && b->names && i < pools[0]; i++) {
    const lv_name_t *x = &a->names[i], *y = &b->names[i];
    snprintf(where, sizeof(where), "%s name %u %s", a->name, i, x->text);
    SAME(text_equal(x->text, y->text) && x->name_id == y->name_id && x->symbol_id == y->symbol_id);
  }
  for (unsigned i = 0; a->doubles && b->doubles && i < pools[1]; i++) {
    snprintf(where, sizeof(where), "%s double %u", a->name, i);
    SAME(!memcmp(&a->doubles[i], &b->doubles[i], sizeof(double)));
  }
  SAME(!a->handler_order == !b->handler_order && a->bucket_count == b->bucket_count);
  if (a->handler_order && b->handler_order) {
    SAME(!memcmp(a->handler_order, b->handler_order, a->count * sizeof(uint16_t)));
    SAME(!memcmp(a->handler_buckets, b->handler_buckets,
                 (a->bucket_count + 1) * sizeof(uint16_t)));
  }
}

static void compare_scene(const dg_movie_t *a, const dg_movie_t *b) {
  snprintf(where, sizeof(where), "%s scene", a->code->name);
  SAME(a->id == b->id && a->tempo == b->tempo && a->cast_count == b->cast_count);
  SAME(a->member_count == b->member_count && a->frame_count == b->frame_count);
  SAME(a->label_count == b->label_count);
  SAME(!memcmp(a->palette, b->palette, 256 * sizeof(uint32_t)));
#if DG_MODERN
  SAME(a->stage_color == b->stage_color);
#endif
  for (unsigned i = 0; i < a->cast_count && i < b->cast_count; i++)
    SAME(text_equal(a->casts[i].name, b->casts[i].name) &&
         a->casts[i].file == b->casts[i].file && a->casts[i].cast == b->casts[i].cast);
  for (unsigned i = 0; i < a->member_count && i < b->member_count; i++) {
    const dg_member_t *x = &a->members[i], *y = &b->members[i];
    snprintf(where, sizeof(where), "%s member %u", a->code->name, i);
    SAME(x->id == y->id && x->number == y->number && x->cast == y->cast && x->type == y->type);
    SAME(x->width == y->width && x->height == y->height && x->reg_x == y->reg_x &&
         x->reg_y == y->reg_y);
    SAME(text_equal(x->name, y->name) && text_equal(x->asset, y->asset) &&
         text_equal(x->text, y->text));
    SAME(x->samples == y->samples && x->rate == y->rate && x->loop_start == y->loop_start &&
         x->loop_end == y->loop_end);
    SAME(x->shape == y->shape && x->pattern == y->pattern && x->filled == y->filled &&
         x->line_width == y->line_width && x->looping == y->looping);
    SAME(x->film_count == y->film_count && x->film_loop == y->film_loop &&
         x->line_direction == y->line_direction);
    if (x->film_count == y->film_count && x->film_count)
      SAME(names_equal(x->film_assets, y->film_assets, x->film_count));
#if DG_EXTENDED
    SAME(x->editable == y->editable && x->text_bytes == y->text_bytes &&
         x->source_bytes == y->source_bytes && x->cue_count == y->cue_count);
    if (x->text_bytes && x->text_bytes == y->text_bytes)
      SAME(!memcmp(x->text, y->text, x->text_bytes + 1));
    for (unsigned c = 0; c < x->cue_count && x->cue_count == y->cue_count; c++)
      SAME(x->cues[c].milliseconds == y->cues[c].milliseconds &&
           text_equal(x->cues[c].name, y->cues[c].name));
#endif
#if DG_MODERN
    SAME(style_equal(x->text_style, y->text_style));
    SAME(style_equal(x->text_insert_style, y->text_insert_style));
#endif
#if DG_D5
    SAME(text_equal(x->video_audio, y->video_audio) && x->video_flags == y->video_flags);
    SAME(!x->film_sounds == !y->film_sounds);
    if (x->film_sounds && y->film_sounds && x->film_count == y->film_count)
      SAME(!memcmp(x->film_sounds, y->film_sounds, 2u * x->film_count * sizeof(uint32_t)));
#endif
#if DG_D10
    SAME(x->source_xtra == y->source_xtra && x->flash_label_count == y->flash_label_count &&
         x->flash_field_count == y->flash_field_count);
    for (unsigned f = 0; f < x->flash_label_count && x->flash_label_count == y->flash_label_count; f++)
      SAME(text_equal(x->flash_labels[f].name, y->flash_labels[f].name) &&
           x->flash_labels[f].frame == y->flash_labels[f].frame);
    for (unsigned f = 0; f < x->flash_field_count && x->flash_field_count == y->flash_field_count; f++) {
      const dg_flash_field_t *p = &x->flash_fields[f], *q = &y->flash_fields[f];
      SAME(text_equal(p->name, q->name) && text_equal(p->variable, q->variable) &&
           text_equal(p->text, q->text));
      SAME(p->x == q->x && p->y == q->y && p->width == q->width && p->height == q->height);
      SAME(p->margin_left == q->margin_left && p->margin_right == q->margin_right &&
           p->indent == q->indent && p->align == q->align && p->word_wrap == q->word_wrap &&
           p->multiline == q->multiline && p->leading == q->leading);
      SAME(style_equal(p->style, q->style));
    }
#endif
    SAME(a->member_index[i].hash == b->member_index[i].hash &&
         a->member_index[i].member == b->member_index[i].member);
  }
  snprintf(where, sizeof(where), "%s score", a->code->name);
  unsigned deltas = 0;
  for (unsigned i = 0; i < a->frame_count && i < b->frame_count; i++) {
    SAME(a->frames[i].first == b->frames[i].first && a->frames[i].count == b->frames[i].count);
    if (a->frames[i].first + a->frames[i].count > deltas)
      deltas = a->frames[i].first + a->frames[i].count;
  }
  for (unsigned i = 0; i < deltas; i++) {
    const dg_delta_t *x = &a->deltas[i], *y = &b->deltas[i];
    snprintf(where, sizeof(where), "%s delta %u", a->code->name, i);
    SAME(x->channel == y->channel && x->mask == y->mask);
    const dg_spec_t *s = &x->value, *t = &y->value;
    SAME(s->member == t->member && s->script == t->script && s->x == t->x && s->y == t->y);
    SAME(s->width == t->width && s->height == t->height && s->ink == t->ink &&
         s->blend == t->blend && s->type == t->type && s->flags == t->flags);
    SAME(s->fore == t->fore && s->back == t->back && s->thickness == t->thickness &&
         s->stretch == t->stretch && s->trails == t->trails);
#if DG_MODERN
    SAME(s->loc_z == t->loc_z && s->tempo == t->tempo && s->delay == t->delay);
    SAME(s->fore_rgb == t->fore_rgb && s->back_rgb == t->back_rgb &&
         s->rotation == t->rotation && s->skew == t->skew);
#endif
#if DG_EXTENDED
    SAME(s->behavior_count == t->behavior_count);
    for (unsigned k = 0; k < s->behavior_count && s->behavior_count == t->behavior_count; k++)
      SAME(s->behaviors[k].script == t->behaviors[k].script &&
           text_equal(s->behaviors[k].parameters, t->behaviors[k].parameters));
#endif
  }
#if DG_EXTENDED
  // Block identity is span identity to the director, so the deltas sharing a
  // behavior block must be the same deltas on both sides.
  for (unsigned i = 0; i < deltas; i++) {
    const dg_behavior_t *x = a->deltas[i].value.behaviors, *y = b->deltas[i].value.behaviors;
    if (!x || !y)
      continue;
    unsigned first_a = i, first_b = i;
    for (unsigned j = 0; j < i && (first_a == i || first_b == i); j++) {
      if (first_a == i && a->deltas[j].value.behaviors == x) first_a = j;
      if (first_b == i && b->deltas[j].value.behaviors == y) first_b = j;
    }
    snprintf(where, sizeof(where), "%s delta %u behaviors", a->code->name, i);
    SAME(first_a == first_b);
  }
#endif
  for (unsigned i = 0; i < a->label_count && i < b->label_count; i++)
    SAME(text_equal(a->labels[i].name, b->labels[i].name) &&
         a->labels[i].frame == b->labels[i].frame);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: compare PACKAGE ABI-HEX\n");
    return 2;
  }
  FILE *file = fopen(argv[1], "rb");
  if (!file)
    return 2;
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  fseek(file, 0, SEEK_SET);
  uint8_t *data = malloc((size_t)length);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length)
    return 2;
  fclose(file);
  uint8_t abi[DG_PACKAGE_ABI_BYTES];
  for (unsigned i = 0; i < DG_PACKAGE_ABI_BYTES; i++)
    sscanf(argv[2] + 2 * i, "%2hhx", &abi[i]);
  dg_package_t package;
  char error[256];
  if (!dg_package_load(&package, data, (size_t)length, abi, error, sizeof(error))) {
    fprintf(stderr, "package rejected: %s\n", error);
    return 1;
  }
  free(data); // the package keeps its own copy
  snprintf(where, sizeof(where), "registry");
  SAME(package.movie_count == dg_registry_count);
  for (unsigned i = 0; i < dg_registry_count && i < package.movie_count; i++) {
    compare_code(dg_registry[i]->code, package.registry[i]->code, &package.pool_counts[2 * i]);
    compare_scene(dg_registry[i], package.registry[i]);
  }
  snprintf(where, sizeof(where), "globals");
  unsigned globals = sizeof(global_names) / sizeof(*global_names);
  SAME(package.global_count == globals);
  for (unsigned i = 0; i < globals && i < package.global_count; i++)
    SAME(text_equal(global_names[i], package.globals[i]));
  snprintf(where, sizeof(where), "symbols");
  SAME(package.symbols.count == lb_symbols.count &&
       package.symbols.bucket_count == lb_symbols.bucket_count);
  for (unsigned i = 0; i < lb_symbols.count && i < package.symbols.count; i++)
    SAME(text_equal(lb_symbols.text[i], package.symbols.text[i]));
  for (unsigned i = 0; i <= lb_symbols.bucket_count && package.symbols.count == lb_symbols.count; i++)
    SAME(lb_symbols.buckets[i] == package.symbols.buckets[i]);
  dg_package_free(&package);
  printf("{\"movies\":%u,\"differences\":%u}\n", dg_registry_count, differences);
  return differences ? 1 : 0;
}
