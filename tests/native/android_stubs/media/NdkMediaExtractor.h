#ifndef TEST_ANDROID_EXTRACTOR_H
#define TEST_ANDROID_EXTRACTOR_H
#include "NdkMediaCodec.h"
typedef int64_t off64_t;
typedef struct { int unused; } AMediaExtractor;
AMediaExtractor *AMediaExtractor_new(void);
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *,int,off64_t,off64_t);
size_t AMediaExtractor_getTrackCount(AMediaExtractor *);
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *,size_t);
media_status_t AMediaExtractor_selectTrack(AMediaExtractor *,size_t);
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *,uint8_t *,size_t);
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *);
bool AMediaExtractor_advance(AMediaExtractor *);
media_status_t AMediaExtractor_delete(AMediaExtractor *);
#endif
