#include "flash_audio.h"
#include "../../runtime/director/flash_audio.h"
#include <libdragon.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Each voice streams exact source PCM through its own tiny ring. The wrapper
// emits one finite virtual waveform, so in/out boundaries, repeated passes
// and stereo envelopes are processed per sample rather than once per tick.
typedef struct {
  uint32_t id,start,end,rate;
  uint64_t duration;
  wav64_t source;
  waveform_t wave;
  samplebuffer_t input;
  dg_flash_sound_event_t event;
  dg_flash_envelope_t envelopes[255];
  bool opened,paused;
} flash_audio_slot_t;
static flash_audio_slot_t slots[FLASH_AUDIO_SLOTS];
static unsigned channel(unsigned slot){return D5_MIXER_CHANNELS+slot*2;}
static void close_slot(unsigned slot) {
  flash_audio_slot_t *s=&slots[slot];if(!s->id)return;
  mixer_ch_stop(channel(slot));rspq_wait();
  if(samplebuffer_is_inited(&s->input))samplebuffer_close(&s->input);
  if(s->opened)wav64_close(&s->source);
  memset(s,0,sizeof(*s));
}
void flash_audio_close(void){for(unsigned n=0;n<FLASH_AUDIO_SLOTS;n++)close_slot(n);}
static void read_samples(void *ctx,samplebuffer_t *output,int position,int length,bool seeking) {
  (void)seeking;flash_audio_slot_t *s=ctx;
  while(length>0) {
    int count=length>128?128:length;
    bool silent=(uint64_t)position>=s->duration;
    unsigned at=s->start+(unsigned)((uint64_t)position%(s->end-s->start));
    if(!silent && count>(int)(s->end-at))count=(int)(s->end-at);
    if(!silent && (uint64_t)count>s->duration-position)count=(int)(s->duration-position);
    const void *source=NULL;
    if(!silent)source=samplebuffer_get(&s->input,(int)at,&count);
    int16_t *out=samplebuffer_append(output,count);
    for(int n=0;n<count;n++) {
      if(silent){out[n*2]=out[n*2+1]=0;continue;}
      unsigned channels=s->source.wave.channels;
      int left=s->source.wave.bits==16?((const int16_t *)source)[n*channels]:((const int8_t *)source)[n*channels]*256;
      int right=channels==1?left:s->source.wave.bits==16?((const int16_t *)source)[n*channels+1]:((const int8_t *)source)[n*channels+1]*256;
      float lg,rg;dg_flash_envelope(&s->event,at+n,s->rate,channels==1,&lg,&rg);
      out[n*2]=(int16_t)lrintf(left*lg);out[n*2+1]=(int16_t)lrintf(right*rg);
    }
    position+=count;length-=count;
  }
}
void flash_audio_update(dg_runtime_t *d,float volume) {
  dg_flash_audio_t requests[FLASH_AUDIO_SLOTS];unsigned count=0;
  if(!dg_flash_audio_collect(d,requests,FLASH_AUDIO_SLOTS,&count,true,false))return;
  for(unsigned n=0;n<FLASH_AUDIO_SLOTS;n++)if(slots[n].id) {
    bool found=false;for(unsigned k=0;k<count;k++)if(requests[k].id==slots[n].id)found=true;
    if(!found)close_slot(n);
  }
  for(unsigned n=0;n<count;n++) {
    dg_flash_audio_t *v=&requests[n];unsigned slot=0;
    while(slot<FLASH_AUDIO_SLOTS&&slots[slot].id!=v->id)slot++;
    bool fresh=slot==FLASH_AUDIO_SLOTS;
    if(fresh){slot=0;while(slots[slot].id)slot++;}
    flash_audio_slot_t *s=&slots[slot];unsigned ch=channel(slot);
    if(fresh) {
      s->id=v->id;s->start=v->start_sample;s->end=v->end_sample;s->rate=v->sound->rate;
      s->duration=(uint64_t)(s->end-s->start)*v->loops;
      if(s->duration>WAVEFORM_MAX_LEN){lv_fail(d->values,"Flash event sound duration exceeds console waveform capacity");close_slot(slot);return;}
      char path[192];snprintf(path,sizeof(path),"rom:/audio/%s",v->sound->asset);wav64_open(&s->source,path);s->opened=true;
      waveform_t *input=&s->source.wave;
      if(input->format!=WAVEFORM_FORMAT_PCM || input->frequency!=s->rate || input->len<(int)v->sound->samples ||
          input->rsp_written || input->channels<1 || input->channels>2 || (input->bits!=8&&input->bits!=16)) {
        lv_fail(d->values,"Flash event audio requires exact uncompressed source PCM");close_slot(slot);return;
      }
      if(v->event)s->event=*v->event;
      if(s->event.envelope_count)memcpy(s->envelopes,s->event.envelopes,s->event.envelope_count*sizeof(*s->envelopes));
      s->event.envelopes=s->envelopes;
      int size=4096, state=(input->state_size+15)&~15;
      void *memory=malloc_uncached(size+state);
      if(!memory){lv_fail(d->values,"Flash event audio buffer allocation failed");close_slot(slot);return;}
      samplebuffer_init(&s->input,memory,size,state);samplebuffer_set_bps(&s->input,input->bits*input->channels);
      samplebuffer_set_waveform(&s->input,input,input->read);if(input->start)input->start(input->ctx,&s->input);
      s->wave=(waveform_t){.name="Flash event",.bits=16,.channels=2,.frequency=s->rate,
          .len=(int)s->duration,.read=read_samples,.ctx=s};
      mixer_ch_play(ch,&s->wave);mixer_ch_set_pos(ch,(double)v->position);
    }
    s->paused=v->paused;mixer_ch_set_freq(ch,s->paused?0:s->rate);
    float gain=volume*v->gain;mixer_ch_set_vol(ch,gain,gain);
  }
}
