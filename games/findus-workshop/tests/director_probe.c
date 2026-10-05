// Local-disc integration probe. Generated scripts/data are never committed.
#include "archive.h"
#include "image.h"
#include "virtual_files.h"
#include "director.h"
#include "pointer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const dg_movie_t *const dg_registry[];
extern const unsigned dg_registry_count;
static const char *const global_names[] = {
#include "globals.inc"
};
static lv_runtime_t values;
static dg_runtime_t director;
static pointer_control_t pointer;
static unsigned busy_until[2];
static unsigned sound_count;
static char saves[9][16384];
static unsigned save_lengths[9];
static archive_t archive;
static unsigned char flash[2 * ARCHIVE_BYTES];
static bool flash_read(void *ctx, size_t offset, void *data, size_t length) {
  (void)ctx;
  if (offset + length > sizeof(flash)) return false;
  memcpy(data, flash + offset, length);
  return true;
}
static bool flash_write(void *ctx, size_t offset, const void *data, size_t length) {
  (void)ctx;
  if (offset + length > sizeof(flash)) return false;
  memcpy(flash + offset, data, length);
  return true;
}
static bool read_file(void *ctx, const char *name, char *data, unsigned cap,
                      unsigned *length) {
  (void)ctx;
  return archive_read(&archive, name, data, cap, length);
}
static bool write_file(void *ctx, const char *name, const char *data,
                       unsigned length) {
  (void)ctx;
  if (!archive_write(&archive, name, data, length)) return false;
  int id = archive_file_id(name);
  memcpy(saves[id], data, length);
  saves[id][length] = 0;
  save_lengths[id] = length;
  return true;
}
static void sound(void *ctx, unsigned channel, const dg_member_t *m) {
  (void)ctx;
  busy_until[channel] =
      director.ticks +
      (m && m->rate ? (m->samples * 60u + m->rate - 1) / m->rate : 0);
  if (m && m->looping)
    busy_until[channel] = UINT32_MAX;
  sound_count++;
}
static bool busy(void *ctx, unsigned channel) {
  (void)ctx;
  return director.ticks < busy_until[channel];
}
static void trace(void *ctx, const char *text) {
  (void)ctx;
  (void)text;
}
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx;
  return native_image_hit(&values, m, ink, x, y);
}
static const dg_movie_t *find(const char *name) {
  char normalized[64];
  snprintf(normalized, sizeof(normalized), "%s", name);
  for (char *p = normalized; *p; p++)
    if (*p >= 'a' && *p <= 'z')
      *p -= 32;
  if (!strchr(normalized, '.'))
    strcat(normalized, ".DXR");
  for (unsigned i = 0; i < dg_registry_count; i++)
    if (!strcmp(dg_registry[i]->code->name, normalized))
      return dg_registry[i];
  return NULL;
}
static bool enter(const dg_movie_t *movie, unsigned frame) {
  const dg_movie_t *shared[3];
  unsigned count = 0;
  if (!movie) {
    lv_fail(&values, "probe missing movie");
    return false;
  }
  for (unsigned i = 0; i < movie->cast_count; i++)
    if (movie->casts[i].file != movie->id) {
      for (unsigned j = 0; j < dg_registry_count; j++)
        if (dg_registry[j]->id == movie->casts[i].file) {
          bool added = false;
          for (unsigned k = 0; k < count; k++)
            if (shared[k] == dg_registry[j])
              added = true;
          if (!added)
            shared[count++] = dg_registry[j];
        }
    }
  printf("ENTER %s frame=%u tick=%u\n", movie->code->name, frame,
         director.ticks);
  return dg_enter(&director, movie, frame, shared, count);
}
static void global(const char *name, lv_t value) {
  int id = lv_global_id(&values, name);
  if (id >= 0)
    values.globals[id] = value;
}
static void boot(bool activity_fixture) {
  memset(busy_until, 0, sizeof(busy_until));
  dg_init(&director, &values,
          (dg_platform_t){.nth_file = workshop_nth_file, .read_file = read_file, .write_file = write_file,
                          .sound = sound, .sound_busy = busy, .trace = trace, .hit = hit},
          NULL, global_names, sizeof(global_names) / sizeof(*global_names), 42);
  archive_load(&archive, (save_backend_t){flash_read, flash_write, NULL});
  unsigned offset = 0;
  for (unsigned i = 0; i < ARCHIVE_FILES; i++) {
    save_lengths[i] = archive.lengths[i];
    memcpy(saves[i], archive.data + offset, save_lengths[i]);
    saves[i][save_lengths[i]] = 0;
    offset += save_lengths[i];
  }
  pointer_init(&pointer);
  // START and reboot journeys must initialize their own source globals, just
  // like the playable ROM. Only direct-activity fixtures bypass the intro.
  if (!activity_fixture)
    return;
  global("gpathdatafiles", lv_text(&values, "C:\\data\\", false));
  global("gvaktfilename", lv_text(&values, "vakt1.txt", false));
  global("lskattmatris", lv_literal(&values,
      "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]"));
  global("lskattkartamatris", lv_literal(&values, "[0,0,0,0,0,0,0,0,0]"));
}
#include "draw_record.inc"
static bool step(int x, int y, bool down) {
  if (director.next_movie[0] && !values.depth) {
    const dg_movie_t *next = find(director.next_movie);
    unsigned frame = director.next_movie_frame;
    if (!enter(next, frame ? frame : 1))
      return false;
  }
  bool ok = dg_tick(&director, x, y, down, DG_SERVICE_BUDGET);
  dg_update_stage(&director);draw_record_tick(&director);
  return ok && !values.failed;
}
static void quoted(const char *text) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (*p < 32 || *p == '"' || *p == '\\')
      printf("\\u%04x", *p);
    else
      putchar(*p);
  }
  putchar('"');
}
static void state(void) {
  printf("{\"movie\":");
  quoted(director.movie->code->name);
  printf(",\"frame\":%u,\"tick\":%u,\"depth\":%u,\"quit\":%s,\"sounds\":%u,\"error\":", director.frame,
         director.ticks, values.depth, director.quit ? "true" : "false", sound_count);
  quoted(values.failed ? values.error : "");
  printf(",\"script_errors\":%u,\"last_script_error\":", values.script_error_count);
  quoted(values.last_script_error);
  printf(",\"handler\":");
  quoted(values.depth ? values.frames[values.depth - 1].handler->name : "");
  printf(",\"line\":%u,\"globals\":{",
         values.depth ? values.frames[values.depth - 1].line : 0);
  bool comma = false;
  for (unsigned i = 0; i < values.global_count; i++)
    if (lv_type(values.globals[i])) {
      char text[16384];
      if (!lv_format(&values, values.globals[i], text, sizeof(text)))
        break;
      if (comma)
        putchar(',');
      comma = true;
      quoted(global_names[i]);
      putchar(':');
      quoted(text);
    }
  printf("},\"sprites\":[");
  comma = false;
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    dg_sprite_t *s = &director.sprites[i];
    if (!s->value.type || !s->visible)
      continue;
    int l, t, r, b;
    dg_bounds(&director, i, &l, &t, &r, &b);
    if (comma)
      putchar(',');
    comma = true;
    printf("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],"
           "\"blend\":%u,\"loc\":[%d,%d],\"moveable\":%u}",
           i, s->value.member, s->value.script, l, t, r, b, dg_opacity(s),
           s->value.x, s->value.y, s->moveable);
  }
  const pointer_player_t *driver = pointer_driver(&pointer);
  printf("],\"pointer\":{\"player\":%u,\"x\":%d,\"y\":%d},\"save_lengths\":[",
         pointer.active + 1, driver->input.x / INPUT_ONE,
         driver->input.y / INPUT_ONE);
  for (unsigned i = 0; i < 9; i++)
    printf("%s%u", i ? "," : "", save_lengths[i]);
  printf("],\"save_contents\":[");
  for (unsigned i = 0; i < 9; i++) {
    if (i)
      putchar(',');
    quoted(saves[i]);
  }
  printf("],\"save_generation\":%u}\n", archive.generation);
  fflush(stdout);
}
static int rpc(void) {
  char command[128];
  state();
  while (fgets(command, sizeof(command), stdin)) {
    if (!strcmp(command, "reboot\n")) {
      // A console reset: reset volatile state, retaining ONLY persisted files.
      boot(false);
      if (!enter(find("START"), 1))
        return 1;
      state();
      continue;
    }
    unsigned ticks;
    int x, y, down;
    unsigned sprite;
    unsigned buttons;
    if (sscanf(command, "pad %u %d %d %u", &ticks, &x, &y, &buttons) == 4 &&
        ticks <= 100000 && x >= -128 && x <= 127 && y >= -128 && y <= 127 &&
        buttons < 128) {
      input_sample_t sample = {.connected = true, .stick_x = x, .stick_y = y,
                               .buttons = buttons};
      for (unsigned i = 0; i < ticks && !values.failed && !director.quit; i++) {
        pointer_update(&pointer, &director, &sample);
        const input_state_t *in = &pointer_driver(&pointer)->input;
        step(in->x / INPUT_ONE, in->y / INPUT_ONE, !!(in->held & INPUT_A));
      }
      state();
      continue;
    }
    if (sscanf(command, "point %u", &sprite) == 1 && sprite > 0 &&
        sprite < DG_SPRITES) {
      int l, t, r, b;
      dg_bounds(&director, sprite, &l, &t, &r, &b);
      bool found = false;
      int cx = (l + r) / 2, cy = (t + b) / 2;
      if (cx >= 8 && cx < 630 && cy >= 8 && cy < 470 &&
          dg_mouse_hit(&director, cx, cy) == sprite) {
        printf("{\"point\":[%d,%d],\"error\":\"\"}\n", cx, cy);
        found = true;
      }
      for (int spacing = 8; spacing >= 1 && !found; spacing /= 8) {
        for (int yy = t; yy < b && !found; yy += spacing)
          for (int xx = l; xx < r && !found; xx += spacing)
            if (xx >= 8 && xx < 630 && yy >= 8 && yy < 470 &&
                dg_mouse_hit(&director, xx, yy) == sprite) {
              printf("{\"point\":[%d,%d],\"error\":\"\"}\n", xx, yy);
              found = true;
            }
      }
      if (!found)
        puts("{\"point\":null,\"error\":\"\"}");
      fflush(stdout);
      continue;
    }
    if (sscanf(command, "step %u %d %d %d", &ticks, &x, &y, &down) == 4 &&
        ticks <= 100000) {
      for (unsigned i = 0; i < ticks && !values.failed && !director.quit; i++)
        if (!step(x, y, down != 0))
          break;
    } else if (strncmp(command, "state", 5))
      return 2;
    state();
  }
  return values.failed ? 1 : 0;
}
int main(int argc, char **argv) {
  memset(flash, 255, sizeof(flash));
  const char *name = argc > 1 ? argv[1] : "START";
  boot(strcmp(name, "START") != 0);
  unsigned limit = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 3600;
  if (!enter(find(name), 1))
    return 1;
  if (argc > 2 && !strcmp(argv[2], "rpc"))
    return rpc();
  for (unsigned i = 0; i < limit && !values.failed && !director.quit; i++) {
    int x = 320, y = 240;
    bool down = false;
    if (argc > 3 && !strcmp(argv[3], "spine")) {
      const char *scene = director.movie->code->name;
      down = i % 120 < 20;
      if (!strcmp(scene, "GINTRO.DXR")) {
        x = 50;
        y = 100;
      } else if (!strcmp(scene, "GARDEN.DXR")) {
        x = 195;
        y = 300;
      } else if (!strcmp(scene, "VAKT.DXR")) {
        x = 473;
        y = 139;
      } else if (!strcmp(scene, "PINTRO.DXR")) {
        x = 530;
        y = 35;
      } else
        down = false;
    }
    step(x, y, down);
  }
  printf("RESULT movie=%s frame=%u ticks=%u depth=%u heap=%u peak=%u sounds=%u "
         "error=%s\n",
         director.movie->code->name, director.frame, director.ticks,
         values.depth, values.heap_used, values.heap_high_water, sound_count,
         values.failed ? values.error : "none");
  return values.failed ? 1 : 0;
}
