#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// LZ8 revisits a labelled page after its heading was played. beginSprite
// clears the sound name while exitFrame still calls playFile("@:Sounds:" &
// EMPTY). An empty filename leaves playback alone, including a current voice.
// Original-projector evidence: tools/reference/stream-empty[-control].lingo,
// loaded Director 8.5.1r104 under Wine. This differs from the null-stream stop
// in clean ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc sound.cpp.
static dg_runtime_t director;
static lv_runtime_t values;
static bool busy;
static unsigned starts, stops, lookups;
static const lv_movie_t code = {.name="STREAM.DXR"};
static const dg_frame_t frames[] = {{0,0}};
static const dg_movie_t movie = {.code=&code,.id=1,.tempo=30,
  .frame_count=1,.frames=frames};

static const char *movie_path(void *context) {
  (void)context;
  return "C:\\DATA\\LEXIKON\\";
}
static const char *stream_name(void *context, const char *name) {
  (void)context;
  lookups++;
#ifdef DIRECTOR64_STREAM_PATHS
  return !strcmp(name,"lexikon/sounds/title") ? "packaged-title" : NULL;
#else
  return !strcmp(name,"sounds/title") ? "packaged-title" : NULL;
#endif
}
static void play_file(void *context, unsigned channel, const char *name) {
  (void)context;
  assert(channel == 0);
  if (name) {
    assert(!strcmp(name,"packaged-title"));
    starts++;
    busy = true;
  } else {
    stops++;
    busy = false;
  }
}
static void sound(void *context, unsigned channel, const dg_member_t *member) {
  assert(!member);
  play_file(context,channel,NULL);
}
static void boot(void) {
  busy = false;
  starts = stops = lookups = 0;
  dg_init(&director,&values,(dg_platform_t){.movie_path=movie_path,
    .stream_name=stream_name,.play_file=play_file,.sound=sound},NULL,NULL,0,42);
  assert(dg_enter(&director,&movie,1,NULL,0));
}
static void play(int channel, const char *name) {
  lv_t out = {0};
  bool yield = false;
  lv_t args[] = {lv_num(channel),lv_text(&values,name,false)};
  assert(values.services.call(&values,"sound_playfile",2,args,&out,&yield));
  assert(!yield);
}
int main(void) {
  const char *empty_names[] = {"", "@:", "@:Sounds:", "C:\\", "C:\\absent\\",
    "Sounds/", "/absent/"};
  for (unsigned i=0; i<sizeof(empty_names)/sizeof(*empty_names); i++) {
    boot();
    play(1,"@:Sounds:TITLE.AIF");
    assert(!values.failed && busy && starts == 1 && lookups == 1);
    director.sound_paused[0] = true;
    director.fade_duration[0] = 60;
    unsigned serial = director.sound_serial[0];
    play(1,empty_names[i]);
    assert(!values.failed && !values.script_error_count);
    assert(busy && stops == 0 && starts == 1 && lookups == 1);
    assert(director.sound_paused[0] && director.fade_duration[0] == 60);
    assert(director.sound_puppet[0] && director.sound_serial[0] == serial);
    assert(director.linked_sound_owner[0] == &director);
    play(1,"@:Sounds:title.aif");
    assert(!values.failed && busy && starts == 2 && lookups == 2);
    boot();
    play(1,empty_names[i]);
    assert(!values.failed && !busy && !starts && !stops && !lookups);
    assert(!director.sound_puppet[0] && !director.linked_sound_owner[0]);
  }
  // Original playback tolerates absent files, but a named required asset
  // absent from our package must keep its actionable completeness failure.
  boot();
  play(1,"@:Sounds:title.aif");
  play(1,"@:Sounds:missing-title.aif");
  assert(values.failed && busy && starts == 1 && stops == 0 && lookups == 2);
  assert(strstr(values.error,"missing packaged streamed sound"));
  boot();
  play(0,"");
  assert(values.failed && !starts && !stops && !lookups);
  assert(strstr(values.error,"sound channel"));
  puts("Empty streamed sound retains playback; missing named media stays fatal");
}
