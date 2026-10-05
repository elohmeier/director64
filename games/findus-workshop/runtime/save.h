#ifndef DIRECTOR64_SAVE_H
#define DIRECTOR64_SAVE_H
#include "storage.h"
#include <stddef.h>
#include <stdint.h>
#define SAVE_PROFILES 7
#define SAVE_SNAPSHOT_BYTES 512
#define SAVE_SLOT_STRIDE 65536

typedef struct {
    uint32_t feathers, coins;
    uint8_t treasure[35], map[9];
} save_profile_t;
typedef struct { save_profile_t profiles[SAVE_PROFILES]; } save_model_t;
typedef struct {
    save_backend_t backend;
    save_model_t model;
    save_status_t status;
    uint32_t generation;
    int active_slot;
} save_store_t;

save_status_t save_load(save_store_t *store, save_backend_t backend);
bool save_commit(save_store_t *store, const save_model_t *model);
bool save_add_feather(save_store_t *store, unsigned profile);
#endif
