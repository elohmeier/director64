// Source-free coverage/provider tests for the N64 native-film fast path.
// The cache mock supplies decoded pixels; classification is the target code.
#include "film_transform.h"
#include "ink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ID(n) DG_ID(1,1,n)
enum { IMAGE_RGBA16, IMAGE_RGBA32 };
typedef struct {
  void *data;
  unsigned format,alpha_offset;
  uint8_t coverage_flags;
} cached_image_t;
static uint8_t source_data[40],mask_data[40];
static cached_image_t source={.data=source_data,.format=IMAGE_RGBA32};
static cached_image_t mask={.data=mask_data,.format=IMAGE_RGBA32};
static cached_image_t *image(const dg_member_t *member,unsigned ink) {
  (void)ink;
  return member->asset&&!strcmp(member->asset,"source")?&source:
      member->asset&&!strcmp(member->asset,"mask")?&mask:NULL;
}
static uint32_t image_pixel(const cached_image_t *image,unsigned width,unsigned height,
                            unsigned x,unsigned y) {
  assert(x<width&&y<height);
  const uint8_t *p=(const uint8_t *)image->data+32+(y*width+x)*4;
  return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static bool mask_pair(const dg_member_t *a,const dg_member_t *b) {
  return dg_mask_pair(a->name,b->name,(int)b->width-a->width,(int)b->height-a->height);
}
#include "film_backdrop.inc"
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code={.name="FILM-BACKDROP.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t score={0};
static dg_film_layer_t leaf={.channel=1,.value={.member=ID(1),.type=16,.blend=100,
    .ink=9,.thickness=DG_HAS_BLEND}};
static const dg_film_frame_t frame={1,&leaf};
static const dg_film_layer_t nested={.channel=1,.value={.member=ID(3),.type=16,
    .blend=0,.ink=39,.thickness=DG_HAS_BLEND}};
static const dg_film_frame_t outer_frame={1,&nested};
static const dg_member_t members[]={
 {.id=ID(1),.number=1,.cast=1,.type=1,.width=2,.height=1,.name="source",.asset="source"},
 {.id=ID(2),.number=2,.cast=1,.type=1,.width=2,.height=1,.name="mask",.asset="mask"},
 {.id=ID(3),.number=3,.cast=1,.type=2,.width=2,.height=1,.film_count=1,.film_frames=&frame},
 {.id=ID(4),.number=4,.cast=1,.type=2,.width=2,.height=1,.film_count=1,.film_frames=&outer_frame}};
static const dg_movie_t movie={.id=1,.code=&code,.cast_count=1,.casts=&cast,
 .member_count=4,.members=members,.frame_count=1,.frames=&score};
static bool safe(const dg_film_frame_t *test) {
  return dg_film_isolated_safe(&d,1,&members[2],test,film_leaf_safe,NULL,0);
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
  source_data[9]=mask_data[9]=2;source_data[11]=mask_data[11]=1;
  for(unsigned i=0;i<2;i++) {
    source_data[32+i*4]=17;source_data[33+i*4]=199;source_data[34+i*4]=83;
    source_data[35+i*4]=mask_data[35+i*4]=255;
  }
  mask_data[36]=mask_data[37]=mask_data[38]=255;
  assert(safe(&frame)); // Black/white mask is valid independent coverage.
  assert(source.coverage_flags&&mask.coverage_flags);
  leaf.value.ink=39;assert(!safe(&frame));
  leaf.value.ink=37;assert(!safe(&frame));
  leaf.value.ink=8;leaf.value.blend=15;assert(!safe(&frame));
  leaf.value.blend=100;source_data[35]=128;source.coverage_flags=0;
  assert(!safe(&frame)); // Fractional source alpha needs target rounding.
  source_data[35]=255;source.coverage_flags=0;assert(safe(&frame));
  leaf.value.ink=9;mask_data[32]=mask_data[33]=mask_data[34]=128;mask.coverage_flags=0;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  assert(!safe(&frame)); // Soft masks use the actual stage destination.
#else
  assert(safe(&frame));
#endif
  mask_data[32]=mask_data[33]=mask_data[34]=0;mask.coverage_flags=0;
  assert(safe(&frame));
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  assert(safe(&outer_frame)); // Parent's blend0/ink39 do not affect leaves.
  leaf.value.ink=39;assert(!safe(&outer_frame));leaf.value.ink=9;
#endif
  lv_frame_t call={.movie=&code};
  lv_t replacement=lv_call(&r,&call,"image",3,(lv_t[]){lv_num(2),lv_num(1),lv_num(32)});
  lv_set(&r,NULL,"image",lv_make(LV_MEMBER,ID(1)),replacement);
  assert(!safe(&frame)); // Mutable providers cannot reuse immutable facts.
  assert(!r.failed);dg_films_dispose(&d);dg_images_dispose(&d);
  puts("N64 film isolation: binary source/mask, soft coverage, arithmetic, nested films and mutable providers passed");
}
