#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t d;
static lv_runtime_t r;
static const lv_t sprite = lv_make(LV_SPRITE, 1);
static const lv_movie_t code = {"STATE.DXR",0,NULL,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t cast = {"Internal",1,1};
static const dg_member_t members[] = {
  {.id=0x110001,.number=1,.cast=1,.type=3,.text="First"},
  {.id=0x110002,.number=2,.cast=1,.type=3,.text="Second"},
  {.id=0x110003,.number=3,.cast=1,.type=1,.width=10,.height=20},
  {.id=0x110004,.number=4,.cast=1,.type=2,.width=10,.height=20,.film_count=6,.film_loop=1},
  {.id=0x110005,.number=5,.cast=1,.type=6,.rate=60,.samples=600,.looping=1},
  {.id=0x110006,.number=6,.cast=1,.type=2,.width=10,.height=20,.film_count=3},
  {.id=0x110007,.number=7,.cast=1,.type=6,.rate=60,.samples=2},
};
static dg_spec_t initial = {.member=0x110003,.type=16,.blend=100,
                            .x=10,.y=20,.width=10,.height=20};
static dg_delta_t deltas[5];
static dg_frame_t frames[5];
static dg_movie_t movie;
static unsigned stops[2], starts[2], busy_until[2];
static void sound(void *ctx, unsigned ch, const dg_member_t *m) {
  (void)ctx;
  if (m) {
    starts[ch]++;
    busy_until[ch] = m->looping ? UINT32_MAX : d.ticks + m->samples;
  } else {
    stops[ch]++;
    busy_until[ch] = 0;
  }
}
static bool busy(void *ctx, unsigned ch) { (void)ctx; return d.ticks < busy_until[ch]; }
static void boot(void) {
  memset(stops,0,sizeof(stops));
  memset(starts,0,sizeof(starts));
  memset(busy_until,0,sizeof(busy_until));
  memset(frames,0,sizeof(frames));
  for (unsigned i=0;i<5;i++) deltas[i]=(dg_delta_t){6,0,initial};
  frames[0]=(dg_frame_t){0,1};
  deltas[0].mask=DG_ALL;
  movie=(dg_movie_t){.code=&code,.id=1,.tempo=30,.cast_count=1,.casts=&cast,
    .member_count=7,.members=members,.frame_count=5,.frames=frames,.deltas=deltas};
  dg_init(&d,&r,(dg_platform_t){.sound=sound,.sound_busy=busy},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
}
static void change(unsigned frame, unsigned mask, dg_spec_t value) {
  deltas[frame-1]=(dg_delta_t){6,mask,value};
  frames[frame-1]=(dg_frame_t){frame-1,1};
}
static void set(const char *property, int value) { lv_set(&r,NULL,property,sprite,lv_num(value)); }
static lv_t call(const char *name, unsigned count, lv_t *args) {
  lv_t result={0}; bool yield=false;
  assert(r.services.call(&r,name,count,args,&result,&yield) && !r.failed);
  return result;
}
static void puppet(bool enabled) { call("puppetsprite",2,(lv_t[]){lv_num(1),lv_num(enabled)}); }
static void advance(void) {
  // The public event service follows a real score request, with no fake film mutation.
  d.next_frame=d.frame==5 ? 1 : d.frame+1;
  assert(dg_service(&d,DG_SERVICE_BUDGET));
}
static void property_ownership(void) {
  boot();
  dg_spec_t next=initial;
  next.x=30;
  change(3,DG_POSITION,next);
  set("loch",99);
  set("width",71);
  set("height",81);
  set("forecolor",9);
  set("stretch",1);
  set("trails",1);
  set("moveablesprite",1);
  assert(dg_seek(&d,2)); // Empty span preserves automatic overrides.
  assert(d.sprites[1].value.x==99 && d.sprites[1].value.width==71);
  assert(dg_seek(&d,3));
  assert(d.sprites[1].value.x==30 && !(d.sprites[1].auto_mask & DG_POSITION));
  assert(d.sprites[1].value.width==71 && d.sprites[1].value.fore==9);
  next.height=40; next.back=7;
  change(4,DG_HEIGHT|DG_BACK|DG_TYPE,next);
  assert(dg_seek(&d,4));
  assert(d.sprites[1].value.height==40 && d.sprites[1].value.width==71);
  assert(d.sprites[1].value.fore==9 && d.sprites[1].value.back==7);
  assert(d.sprites[1].stretch && d.sprites[1].trails && d.sprites[1].moveable);
  next.width=50; next.fore=3; next.ink=8;
  change(5,DG_WIDTH|DG_FORE|DG_INK|DG_MOVEABLE,next);
  assert(dg_seek(&d,5));
  assert(d.sprites[1].value.width==50 && d.sprites[1].value.fore==3);
  assert(!d.sprites[1].stretch && !d.sprites[1].trails && !d.sprites[1].moveable);
  assert(!d.sprites[1].auto_mask);

  boot(); // Forward jumps aggregate copy-back, rewinds restore the full score.
  next=initial; next.x=30;
  change(3,DG_POSITION,next);
  set("loch",99);
  assert(dg_seek(&d,4) && d.sprites[1].value.x==30);
  set("width",71);
  assert(dg_seek(&d,2));
  assert(d.sprites[1].value.x==10 && d.sprites[1].value.width==10);
  assert(!d.sprites[1].auto_mask);

  boot(); // A cast write releases both dimensions, even without size bytes.
  next=initial; next.member=0x110004;
  change(2,DG_MEMBER,next);
  set("width",71); set("height",81); set("castnum",6);
  assert(dg_seek(&d,2));
  assert(d.sprites[1].value.member==0x110004);
  assert(d.sprites[1].value.width==10 && d.sprites[1].value.height==20);
  assert(!d.sprites[1].auto_mask);

  boot(); // Whole-sprite puppet control survives every authored field and rewind.
  next=initial; next.x=30;
  change(3,DG_ALL,next);
  puppet(true); set("loch",99); set("blend",0);
  assert(dg_seek(&d,3) && d.sprites[1].value.x==99 && dg_opacity(&d.sprites[1])==0);
  assert(dg_seek(&d,1) && d.sprites[1].value.x==99);
  puppet(false);
  assert(d.sprites[1].value.x==10 && dg_opacity(&d.sprites[1])==100);
  assert(!d.sprites[1].auto_mask);
}
static void blend_flags(void) {
  boot();
  dg_spec_t next=initial; next.blend=0;
  change(2,DG_BLEND,next);
  assert(dg_seek(&d,2));
  assert(d.sprites[1].value.blend==0 && dg_opacity(&d.sprites[1])==100);
  next.thickness=DG_HAS_BLEND;
  change(3,DG_THICKNESS,next);
  assert(dg_seek(&d,3) && dg_opacity(&d.sprites[1])==0);
  next.thickness=128; // Tween flag is neither line width nor blending.
  change(4,DG_THICKNESS,next);
  assert(dg_seek(&d,4) && dg_opacity(&d.sprites[1])==100);
  next.ink=32;
  change(5,DG_INK,next);
  assert(dg_seek(&d,5) && dg_opacity(&d.sprites[1])==0);
  assert(dg_seek(&d,1));
  set("blend",0);
  assert(dg_opacity(&d.sprites[1])==0);
  next=initial; next.blend=40;
  change(2,DG_BLEND,next);
  assert(dg_seek(&d,2) && dg_opacity(&d.sprites[1])==40);
  next.thickness=0;
  change(3,DG_THICKNESS,next);
  assert(dg_seek(&d,3) && dg_opacity(&d.sprites[1])==100);
}
static void field_lifecycle(void) {
  boot();
  lv_t first=lv_make(LV_MEMBER, 0x110001), second=lv_make(LV_MEMBER, 0x110002);
  lv_set(&r,NULL,"forecolor",first,lv_num(8));
  lv_set(&r,NULL,"text",first,lv_text(&r,"Changed",false));
  assert(lv_numeric(r.roots[DG_FIELD_COLOR_ROOT])==8);
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",second)),"Second"));
  assert(lv_type(r.roots[DG_FIELD_COLOR_ROOT+1])==LV_VOID);
  assert(lv_numeric(r.roots[DG_FIELD_COLOR_ROOT])==8);
  assert(dg_enter(&d,&movie,1,NULL,0));
  for (unsigned i=DG_FIELD_TEXT_ROOT;i<DG_FIELD_COLOR_ROOT+DG_FIELDS;i++)
    assert(lv_type(r.roots[i])==LV_VOID);
  assert(lv_type(r.roots[0])==LV_LIST);
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",second)),"Second"));
  assert(d.field_members[0]==0x110002 && lv_type(r.roots[DG_FIELD_COLOR_ROOT])==LV_VOID);
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",first)),"First"));
}
static void audio_lifecycle(void) {
  for (unsigned adapter=0;adapter<2;adapter++) {
    boot();
    call("puppetsound",2,(lv_t[]){lv_num(1),lv_num(5)});
    call("puppetsound",2,(lv_t[]){lv_num(2),lv_num(5)});
    assert(busy(NULL,0) && busy(NULL,1));
    if (adapter) { // N64 stops/fences before unloading overlays; native enters directly.
      dg_stop_sounds(&d);
      dg_stop_sounds(&d);
      assert(!busy(NULL,0) && !busy(NULL,1));
    }
    dg_delta_t audio={.channel=4,.value={.member=0x110007}};
    dg_frame_t audio_frame={0,1};
    dg_movie_t destination=movie;
    destination.frame_count=1; destination.frames=&audio_frame; destination.deltas=&audio;
    assert(dg_enter(&d,&destination,1,NULL,0));
    assert(stops[0]==1 && stops[1]==1 && starts[0]==2 && starts[1]==1);
    assert(!d.sound_puppet[0] && !d.sound_puppet[1]);
    assert(d.sounds[0]==0x110007 && !d.sounds[1]);
    assert(lv_truth(&r,call("soundbusy",1,(lv_t[]){lv_num(1)})));
    assert(!lv_truth(&r,call("soundbusy",1,(lv_t[]){lv_num(2)})));
    for (unsigned i=0;i<2;i++) assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));
    assert(!lv_truth(&r,call("soundbusy",1,(lv_t[]){lv_num(1)})));
    assert(dg_enter(&d,&movie,1,NULL,0));
    assert(!busy(NULL,0) && !busy(NULL,1) && !d.sounds[0]);
  }
}
static void film_and_trails(void) {
  boot(); puppet(true);
  for (unsigned i=0;i<5;i++) advance();
  assert(!d.sprites[1].film_frame); // Bitmaps do not accrue a hidden animation clock.
  set("castnum",4); set("trails",1);
  dg_update_stage(&d);
  for (unsigned i=0;i<5;i++) { advance(); dg_update_stage(&d); }
  assert(d.sprites[1].film_frame==5 && d.trail_count==5);
  for (unsigned i=0;i<5;i++) assert(d.trails[i].sprite.film_frame==i);
  set("castnum",4); // Reassigning the same member keeps the current pose.
  assert(d.sprites[1].film_frame==5);
  set("castnum",6); // A different film starts at its first asset.
  assert(d.sprites[1].film_frame==0);
  dg_update_stage(&d);
  advance(); dg_update_stage(&d);
  advance(); dg_update_stage(&d);
  unsigned trails=d.trail_count;
  for (unsigned i=0;i<5;i++) { advance(); dg_update_stage(&d); }
  assert(d.sprites[1].film_frame==2 && d.trail_count==trails); // Nonlooping film clamps.
  set("castnum",4); dg_update_stage(&d);
  for (unsigned i=0;i<6;i++) { advance(); dg_update_stage(&d); }
  assert(!d.sprites[1].film_frame); // Loop wraps rather than overflowing.
  boot();
  dg_spec_t next=initial; next.member=0x110004;
  change(2,DG_MEMBER,next);
  advance();
  assert(d.sprites[1].value.member==0x110004 && !d.sprites[1].film_frame);
  advance();
  assert(d.sprites[1].film_frame==1); // Score assignment also presents pose 0 first.

  boot(); puppet(true); set("castnum",1); set("trails",1);
  dg_update_stage(&d);
  set("forecolor",8); dg_update_stage(&d);
  assert(d.trail_count==1 && d.trails[0].sprite.value.fore==0);
  d.sprites[1].value.back=7; dg_sprite_changed(&d,1); dg_update_stage(&d);
  assert(d.trail_count==2);
  d.sprites[1].value.thickness=2; dg_sprite_changed(&d,1); dg_update_stage(&d);
  assert(d.trail_count==3);
  d.sprites[1].value.thickness|=128; dg_sprite_changed(&d,1); dg_update_stage(&d);
  assert(d.trail_count==3); // Non-rendered flag changes do not stamp a duplicate.
  assert(!r.failed);
}
int main(void) {
  property_ownership(); blend_flags(); field_lifecycle(); audio_lifecycle(); film_and_trails();
  puts("Director state: D6 property release, blend flags, field/audio lifecycle, film/trails OK");
}
