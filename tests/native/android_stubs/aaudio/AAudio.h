#include <stdint.h>
typedef struct {int unused;} AAudioStream;
typedef struct {int unused;} AAudioStreamBuilder;
typedef int aaudio_data_callback_result_t;
typedef int aaudio_result_t;
#define AAUDIO_OK 0
#define AAUDIO_FORMAT_PCM_FLOAT 2
#define AAUDIO_USAGE_GAME 14
#define AAUDIO_CONTENT_TYPE_SONIFICATION 4
#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
int AAudio_createStreamBuilder(AAudioStreamBuilder **);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *,int);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *,int);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *,int);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *,int);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *,aaudio_data_callback_result_t (*)(AAudioStream *,void *,void *,int32_t),void *);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *,void (*)(AAudioStream *,void *,aaudio_result_t),void *);
int AAudioStreamBuilder_openStream(AAudioStreamBuilder *,AAudioStream **);
int32_t AAudioStream_getSampleRate(AAudioStream *);
int AAudioStream_requestStart(AAudioStream *);
int AAudioStream_requestStop(AAudioStream *);
int AAudioStream_requestPause(AAudioStream *);
int AAudioStream_close(AAudioStream *);
int AAudioStreamBuilder_delete(AAudioStreamBuilder *);
