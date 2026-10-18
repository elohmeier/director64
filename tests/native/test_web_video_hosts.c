#include "web_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t director;
static lv_runtime_t values;
static web_host_t host;
#include "../../platforms/web/video.inc"

typedef struct {
  unsigned id, serial;
  double time, rate;
  float gain;
  bool active, sound, picture;
} record_t;
static record_t records[16];
static unsigned count, closes;
static void video(void *context, unsigned id, const char *asset,
                  const char *audio, double time, double rate, unsigned serial,
                  float gain) {
  (void)context;
  unsigned i = 0;
  while (i < count && records[i].id != id)
    i++;
  if (!asset) {
    assert(i < count && records[i].active);
    records[i].active = false;
    closes++;
    return;
  }
  assert(count < 16);
  if (i == count)
    count++;
  records[i] = (record_t){id, serial, time, rate, gain, true, audio && *audio, asset && *asset};
}
static const dg_cast_t root_cast = {.name="Internal",.file=1,.cast=1},
                       child_cast = {.name="Internal",.file=2,.cast=1};
static const lv_movie_t root_code = {.name = "ROOT"},
                        child_code = {.name = "CHILD"};
static const dg_member_t root_members[] = {
    {.id = DG_ID(1, 1, 1),
     .number = 1,
     .cast = 1,
     .type = 10,
     .asset = "root",
     .samples = 6000,
     .video_flags = 8,
     .video_audio = "root-audio"},
    {.id = DG_ID(1, 1, 2),
     .number = 2,
     .cast = 1,
     .type = 9,
     .width = 8,
     .height = 8,
     .linked_movie = 2,
     .linked_flags = 0x0b},
    {.id = DG_ID(1, 1, 3),
     .number = 3,
     .cast = 1,
     .type = 9,
     .width = 8,
     .height = 8,
     .linked_movie = 2,
     .linked_flags = 0x03},
};
static const dg_member_t child_member = {.id = DG_ID(2, 1, 1),
                                         .number = 1,
                                         .cast = 1,
                                         .type = 10,
                                         .asset = "child",
                                         .samples = 6000,
                                         .video_flags = 8,
                                         .video_audio = "child-audio"};
static const dg_delta_t root_delta[] = {
    {.channel = 6,
     .mask = DG_ALL,
     .value = {.type = 1, .member = DG_ID(1, 1, 1)}},
    {.channel = 7,
     .mask = DG_ALL,
     .value = {.type = 1, .member = DG_ID(1, 1, 2), .width = 8, .height = 8}},
    {.channel = 8,
     .mask = DG_ALL,
     .value = {.type = 1, .member = DG_ID(1, 1, 3), .width = 8, .height = 8}},
};
static const dg_delta_t child_delta = {
    .channel = 6,
    .mask = DG_ALL,
    .value = {.type = 1, .member = DG_ID(2, 1, 1)}};
static const dg_frame_t root_frames[] = {{0, 3}}, child_frames[] = {{0, 1}};
static const dg_movie_t root_movie = {.id = 1,
                                      .code = &root_code,
                                      .casts = &root_cast,
                                      .cast_count = 1,
                                      .members = root_members,
                                      .member_count = 3,
                                      .tempo = 1,
                                      .frame_count = 1,
                                      .frames = root_frames,
                                      .deltas = root_delta};
static const dg_movie_t child_movie = {.id = 2,
                                       .code = &child_code,
                                       .casts = &child_cast,
                                       .cast_count = 1,
                                       .members = &child_member,
                                       .member_count = 1,
                                       .tempo = 1,
                                       .frame_count = 1,
                                       .frames = child_frames,
                                       .deltas = &child_delta};
static const dg_movie_t *find(void *context, const char *stem, unsigned file) {
  (void)context;
  (void)stem;
  return file == 2 ? &child_movie : &root_movie;
}
int main(void) {
  host.video = video;
  dg_init(&director, &values, (dg_platform_t){.find_movie = find}, NULL, NULL,
          0, 1);
  assert(dg_enter(&director, &root_movie, 1, NULL, 0));
  assert(dg_tick(&director, -100, -100, false, DG_SERVICE_BUDGET));
  dg_runtime_t *first = dg_linked_begin(&director, 2);
  assert(first);
  first->sprites[1].video_time = 120;
  first->sprites[1].video_rate = 1;
  first->sprites[1].video_serial = 7;
  unsigned first_id = first->linked_instance * DG_SPRITES + 1;
  dg_linked_end(&director);
  dg_runtime_t *second = dg_linked_begin(&director, 3);
  assert(second);
  second->sprites[1].video_time = 360;
  second->sprites[1].video_rate = 1;
  second->sprites[1].video_serial = 11;
  unsigned second_id = second->linked_instance * DG_SPRITES + 1;
  dg_linked_end(&director);
  update_videos();
  assert(values.context == &director && !values.failed);
  assert(count == 3 && records[0].id == 1 && records[0].sound);
  assert(records[1].id == first_id && records[1].time == 120 &&
         records[1].serial == 7 && records[1].sound);
  assert(records[2].id == second_id && records[2].time == 360 &&
         records[2].serial == 11 && !records[2].sound);
  assert(first_id != second_id && first_id != 1 && second_id != 1);
  director.sprites[2].film_paused = true;
  update_videos();
  assert(records[1].rate == 0 && records[2].rate == 1);
  director.sprites[2].film_paused = false;
  update_videos();
  assert(records[1].rate == 1 && records[1].time == 120);
  // Freed child contexts can be reallocated at the same address. Their host
  // identities must still change, and old players must close before reuse.
  dg_linked_dispose(&director);
  update_videos();
  assert(closes == 2 && records[0].active && !records[1].active &&
         !records[2].active);
  assert(dg_tick(&director, -100, -100, false, DG_SERVICE_BUDGET));
  update_videos();
  assert(count == 5 && records[3].id != first_id && records[4].id != second_id);
  close_videos();
  assert(closes == 5);
  dg_linked_dispose(&director);
  dg_images_dispose(&director);
  // Flash keeps its picture in the compositor and streams audio through
  // the same dedicated, independent per-instance host players.
  static const uint32_t positions[]={0,50,100,150};
  static const dg_member_t flash={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,
    .source_xtra=1,.asset="picture.fdi",.film_count=4,.film_loop=1,.flash_rate=12*65536,
    .video_audio="flash.wav64",.rate=600,.samples=200,.flash_audio_positions=positions};
  static const dg_delta_t flash_delta={.channel=6,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1)}};
  static const dg_movie_t flash_movie={.id=1,.code=&root_code,.casts=&root_cast,.cast_count=1,
    .members=&flash,.member_count=1,.tempo=60,.frames=child_frames,.frame_count=1,.deltas=&flash_delta};
  count=closes=0;memset(records,0,sizeof(records));
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&flash_movie,1,NULL,0));
  for(unsigned i=0;i<10;i++)assert(dg_tick(&director,0,0,false,DG_SERVICE_BUDGET));
  update_videos();
  assert(count==1 && records[0].sound && !records[0].picture && records[0].time==100 && records[0].rate==1);
  lv_t out=0;bool yield=false;
  assert(values.services.call(&values,"stop",1,(lv_t[]){lv_make(LV_SPRITE,1)},&out,&yield));
  assert(dg_tick(&director,0,0,false,DG_SERVICE_BUDGET));update_videos();
  assert(records[0].rate==0 && records[0].time==100);
  close_videos();assert(closes==1);
  puts("web video hosts: independent child clocks, audio flags and instance "
       "cleanup passed");
}
