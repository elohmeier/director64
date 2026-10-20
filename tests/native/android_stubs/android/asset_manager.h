#ifndef DIRECTOR64_TEST_ASSET_MANAGER_H
#define DIRECTOR64_TEST_ASSET_MANAGER_H
#include <stddef.h>
#include <stdint.h>

typedef struct AAssetManager AAssetManager;

typedef struct AAsset AAsset;

enum { AASSET_MODE_UNKNOWN, AASSET_MODE_RANDOM, AASSET_MODE_STREAMING, AASSET_MODE_BUFFER };
AAsset *AAssetManager_open(AAssetManager *, const char *, int);
int64_t AAsset_getLength64(AAsset *);
int AAsset_read(AAsset *, void *, size_t);
void AAsset_close(AAsset *);
int AAsset_openFileDescriptor64(AAsset *, int64_t *, int64_t *);
const void *AAsset_getBuffer(AAsset *);
#endif
