#ifndef TEST_ANDROID_CODEC_H
#define TEST_ANDROID_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef int media_status_t;
typedef struct { int unused; } AMediaCodec;
typedef struct { int unused; } AMediaFormat;
typedef struct { int32_t offset,size; int64_t presentationTimeUs; uint32_t flags; } AMediaCodecBufferInfo;
#define AMEDIA_OK 0
#define AMEDIACODEC_INFO_TRY_AGAIN_LATER (-1)
#define AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED (-2)
#define AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED (-3)
#define AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM 4
#define AMEDIAFORMAT_KEY_MIME "mime"
#define AMEDIAFORMAT_KEY_CHANNEL_COUNT "channels"
#define AMEDIAFORMAT_KEY_SAMPLE_RATE "rate"
AMediaCodec *AMediaCodec_createDecoderByType(const char *);
media_status_t AMediaCodec_configure(AMediaCodec *,const AMediaFormat *,void *,void *,uint32_t);
media_status_t AMediaCodec_start(AMediaCodec *);
media_status_t AMediaCodec_stop(AMediaCodec *);
media_status_t AMediaCodec_delete(AMediaCodec *);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *,int64_t);
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *,size_t,size_t *);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *,size_t,off_t,size_t,uint64_t,uint32_t);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *,AMediaCodecBufferInfo *,int64_t);
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *);
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *,size_t,size_t *);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *,size_t,bool);
bool AMediaFormat_getString(AMediaFormat *,const char *,const char **);
bool AMediaFormat_getInt32(AMediaFormat *,const char *,int32_t *);
media_status_t AMediaFormat_delete(AMediaFormat *);
#endif
