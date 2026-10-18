#include "film_crop.h"
#include <assert.h>
#include <stdio.h>
#define ID(n) DG_ID(1,1,n)
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code={.name="CROP.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t score={0};
static const dg_member_t members[]={
  {.id=ID(1),.number=1,.cast=1,.type=2,.width=800,.height=600,
   .film_count=1,.film_left=10,.film_top=20},
  {.id=ID(2),.number=2,.cast=1,.type=1,.width=20,.height=10,.reg_x=2,.reg_y=3},
  {.id=ID(3),.number=3,.cast=1,.type=3,.width=40,.height=20,.text="text"},
  {.id=ID(4),.number=4,.cast=1,.type=2,.width=100,.height=50,.reg_x=10,.reg_y=20,.film_count=1},
  {.id=ID(5),.number=5,.cast=1,.type=6},
  {.id=ID(6),.number=6,.cast=1,.type=15},
  {.id=ID(7),.number=7,.cast=1,.type=2,.source_xtra=1,.width=20,.height=20},
};
static const dg_movie_t movie={.id=1,.code=&code,.cast_count=1,.casts=&cast,
 .member_count=7,.members=members,.frame_count=1,.frames=&score};
static dg_film_layer_t layer={.channel=1,.value={.member=ID(2),.type=16,
 .x=112,.y=123,.blend=100,.thickness=DG_HAS_BLEND}};
static dg_film_frame_t frame={1,&layer};
static n64_film_crop_t crop(void){return n64_film_crop(&d,1,&members[0],&frame,640,480);}
static void expect(unsigned l,unsigned t,unsigned w,unsigned h) {
  n64_film_crop_t got=crop();
  if(got.left!=l || got.top!=t || got.width!=w || got.height!=h)
    fprintf(stderr,"crop %u,%u,%u,%u expected %u,%u,%u,%u\n",got.left,got.top,got.width,got.height,l,t,w,h);
  assert(got.left==l && got.top==t && got.width==w && got.height==h);
}
static void transformed_coverage(void) {
  n64_film_crop_t got=crop();
  assert(!(got.left&3) && !(got.top&3));
  dg_sprite_t child;assert(dg_film_layer_sprite(&d,1,&layer,&child));
  child.value.x-=members[0].film_left;child.value.y-=members[0].film_top;
  float q[8];dg_sprite_quad(&d,&child,q);
  for(unsigned i=0;i<8;i++)q[i]*=.8f;
  unsigned covered=0;
  for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++) {
    float u,v;
    if(dg_quad_uv(q,x+.5f,y+.5f,&u,&v)) {
      assert(x>=got.left && y>=got.top && x<got.left+got.width && y<got.top+got.height);
      covered++;
    }
  }
  assert(covered);
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
  expect(80,80,16,8); // authored film origin and child registration
  dg_film_layer_t with_empty[]={layer,{.channel=2,.value={.type=16,
      .width=800,.height=600,.blend=100,.thickness=DG_HAS_BLEND}}};
  frame=(dg_film_frame_t){2,with_empty};expect(80,80,16,8);
  frame=(dg_film_frame_t){1,&layer};
  layer.value.x=809;layer.value.y=619;expect(636,476,4,4); // clip and preserve phase
  layer.value.x=0;layer.value.y=0;expect(0,0,0,0);
  layer.value.x=112;layer.value.y=123;layer.value.blend=0;expect(0,0,0,0);
  layer.value.blend=100;layer.value.member=ID(5);expect(0,0,0,0);
  layer.value.member=ID(99);expect(0,0,0,0); // unresolved placeholder draws nothing
  layer.value.member=ID(6);expect(0,0,640,480); // unknown visual provider
  layer.value.member=ID(7);expect(0,0,640,480); // Flash may exceed member bounds
  layer.value.member=ID(2);layer.name="dynamic";expect(0,0,640,480);layer.name=NULL;
  layer.affine=true;expect(0,0,640,480);layer.affine=false;
  layer.value.member=ID(3);layer.value.x=110;layer.value.y=120;expect(80,80,32,16);
  lv_set(&r,NULL,"text",lv_make(LV_MEMBER,ID(3)),lv_text(&r,"longer live text\rsecond line",false));
  expect(80,80,32,16); // text capture clips to its live sprite plane
  layer.value.member=ID(4);layer.value.x=210;layer.value.y=220;expect(152,144,80,40);
  // Original 8.5 native containers ignore their own blend, while bitmap and
  // Flash layers still disappear at blend0. Crop must match the draw path.
  layer.value.blend=0;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  expect(152,144,80,40);
#else
  expect(0,0,0,0);
#endif
  layer.value.member=ID(7);expect(0,0,0,0);
  layer.value.member=ID(4);layer.value.blend=100;
  layer.value.rotation=4500;layer.value.skew=1700;transformed_coverage();
  layer.value.rotation=layer.value.skew=0;
  layer.asset="synthetic.fdi";layer.value.member=0;layer.value.x=100;layer.value.y=200;
  layer.value.width=31;layer.value.height=17;transformed_coverage();layer.asset=NULL;
  // A real Imaging assignment changes natural dimensions and registration.
  lv_frame_t call_frame={.movie=&code};
  lv_t image=lv_call(&r,&call_frame,"image",3,(lv_t[]){lv_num(50),lv_num(30),lv_num(32)});
  lv_set(&r,NULL,"image",lv_make(LV_MEMBER,ID(2)),image);
  lv_set(&r,NULL,"regpoint",lv_make(LV_MEMBER,ID(2)),lv_point(&r,5,7));
  layer.value.member=ID(2);layer.value.x=115;layer.value.y=127;expect(80,80,40,24);
  // Feed actual resolved overrides, including an unrecorded child channel.
  d.sprites[1]=(dg_sprite_t){.value={.member=ID(1),.type=16,.blend=100},.visible=true};
  dg_films_present(&d,true);
  lv_t child=dg_film_tell(&d,lv_make(LV_SPRITE,1),999);
  lv_set(&r,NULL,"member",child,lv_make(LV_MEMBER,ID(2)));
  lv_set(&r,NULL,"loc",child,lv_point(&r,115,127));
  dg_film_frame_t resolved;
  dg_film_layer_t *owned=dg_film_frame_resolve(&d,1,&d.sprites[1],&members[0],&resolved);
  assert(owned && resolved.count==1);frame=resolved;expect(80,80,40,24);free(owned);
  assert(!r.failed);dg_films_dispose(&d);dg_images_dispose(&d);
  puts("N64 film crop: source origin, clipping, text, nested transforms, mutable images, overrides and unknown providers passed");
}
