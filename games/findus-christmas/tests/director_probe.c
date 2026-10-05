// Input-driven native probe for the original D7 launcher and recovered media.
#include "game.h"
#include "image.h"
#include "director.h"
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
static unsigned busy_until[DG_SOUND_CHANNELS], sound_started[DG_SOUND_CHANNELS], sound_count;
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
static bool write_file(void *ctx, const char *name, const char *data, unsigned length) {
  (void)ctx; return game_write_file(name, data, length);
}
static void sound(void *ctx, unsigned channel, const dg_member_t *m) {
  (void)ctx;
  busy_until[channel] = director.ticks + (m && m->rate ? (m->samples * 60u + m->rate - 1) / m->rate : 0);
  sound_started[channel] = director.ticks;
  if (m && m->looping) busy_until[channel] = UINT32_MAX;
  sound_count++;
}
static bool busy(void *ctx, unsigned channel) { (void)ctx; return director.ticks < busy_until[channel]; }
// No mixer here, so playback advances with the tick the sound started on.
static unsigned position(void *ctx, unsigned channel) {
  (void)ctx;
  if (director.ticks <= sound_started[channel]) return 0;
  return (director.ticks - sound_started[channel]) * 1000u / 60u;
}
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
static void boot(void) {
  memset(busy_until, 0, sizeof(busy_until));
  memset(sound_started, 0, sizeof(sound_started));
  game_save_load((save_backend_t){flash_read, flash_write, NULL});
  dg_init(&director, &values, (dg_platform_t){.read_file=read_file, .write_file=write_file,
    .nth_file=game_nth_file, .long_date=game_long_date, .sound=sound, .sound_busy=busy, .sound_position=position, .trace=trace, .hit=hit},
    NULL, global_names, sizeof(global_names)/sizeof(*global_names), 42);
}
#include "draw_record.inc"
#include "step.inc"
static void quoted(const char *text) {
  putchar('"'); for (const unsigned char *p=(const unsigned char *)text; *p; p++) {
    if (*p < 32 || *p == '"' || *p == '\\') printf("\\u%04x", *p); else putchar(*p);
  } putchar('"');
}
#include "state_keys.inc"
#include "state.inc"
int main(int argc,char **argv) {
  bool export_requested = false;
  memset(flash,255,sizeof(flash)); boot(); if(!enter(find(argc>1?argv[1]:"START"),1)){state();return 1;}
  if(argc>2&&!strcmp(argv[2],"rpc")) {
    char command[128];state();while(fgets(command,sizeof(command),stdin)) {
      unsigned ticks=0;int x=320,y=240,down=0;
      unsigned text_sprite=0;char entered[64];
      if(!strcmp(command,"quit\n")) break;
      // Diagnostic export is requested only after the host journey assertions
      // pass. Ordinary quit, failure and EOF never create a save fixture.
      if(!strcmp(command,"quit-export\n")) {export_requested=true;break;}
      if(sscanf(command,"text %u %63[^\n]",&text_sprite,entered)==2) {
        if(!dg_edit_text(&director,text_sprite,entered))lv_fail(&values,"invalid text entry");
      }
      else if(!strcmp(command,"reboot\n")){boot();enter(find("START"),1);}
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
