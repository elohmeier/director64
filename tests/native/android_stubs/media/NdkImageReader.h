#include <media/NdkMediaCodec.h>
typedef struct {int unused;} AImageReader;
typedef struct {int unused;} AImage;
typedef struct {int unused;} ANativeWindow;
typedef struct {int32_t left,top,right,bottom;} AImageCropRect;
#define AIMAGE_FORMAT_YUV_420_888 35
#define AMEDIAFORMAT_KEY_WIDTH "width"
#define AMEDIAFORMAT_KEY_HEIGHT "height"
media_status_t AImageReader_new(int32_t,int32_t,int32_t,int32_t,AImageReader **);
media_status_t AImageReader_getWindow(AImageReader *,ANativeWindow **);
void AImageReader_delete(AImageReader *);
media_status_t AImageReader_acquireLatestImage(AImageReader *,AImage **);
media_status_t AImage_getWidth(AImage *,int32_t *);
media_status_t AImage_getHeight(AImage *,int32_t *);
media_status_t AImage_getCropRect(AImage *,AImageCropRect *);
media_status_t AImage_getPlaneData(AImage *,int,uint8_t **,int *);
media_status_t AImage_getPlaneRowStride(AImage *,int,int *);
media_status_t AImage_getPlanePixelStride(AImage *,int,int *);
void AImage_delete(AImage *);
AMediaFormat *AMediaFormat_new(void);
void AMediaFormat_setString(AMediaFormat *,const char *,const char *);
void AMediaFormat_setInt32(AMediaFormat *,const char *,int32_t);
media_status_t AMediaCodec_flush(AMediaCodec *);
#define AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE (-30001)
media_status_t AImage_getTimestamp(AImage *,int64_t *);
