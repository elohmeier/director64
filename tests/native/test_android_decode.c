// Source-free MediaCodec contract: partial PCM is never mistaken for a
// complete sound. The same fixture runs on the host and Android ARMv7 libc.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool allocation_fails;
static void *test_realloc(void *p,size_t bytes) { return allocation_fails ? NULL : realloc(p,bytes); }
#define realloc test_realloc
#include "../../platforms/android/jni/decode.c"
#undef realloc
static AMediaCodec codec;
static AMediaExtractor extractor;
static AMediaFormat format;
static int mode,output_count,released,deleted;
static bool input_done;
static int16_t pcm[]={-32768,-1,0,32767};
static float floating[]={-1,0,1,.5f};
enum { COMPLETE, TIMEOUT, FLOAT_COMPLETE, BAD_BUFFER, INPUT_ERROR };
int __android_log_print(int p,const char *tag,const char *fmt,...) {(void)p;(void)tag;(void)fmt;return 0;}
AMediaExtractor *AMediaExtractor_new(void) {return &extractor;}
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *e,int fd,off64_t o,off64_t n) {(void)e;(void)fd;(void)o;(void)n;return 0;}
size_t AMediaExtractor_getTrackCount(AMediaExtractor *e) {(void)e;return 1;}
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *e,size_t t) {(void)e;(void)t;return &format;}
media_status_t AMediaExtractor_selectTrack(AMediaExtractor *e,size_t t) {(void)e;(void)t;return 0;}
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *e,uint8_t *p,size_t n) {(void)e;(void)p;(void)n;return -1;}
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *e) {(void)e;return 0;}
bool AMediaExtractor_advance(AMediaExtractor *e) {(void)e;return false;}
media_status_t AMediaExtractor_delete(AMediaExtractor *e) {(void)e;deleted++;return 0;}
AMediaCodec *AMediaCodec_createDecoderByType(const char *s) {(void)s;return &codec;}
media_status_t AMediaCodec_configure(AMediaCodec *c,const AMediaFormat *f,void *w,void *crypto,uint32_t flags) {(void)c;(void)f;(void)w;(void)crypto;(void)flags;return 0;}
media_status_t AMediaCodec_start(AMediaCodec *c) {(void)c;return 0;}
media_status_t AMediaCodec_stop(AMediaCodec *c) {(void)c;return 0;}
media_status_t AMediaCodec_delete(AMediaCodec *c) {(void)c;deleted++;return 0;}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c,int64_t t) {(void)c;(void)t;return input_done?-1:0;}
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c,size_t i,size_t *n) {(void)c;(void)i;*n=sizeof(pcm);return (uint8_t *)pcm;}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c,size_t i,off_t o,size_t n,uint64_t t,uint32_t flags) {(void)c;(void)i;(void)o;(void)n;(void)t;(void)flags;input_done=true;return mode==INPUT_ERROR?-1:0;}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c,AMediaCodecBufferInfo *i,int64_t t) {
  (void)c;(void)t;
  if(output_count++==0)return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
  if(mode==TIMEOUT&&output_count>2)return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
  *i=(AMediaCodecBufferInfo){.size=mode==FLOAT_COMPLETE?sizeof(floating):sizeof(pcm),
    .flags=mode==TIMEOUT?0:AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM};
  if(mode==BAD_BUFFER)i->offset=100;
  return 0;
}
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c) {(void)c;return &format;}
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *c,size_t i,size_t *n) {(void)c;(void)i;*n=mode==FLOAT_COMPLETE?sizeof(floating):sizeof(pcm);return mode==FLOAT_COMPLETE?(uint8_t *)floating:(uint8_t *)pcm;}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c,size_t i,bool render) {(void)c;(void)i;(void)render;released++;return 0;}
bool AMediaFormat_getString(AMediaFormat *f,const char *key,const char **out) {(void)f;(void)key;*out="audio/opus";return true;}
bool AMediaFormat_getInt32(AMediaFormat *f,const char *key,int32_t *out) {(void)f;*out=!strcmp(key,"channels")?2:!strcmp(key,"rate")?22050:mode==FLOAT_COMPLETE?4:2;return true;}
media_status_t AMediaFormat_delete(AMediaFormat *f) {(void)f;deleted++;return 0;}
static void write16(uint8_t *p,unsigned n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void write32(uint8_t *p,unsigned n) {write16(p,n);write16(p+2,n>>16);}
static void test_pcm(unsigned bits) {
  unsigned samples=bits==8?256:65536, bytes=samples*(bits/8);
  uint8_t *wav=calloc(1,44+bytes);assert(wav);
  memcpy(wav,"RIFF",4);write32(wav+4,36+bytes);memcpy(wav+8,"WAVEfmt ",8);
  write32(wav+16,16);write16(wav+20,1);write16(wav+22,2);write32(wav+24,22050);
  write16(wav+34,bits);memcpy(wav+36,"data",4);write32(wav+40,bytes);
  for(unsigned i=0;i<samples;i++)if(bits==8)wav[44+i]=(uint8_t)i;else write16(wav+44+i*2,i);
  sound_t *s=decode_wav(wav,44+bytes);assert(s&&s->frames==samples/2&&s->channels==2);
  for(unsigned i=0;i<samples;i++)assert(s->pcm[i]==(bits==8?((int)i-128)*256:(int16_t)i));
  assert(sound_bytes(s)==samples*2);sound_release(s);free(wav);
}
int main(void) {
  test_pcm(8);test_pcm(16);
  for(int i=0;i<6;i++) {
    mode=i==5?COMPLETE:i;allocation_fails=i==5;
    output_count=released=deleted=0;input_done=false;
    char error[160];sound_t *s=decode_media(3,0,100,error,sizeof(error));
    if(mode==COMPLETE&&!allocation_fails) {assert(s&&!error[0]&&s->frames==2&&!memcmp(s->pcm,pcm,sizeof(pcm)));}
    else if(mode==FLOAT_COMPLETE) {assert(s&&!error[0]&&s->frames==2&&s->pcm[0]==-32767&&s->pcm[2]==32767);}
    else {assert(!s&&error[0]);assert(strstr(error,allocation_fails?"memory":mode==TIMEOUT?"end of stream":mode==INPUT_ERROR?"input packet":"invalid PCM"));}
    assert(deleted==3+(mode!=INPUT_ERROR));
    if(mode!=INPUT_ERROR)assert(released==1);
    sound_release(s);
  }
  puts("Android decode: exhaustive PCM8/16, EOS, float, timeout, OOM and invalid output passed");
}
