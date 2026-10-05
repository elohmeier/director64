// Input-driven native probe for the original D10 launcher and recovered media.
#include "game.h"
#include "image.h"
#include "director.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern const dg_movie_t *const dg_registry[];
extern const unsigned dg_registry_count;
static const char *const global_names[] = {
#include "globals.inc"
};
static lv_runtime_t values;
static dg_runtime_t director;
static unsigned busy_until[DG_SOUND_CHANNELS], sound_count;
static unsigned char flash[131072];
static bool flash_read(void *ctx, size_t offset, void *data, size_t length) {
  (void)ctx; if (offset > sizeof(flash) || length > sizeof(flash) - offset) return false;
  memcpy(data, flash + offset, length); return true;
}
static bool flash_write(void *ctx, size_t offset, const void *data, size_t length) {
  (void)ctx; if (offset > sizeof(flash) || length > sizeof(flash) - offset) return false;
  memcpy(flash + offset, data, length); return true;
}
static bool read_file(void *ctx, const char *name, char *data, unsigned cap, unsigned *length) {
  (void)ctx; return game_read_file(name, data, cap, length);
}
// Shipped read-only data mirrors the ROM filesystem layout under the work
// directory, so the host reads exactly what the ROM packs.
static bool read_data(void *ctx, const char *name, char *data, unsigned cap, unsigned *length) {
  (void)ctx;
  const char *work = getenv("DIRECTOR64_WORK_DIR");
  char path[512];
  if (!work || snprintf(path, sizeof(path), "%s/filesystem/%s", work, name) >= (int)sizeof(path))
    return false;
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  size_t read = fread(data, 1, cap, file);
  bool complete = read < cap || feof(file);
  fclose(file);
  *length = (unsigned)read;
  return complete;
}
static bool write_file(void *ctx, const char *name, const char *data, unsigned length) {
  (void)ctx; return game_write_file(name, data, length);
}
static void sound(void *ctx, unsigned channel, const dg_member_t *m) {
  (void)ctx;
  busy_until[channel] = director.ticks + (m && m->rate ? (m->samples * 60u + m->rate - 1) / m->rate : 0);
  if (m && m->looping) busy_until[channel] = UINT32_MAX;
  sound_count++;
}
static bool busy(void *ctx, unsigned channel) { (void)ctx; return director.ticks < busy_until[channel]; }
static void trace(void *ctx, const char *text) { (void)ctx; fprintf(stderr, "TRACE %s\n", text); }
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx; return native_image_hit(&values, m, ink, x, y);
}
static const dg_movie_t *find(const char *name) {
  char normalized[64]; snprintf(normalized, sizeof(normalized), "%s", name);
  for (char *p = normalized; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
  if (!strchr(normalized, '.') && strlen(normalized) < sizeof(normalized) - 5) strcat(normalized, ".DXR");
  for (unsigned i = 0; i < dg_registry_count; i++) if (!strcmp(dg_registry[i]->code->name, normalized)) return dg_registry[i];
  return NULL;
}
static const dg_movie_t *find_movie(void *ctx, const char *stem, unsigned file) {
  (void)ctx;
  for (unsigned i = 0; i < dg_registry_count; i++) {
    const dg_movie_t *m = dg_registry[i];
    if (!stem) {
      if (m->id == file) return m;
      continue;
    }
    const char *n = m->code->name;
    unsigned k = 0;
    for (; stem[k] && n[k] && n[k] != '.'; k++) {
      char a = stem[k], b = n[k];
      if (a >= 'a' && a <= 'z') a -= 32;
      if (b >= 'a' && b <= 'z') b -= 32;
      if (a != b) break;
    }
    if (!stem[k] && (!n[k] || n[k] == '.')) return m;
  }
  return NULL;
}
static bool enter(const dg_movie_t *movie, unsigned frame) {
  const dg_movie_t *shared[DG_FILES - 1]; unsigned count = 0;
  if (!movie) { lv_fail(&values, "probe missing movie"); return false; }
  for (unsigned i = 0; i < movie->cast_count; i++) if (movie->casts[i].file != movie->id)
    for (unsigned j = 0; j < dg_registry_count; j++) if (dg_registry[j]->id == movie->casts[i].file) {
      bool added = false; for (unsigned k = 0; k < count; k++) if (shared[k] == dg_registry[j]) added = true;
      if (!added) { if (count == DG_FILES - 1) return false; shared[count++] = dg_registry[j]; }
    }
  fprintf(stderr, "ENTER %s frame=%u tick=%u\n", movie->code->name, frame, director.ticks);
  return dg_enter(&director, movie, frame, shared, count);
}
static void play_file(void *ctx, unsigned channel, const char *name) {
  (void)ctx;
  if (channel >= DG_SOUND_CHANNELS) return;
  // Converted streams play for their virtual name; the probe accounts time
  // with a nominal one-second busy window and logs the request.
  busy_until[channel] = director.ticks + 60; sound_count++;
  fprintf(stderr, "STREAM channel=%u name=%s tick=%u\n", channel + 1, name, director.ticks);
}
static void boot(void) {
  memset(busy_until, 0, sizeof(busy_until));
  game_save_load((save_backend_t){flash_read, flash_write, NULL});
  dg_init(&director, &values, (dg_platform_t){.read_file=read_file, .write_file=write_file,
    .nth_file=game_nth_file, .long_date=game_long_date, .sound=sound, .sound_busy=busy, .trace=trace, .hit=hit, .play_file=play_file, .find_movie=find_movie, .read_data=read_data},
    NULL, global_names, sizeof(global_names)/sizeof(*global_names), 42);
}
#include "probe_stats.inc"
#include "draw_record.inc"
#include "step.inc"
static void quoted(const char *text) {
  // Escape every non-ASCII byte: authored strings mix windows-1252 bytes
  // (numToChar-built database text) with UTF-8 member text, and the report
  // must stay parseable JSON either way.
  putchar('"'); for (const unsigned char *p=(const unsigned char *)text; *p; p++) {
    if (*p < 32 || *p == '"' || *p == '\\' || *p > 126) printf("\\u%04x", *p); else putchar(*p);
  } putchar('"');
}
#include "state_keys.inc"
#include "state.inc"
int main(int argc,char **argv) {
  bool export_requested = false;
  memset(flash,255,sizeof(flash)); boot(); if(!enter(find(argc>1?argv[1]:"DEUTSCH"),1)){state();return 1;}
  if(argc>2&&!strcmp(argv[2],"rpc")) {
    char command[128];state();while(fgets(command,sizeof(command),stdin)) {
      unsigned ticks=0;int x=320,y=240,down=0;
      unsigned text_sprite=0;char entered[64];
      if(!strcmp(command,"quit\n")) break;
      // Diagnostic export is requested only after the host journey assertions
      // pass. Ordinary quit, failure and EOF never create a save fixture.
      if(!strcmp(command,"quit-export\n")) {export_requested=true;break;}
      if(!strcmp(command,"perf\n")) {perf_report();continue;}
      if(!strcmp(command,"heap\n")) {heap_report();continue;}
      if(sscanf(command,"text %u %63[^\n]",&text_sprite,entered)==2) {
        if(!dg_edit_text(&director,text_sprite,entered))lv_fail(&values,"invalid text entry");
      }
      else if(!strcmp(command,"reboot\n")){boot();enter(find("DEUTSCH"),1);}
      else if(sscanf(command,"step %u %d %d %d",&ticks,&x,&y,&down)>=1) {
        if(ticks>360000)ticks=360000;
        for(unsigned i=0;i<ticks&&!values.failed&&!director.quit;i++)if(!step(x,y,down!=0?1u:0u))break;
      }
      state();if(values.failed)return 1;
    }
  } else {
    unsigned ticks=argc>2?(unsigned)strtoul(argv[2],NULL,10):3600;
    for(unsigned i=0;i<ticks&&!values.failed&&!director.quit;i++)if(!step(320,240,0))break;
    state();
  }
  if(export_requested && !values.failed) {
    const char *path=getenv("DIRECTOR64_FLASH_EXPORT");
    if(!path || !*path || !game_save_generation()) lv_fail(&values,"invalid native flash export request");
    else {
      FILE *out=fopen(path,"wbx");
      if(!out) lv_fail(&values,"cannot create native flash export");
      else {
        bool ok=fwrite(flash,1,sizeof(flash),out)==sizeof(flash);
        if(fclose(out)) ok=false;
        if(!ok) {remove(path);lv_fail(&values,"cannot complete native flash export");}
      }
    }
  }
  if(values.failed)fprintf(stderr,"NATIVE_FAIL %s\n",values.error);
  return values.failed?1:0;
}
