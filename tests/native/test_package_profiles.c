// Load actual Rust output, including nonempty data on both sides of every
// profile-dependent member tail. A separately handwritten binary cannot
// catch a writer/loader family mismatch.
#include "package.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *file = fopen(argv[1], "rb");
  assert(file && !fseek(file, 0, SEEK_END));
  long length = ftell(file);
  assert(length > 32 && !fseek(file, 0, SEEK_SET));
  uint8_t *bytes = malloc((size_t)length);
  assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
  fclose(file);
  dg_package_t package = {0};
  char error[256];
  if (!dg_package_load(&package, bytes, (size_t)length, bytes + 12, error, sizeof(error))) {
    fprintf(stderr, "profile %u extended %u: %s\n", DIRECTOR64_DIRECTOR_VERSION,
            DG_EXTENDED, error);
    free(bytes);
    return 1;
  }
  assert(package.movie_count == 1);
  const dg_movie_t *movie = package.registry[0];
  assert(movie->id == 1 && movie->tempo == 27 && movie->member_count == 5);
  assert(movie->cast_count == 1 && !strcmp(movie->casts[0].name, "Internal"));
  const dg_member_t *film = &movie->members[0], *text = &movie->members[1];
  assert(film->type == 2 && film->film_count == 2 && film->film_loop == 1);
  assert(film->line_direction == 6 && film->width == 17 && film->height == 19);
  assert(!strcmp(film->film_assets[0], "first.fdi"));
  assert(!strcmp(film->film_assets[1], "second.fdi"));
#if DG_D10
  assert(film->flash_variant_count == 1 && film->source_xtra == 0);
  assert(!film->flash_clip_count && !film->film_track_count);
#endif
#if DG_D5 || DG_D10
  assert(film->film_sounds && film->film_sounds[0] == DG_ID(1, 1, 4));
  assert(!film->film_sounds[1] && !film->film_sounds[2]);
  assert(film->film_sounds[3] == DG_ID(1, 1, 4));
#endif
  assert(text->type == 3 && !strcmp(text->name, "Text"));
#if DG_EXTENDED
  assert(text->editable && text->text_bytes == 3 && !memcmp(text->text, "A\0B", 4));
  assert(text->source_bytes == 3079 && text->cue_count == 1);
  assert(text->cues[0].milliseconds == 1234 && !strcmp(text->cues[0].name, "Tail cue"));
#else
  assert(!strcmp(text->text, "Text tail"));
#endif
#if DG_MODERN
  assert(text->text_style && !strcmp(text->text_style->font_name, "Fixture"));
  assert(text->text_style->size == 13 && text->text_style->color == 0x123456);
  assert(text->text_insert_style && text->text_insert_style->size == 14);
#endif
  const dg_member_t *palette = &movie->members[2], *sound = &movie->members[3];
  assert(palette->type == 4 && palette->palette_count == 16);
  for (unsigned i = 0; i < 16; i++) assert(palette->palette[i] == 0x123400 + i);
  assert(sound->type == 6 && !strcmp(sound->asset, "sound.wav64"));
  assert(sound->samples == 12345 && sound->rate == 22050);
  assert(sound->looping && sound->loop_start == 123 && sound->loop_end == 456);
  const dg_member_t *last = &movie->members[4];
  assert(last->id == DG_ID(1, 1, 5) && !strcmp(last->name, "Last member"));
  assert(!strcmp(last->asset, "last.fdi") && last->width == 23 && last->height == 31);
  assert(last->reg_x == -7 && last->reg_y == 11);
  assert(movie->palette[17] == 0x111111 && movie->palette[255] == 0xffffff);
  assert(movie->frame_count == 1 && movie->label_count == 1);
  assert(!strcmp(movie->labels[0].name, "Tail") && movie->labels[0].frame == 1);
  dg_package_free(&package);
  free(bytes);
  puts("Rust package member layout matches C runtime profile");
  return 0;
}
