#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t d;
static lv_runtime_t v;
static const lv_movie_t code = {"CURSORS.DXR", 0, NULL,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[] = {{"Internal",1,1},{"Shared",2,1}};
static const dg_member_t members[] = {
  {.id=0x110001,.number=1,.cast=1,.type=1,.width=16,.height=16,.name="image"},
  {.id=0x110002,.number=2,.cast=1,.type=1,.width=13,.height=12,.name="mask"},
  {.id=0x110003,.number=3,.cast=1,.type=1,.width=40,.height=40,.name="target"},
};
static const dg_frame_t frame = {0,0};
static const dg_movie_t movie = {.code=&code,.id=1,.tempo=30,
  .cast_count=2,.casts=casts,.member_count=3,.members=members,
  .frame_count=1,.frames=&frame};
static const lv_movie_t shared_code = {"SHARED.CXT",0,NULL,NULL, NULL, NULL, NULL, 0};
static const dg_member_t shared_member = {.id=0x210001,.number=1,.cast=1,
  .type=1,.width=16,.height=16,.name="external"};
static const dg_movie_t shared = {.code=&shared_code,.id=2,.cast_count=1,
  .casts=&casts[1],.member_count=1,.members=&shared_member,.frame_count=1,.frames=&frame};
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx;(void)m;(void)y;
  return ink != 8 || x >= 20;
}
static void boot(void) {
  dg_init(&d,&v,(dg_platform_t){.hit=hit},NULL,NULL,0,1);
  const dg_movie_t *files[] = {&shared};
  assert(dg_enter(&d,&movie,1,files,1));
}
static void call(const char *name, lv_t arg) {
  lv_t out={0}; bool yield=false;
  assert(v.services.call(&v,name,1,&arg,&out,&yield));
  assert(!yield);
}
static lv_t pair(lv_t a, lv_t b) { return lv_list(&v,2,(lv_t[]){a,b},false); }
static lv_t sprite(unsigned n) { return lv_make(LV_SPRITE, (int)n); }
static lv_t member(unsigned n) { return lv_make(LV_MEMBER, (int)n); }
static void target(unsigned n, int cursor) {
  d.sprites[n].value=(dg_spec_t){.member=0x110003,.type=1,.x=100,.y=100};
#if DIRECTOR64_DIRECTOR_VERSION != 6 || DIRECTOR64_EXTENDED_D6
  d.sprites[n].value.loc_z=(int)n;
#endif
  lv_set(&v,NULL,"cursor",sprite(n),lv_num(cursor));
}
static void references(void) {
  boot();
  lv_t source=pair(member(0x210001),member(0x110002));
  call("cursor",source);
  assert(!v.failed && d.cursor.image==0x210001 && d.cursor.mask==0x110002);
  // No GC root or mutable list identity is retained by the engine.
  lv_set_at(&v,source,1,lv_num(3));
  assert(d.cursor.image==0x210001);
  lv_t result=lv_get(&v,NULL,"cursor",(lv_t){0});
  assert(lv_type(result)==LV_LIST && lv_count(&v,result)==2);
  assert(lv_integer(&v,lv_at(&v,result,1))==131073);
  assert(lv_integer(&v,lv_at(&v,result,2))==2);
  lv_set(&v,NULL,"cursor",sprite(2),result);
  assert(dg_cursor_equal(d.sprites[2].cursor,d.cursor));
  // Numeric list members use Director's multiplexing, not resource IDs.
  call("cursor",pair(lv_num(1),lv_num(2)));
  assert(!v.failed && d.cursor.image==0x110001);
  call("cursor",lv_list(&v,1,(lv_t[]){member(0x110001)},false));
  assert(d.cursor.mask==0);
  call("cursor",lv_get(&v,NULL,"cursor",(lv_t){0}));
  assert(!v.failed && d.cursor.image==0x110001 && !d.cursor.mask);
  dg_cursor_t before=d.cursor;
  call("cursor",pair(lv_num(999),lv_num(2)));
  assert(v.failed && dg_cursor_equal(before,d.cursor));
  boot(); call("cursor",lv_num(128));
  assert(v.failed && !dg_cursor_active(d.cursor));
  boot(); call("cursor",lv_list(&v,0,NULL,false));
  assert(v.failed && !dg_cursor_active(d.cursor));
}
static void selection(void) {
  boot();
  call("cursor",lv_num(4));
  target(1,1);target(2,0);
  assert(dg_hit(&d,110,110)==2);
  assert(dg_cursor_at(&d,110,110).resource==1); // Cursorless foreground skipped.
  lv_set(&v,NULL,"cursor",sprite(2),lv_num(3));
  assert(dg_cursor_at(&d,110,110).resource==3);
#if DIRECTOR64_DIRECTOR_VERSION != 6 || DIRECTOR64_EXTENDED_D6
  d.sprites[1].value.loc_z=3;dg_sprite_changed(&d,1);
  assert(dg_cursor_at(&d,110,110).resource==1);
  d.sprites[1].value.loc_z=1;dg_sprite_changed(&d,1);
#endif
  d.sprites[2].value.ink=8;
  assert(dg_cursor_at(&d,110,110).resource==1); // Matte hole.
  assert(dg_cursor_at(&d,130,110).resource==3);
  d.sprites[2].visible=false;
  assert(dg_cursor_at(&d,130,110).resource==1);
  d.sprites[2].visible=true;
  d.sprites[2].value.ink=0;
  lv_set(&v,NULL,"cursor",sprite(2),lv_num(-1));
  assert(dg_cursor_at(&d,110,110).resource==-1); // Explicit arrow, not clear.
  lv_set(&v,NULL,"cursor",sprite(2),lv_num(200));
  assert(dg_cursor_at(&d,110,110).resource==200);
  call("cursor",lv_num(200));
  lv_set(&v,NULL,"cursor",sprite(2),lv_num(0));
  assert(dg_cursor_at(&d,110,110).resource==1); // Sprite overrides hidden default.
  assert(dg_cursor_at(&d,0,0).resource==200);
  d.mouse_x=d.mouse_y=110;
  dg_cursor_t before=dg_cursor_current(&d);
  lv_set(&v,NULL,"cursor",sprite(1),pair(member(0x110001),member(0x110002)));
  dg_update_stage(&d);
  assert(!dg_cursor_equal(before,dg_cursor_current(&d))); // Stationary mouse.
  d.sprites[1].value.x=200;
  assert(dg_cursor_current(&d).resource==200);
  const dg_movie_t *files[]={&shared};
#if DIRECTOR64_DIRECTOR_VERSION == 5
  target(1,3);d.mouse_x=d.mouse_y=110;
  d.window_open=true;
  assert(dg_enter(&d,&movie,1,files,1));
  assert(d.suspended_stage && !dg_cursor_active(d.cursor));
  d.mouse_x=-2;d.mouse_y=10; // Outside dialog, at stage (110,110).
  assert(dg_cursor_current(&d).resource==3);
  call("cursor",lv_num(1));
  call("continue",(lv_t){0});
  assert(!d.suspended_stage && d.cursor.resource==200);
  assert(d.sprites[1].cursor.resource==3);
#endif
  assert(dg_enter(&d,&movie,1,files,1));
  assert(!dg_cursor_active(d.cursor) && !dg_cursor_active(d.sprites[1].cursor));
  assert(!v.failed);
}
static void tinting(void);
static void composition(void) {
  uint16_t image[256],mask[256],pixels[256];
  for(unsigned i=0;i<256;i++){image[i]=0xfffe;mask[i]=0xffff;}
  image[0]=1;mask[0]=mask[1]=1;
  dg_cursor_bitmap_t b;
  assert(dg_cursor_compose(&b,image,15,16,mask,13,12,7,6));
  dg_cursor_pixels(&b,pixels);
  assert(b.hot_x==7 && b.hot_y==6);
  assert(pixels[0]==1 && pixels[1]==0xffff && pixels[2]==0);
  for(unsigned i=16*12;i<256;i++)assert(!pixels[i]);
  assert(dg_cursor_compose(&b,image,15,16,NULL,0,0,-1,20));
  assert(b.hot_x==8 && b.hot_y==8);
  dg_cursor_pixels(&b,pixels);
  assert(pixels[1]==0xffff && pixels[14]==0xffff && !pixels[15]);
  image[0]=0xf801;
  assert(!dg_cursor_compose(&b,image,16,16,NULL,0,0,8,8));
  dg_cursor_bitmap_t shapes[6];
  for(int n=-1;n<=4;n++) {
    assert(dg_cursor_builtin(&shapes[n+1],n));
    assert(shapes[n+1].hot_x<16 && shapes[n+1].hot_y<16);
    if(n>0)assert(memcmp(&shapes[n+1],&shapes[n],sizeof(b)));
  }
  assert(!memcmp(&shapes[0],&shapes[1],sizeof(b)));
  assert(dg_cursor_builtin(&b,200));
  dg_cursor_pixels(&b,pixels);
  for(unsigned i=0;i<256;i++)assert(!pixels[i]);
  assert(!dg_cursor_builtin(&b,128));
  tinting();
}
/* Several players share one cursor shape and are told apart by its ink. The
 * white contrast pixels and the transparent ones never take a player colour. */
static void tinting(void) {
  uint16_t image[256],mask[256],plain[256],tinted[256];
  for(unsigned i=0;i<256;i++){image[i]=0xfffe;mask[i]=0xffff;}
  image[0]=1;mask[0]=mask[1]=1;
  dg_cursor_bitmap_t b;
  assert(dg_cursor_compose(&b,image,16,16,mask,16,16,7,6));
  dg_cursor_pixels(&b,plain);
  uint16_t ink=dg_cursor_ink(248,32,16);
  assert(ink==(uint16_t)((31u<<11)|(4u<<6)|(2u<<1)|1u) && ink!=1);
  dg_cursor_pixels_ink(&b,tinted,ink);
  for(unsigned i=0;i<256;i++)
    assert(tinted[i]==(plain[i]==1?ink:plain[i]));
  assert(tinted[0]==ink && tinted[1]==0xffff && !tinted[2]);
  /* No colour at all keeps the authored black rather than losing coverage. */
  dg_cursor_pixels_ink(&b,tinted,0);
  assert(!memcmp(tinted,plain,sizeof(plain)));
}
int main(void) {
  references();selection();composition();
  printf("cursor D%d%s: references, selection, lifecycle, masks passed\n",
      DIRECTOR64_DIRECTOR_VERSION,DIRECTOR64_EXTENDED_D6?" extended":"");
  return 0;
}
