#include "flash_audio.h"
enum { WEB_FLASH_VOICES=64 };
static uint32_t flash_voices[WEB_FLASH_VOICES];
static void close_flash_audio(void) {
  for(unsigned i=0;i<WEB_FLASH_VOICES;i++)if(flash_voices[i]&&host.flash_sound)
    host.flash_sound(host.ctx,flash_voices[i],NULL);
  memset(flash_voices,0,sizeof(flash_voices));
}
static void update_flash_audio(void) {
  dg_flash_audio_t requests[WEB_FLASH_VOICES];unsigned count=0;
  if(!dg_flash_audio_collect(&director,requests,WEB_FLASH_VOICES,&count,true,false))return;
  if(count&&!host.flash_sound){lv_fail(director.values,"host Flash event audio service unavailable");return;}
  for(unsigned i=0;i<WEB_FLASH_VOICES;i++)if(flash_voices[i]) {
    bool found=false;for(unsigned n=0;n<count;n++)if(requests[n].id==flash_voices[i])found=true;
    if(!found){host.flash_sound(host.ctx,flash_voices[i],NULL);flash_voices[i]=0;}
  }
  for(unsigned n=0;n<count;n++) {
    unsigned slot=0;while(slot<WEB_FLASH_VOICES&&flash_voices[slot]!=requests[n].id)slot++;
    if(slot==WEB_FLASH_VOICES){slot=0;while(flash_voices[slot])slot++;flash_voices[slot]=requests[n].id;}
    host.flash_sound(host.ctx,requests[n].id,&requests[n]);
  }
}
