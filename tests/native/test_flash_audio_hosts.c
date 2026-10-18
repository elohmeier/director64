#include "web_runtime.h"
#include "flash_audio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t director,child;
static lv_runtime_t values;
static web_host_t host;
static dg_flash_audio_t root_voice,child_voice;
static bool root_alive=true,child_alive=true,linked=true;
static dg_member_t link={.linked_flags=8};
void lv_fail(lv_runtime_t *v,const char *error){(void)error;v->failed=true;}
unsigned dg_flash_audio_count(dg_runtime_t *d){return d==&director?root_alive:child_alive;}
bool dg_flash_audio_at(dg_runtime_t *d,unsigned index,dg_flash_audio_t *out) {
  if(index||!dg_flash_audio_count(d))return false;
  *out=d==&director?root_voice:child_voice;return true;
}
const dg_runtime_t *dg_linked_movie(const dg_runtime_t *d,unsigned ch){return linked&&d==&director&&ch==1?&child:NULL;}
dg_runtime_t *dg_linked_begin(dg_runtime_t *d,unsigned ch){(void)d;(void)ch;return &child;}
void dg_linked_end(dg_runtime_t *d){(void)d;}
const dg_member_t *dg_member(dg_runtime_t *d,uint32_t id){(void)d;(void)id;return &link;}
#include "../../platforms/web/flash_audio.inc"
static unsigned updates,stops;static dg_flash_audio_t received[3];
static void update(void *ctx,uint32_t id,const dg_flash_audio_t *v){(void)ctx;assert(id<3);
  if(v){received[id]=*v;updates++;}else stops++;}
int main(void) {
  director.values=child.values=&values;director.sound_level=7;child.sound_level=3;
  root_voice=(dg_flash_audio_t){.id=1,.gain=1,.playing=true};child_voice=(dg_flash_audio_t){.id=2,.gain=1,.playing=true};
  host.flash_sound=update;update_flash_audio();assert(updates==2&&stops==0);
  assert(received[1].gain==1&&fabs(received[2].gain-3/7.0f)<1e-6);
  link.linked_flags=0;director.sprites[1].film_paused=true;update_flash_audio();
  assert(received[2].gain==0&&received[2].paused);assert(!received[1].paused);
  linked=false;update_flash_audio();assert(stops==1); // child disposed: no stale owner pointer
  root_alive=false;update_flash_audio();assert(stops==2);
  close_flash_audio();assert(stops==2);
  root_alive=true;host.flash_sound=NULL;update_flash_audio();assert(values.failed);
  dg_flash_envelope_t points[]={{0,0,32768},{44100,32768,0}};
  dg_flash_sound_event_t event={.flags=8,.envelope_count=2,.envelopes=points};float l,r;
  dg_flash_envelope(&event,1102.5,11025,false,&l,&r);assert(fabs(l-.1)<1e-6&&fabs(r-.9)<1e-6);
  dg_flash_envelope(&event,1102.5,11025,true,&l,&r);assert(l==.5&&r==.5);
  // Six source frames, trimmed to [1,4), repeated twice. The last output
  // remains untouched after the finite voice ends, including chunked calls.
  const int16_t pcm[]={100,100,10000,-10000,20000,-20000,30000,-30000,400,400,500,500};
  const dg_flash_envelope_t ramp[]={{0,0,32768},{12,32768,0}};
  const dg_flash_sound_event_t shaped={.flags=8,.envelope_count=2,.envelopes=ramp};
  float output[16]={0};double position=0;
  assert(dg_flash_mix_pcm(pcm,6,2,11025,11025,1,4,2,&shaped,&position,output,2,11025,1));
  assert(!dg_flash_mix_pcm(pcm,6,2,11025,11025,1,4,2,&shaped,&position,output+4,6,11025,1));
  for(unsigned n=0;n<6;n++) {
    unsigned at=1+n%3;double gain=at/3.0;
    assert(fabs(output[n*2]-pcm[at*2]/32768.0*gain)<1e-6);
    assert(fabs(output[n*2+1]-pcm[at*2+1]/32768.0*(1-gain))<1e-6);
  }
  assert(position==6&&output[12]==0&&output[15]==0);
  // Output resampling advances fractional source positions while the
  // envelope remains in source-time coordinates, independently of voices.
  memset(output,0,sizeof(output));position=0;
  assert(dg_flash_mix_pcm(pcm,6,2,11025,11025,1,4,2,NULL,&position,output,2,22050,.5));
  assert(position==1&&fabs(output[2]-15000/65536.0)<1e-6);
  puts("Flash audio hosts: independent IDs, linked mute/pause/removal, missing service and sample envelopes passed");
}
