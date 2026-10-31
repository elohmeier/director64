// Exercise the player-facing controller API, including physical-pointer
// handoff into editable-field entry. No game package or retail assets.
#include "../../platforms/web/web_runtime.c"
#include <assert.h>
static const lv_movie_t input_code = {.name="INPUT.DXR"};
static const dg_cast_t input_cast = {.name="Internal",.file=1,.cast=1};
static const dg_frame_t input_frame = {0};
static const dg_member_t input_member = {.id=DG_ID(1,1,1),.cast=1,.number=1,
 .type=3,.text="Name",.width=40,.height=14,.editable=true};
static const dg_movie_t input_movie = {.id=1,.code=&input_code,.tempo=30,
 .cast_count=1,.casts=&input_cast,.member_count=1,.members=&input_member,
 .frame_count=1,.frames=&input_frame};
int main(void) {
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&input_movie,1,NULL,0));
  booted=true; input_init(&pointer);
  d64_pad(1,INPUT_A,0,0);
  assert(d64_step_pad()==0 && pointer.pressed==INPUT_A);
  // While the host uses a physical pointer, it feeds release samples but
  // does not run controller ticks. Returning with a new A must still open
  // the editable field rather than delivering a stale held button to it.
  d64_pad(1,0,0,0);
  director.sprites[1]=(dg_sprite_t){.value={.member=input_member.id,.type=16,
    .x=50,.y=60,.width=40,.height=14},.visible=true,.stretch=true};
  dg_sprite_changed(&director,1);
  d64_pointer_warp(55,65);
  assert(d64_pointer_x()==55 && d64_pointer_y()==65);
  assert(d64_text_field(55,65)==1);
  unsigned before=director.ticks;
  d64_pad(1,INPUT_A,0,0);
  assert(d64_step_pad()==3 && pointer.pressed==INPUT_A);
  assert(director.ticks==before); // Host entry holds the game before this tick.
  d64_pad(0,INPUT_A,127,-128);
  assert(d64_step_pad()==0 && !pointer.held && pointer.released==INPUT_A);
  assert(d64_pointer_x()==55 && d64_pointer_y()==65);
  assert(!values.failed);
  puts("web single input: public API, pointer handoff, fresh field-entry edge and disconnect passed");
}
