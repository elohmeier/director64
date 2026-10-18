#ifndef DIRECTOR64_FLASH_AUDIO_H
#define DIRECTOR64_FLASH_AUDIO_H
#include "flash_runtime.h"
#include <math.h>
#if DG_D10
// SOUNDENVELOPE positions use 44,100-Hz samples even when the sound has a
// different rate (Adobe SWF19, p181). Mono averages the two authored levels.
static inline void dg_flash_envelope(const dg_flash_sound_event_t *event,
    double source_sample, unsigned rate, bool mono, float *left, float *right) {
  *left=*right=1;
  if(!event || !(event->flags&8) || !event->envelope_count || !rate)return;
  const dg_flash_envelope_t *p=event->envelopes;
  double position=source_sample*44100.0/rate;
  unsigned n=0;
  while(n+1<event->envelope_count && p[n+1].position<=position)n++;
  double l=p[n].left,r=p[n].right;
  if(n+1<event->envelope_count && position>p[n].position) {
    double f=(position-p[n].position)/(p[n+1].position-p[n].position);
    l+=(p[n+1].left-l)*f;r+=(p[n+1].right-r)*f;
  }
  if(mono)l=r=(l+r)*0.5;
  *left=(float)(l/32768.0);*right=(float)(r/32768.0);
}

// Mix a finite event into a floating stereo output. `position` is unfolded
// decoded frames; decoding/resampling never changes the source sample grid.
static inline bool dg_flash_mix_pcm(const int16_t *pcm,unsigned frames,unsigned channels,
    unsigned decoded_rate,unsigned source_rate,unsigned start,unsigned end,unsigned loops,
    const dg_flash_sound_event_t *event,double *position,float *output,unsigned count,
    unsigned output_rate,float gain) {
  if(!pcm||!frames||!channels||channels>2||!decoded_rate||!source_rate||!output_rate||end<=start)return false;
  double scale=(double)decoded_rate/source_rate,span=(end-start)*scale;
  double step=(double)decoded_rate/output_rate;
  for(unsigned n=0;n<count;n++) {
    if(*position>=span*loops)return false;
    double sample=start*scale+fmod(*position,span);
    unsigned at=(unsigned)sample;if(at>=frames)return false;
    unsigned next=at+1<frames?at+1:at;
    if(next>=end*scale)next=floor(*position/span)+1<loops?(unsigned)(start*scale):at;
    float fraction=(float)(sample-at);
    float left=pcm[at*channels]+(pcm[next*channels]-pcm[at*channels])*fraction;
    float right=channels==2?pcm[at*2+1]+(pcm[next*2+1]-pcm[at*2+1])*fraction:left;
    float lg,rg;dg_flash_envelope(event,sample/scale,source_rate,channels==1,&lg,&rg);
    output[n*2]+=left*(gain/32768.0f)*lg;output[n*2+1]+=right*(gain/32768.0f)*rg;
    *position+=step;
  }
  return *position<span*loops;
}

// Take a complete snapshot before touching hosts: disposing a linked child
// must not leave a host holding its runtime pointer. Missing IDs mean stop.
static inline bool dg_flash_audio_collect(dg_runtime_t *d,dg_flash_audio_t *out,
    unsigned capacity,unsigned *count,bool sound,bool paused) {
  unsigned voices=dg_flash_audio_count(d);
  for(unsigned n=0;n<voices;n++) {
    if(*count==capacity){lv_fail(d->values,"concurrent Flash event sound capacity exceeded");return false;}
    dg_flash_audio_t *v=&out[(*count)++];
    if(!dg_flash_audio_at(d,n,v)){lv_fail(d->values,"Flash event sound snapshot changed");return false;}
    v->paused|=paused;
    v->gain*=sound?(d->sound_level<0?0:d->sound_level>7?1:d->sound_level/7.0f):0;
  }
  for(unsigned ch=1;ch<DG_SPRITES;ch++)if(dg_linked_movie(d,ch)) {
    const dg_member_t *m=dg_member(d,d->sprites[ch].value.member);
    dg_runtime_t *child=dg_linked_begin(d,ch);
    bool ok=child&&dg_flash_audio_collect(child,out,capacity,count,sound&&m&&(m->linked_flags&8),
        paused||d->sprites[ch].film_paused||child->linked_paused);
    dg_linked_end(d);if(!ok)return false;
  }
  return true;
}
#endif
#endif
