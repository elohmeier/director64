#include "save.h"
#include <limits.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t crc(const uint8_t *data) {
    uint32_t value = UINT32_MAX;
    for (unsigned i = 0; i < SAVE_SNAPSHOT_BYTES; i++) {
        value ^= i >= 16 && i < 20 ? 0 : data[i];
        for (unsigned bit = 0; bit < 8; bit++) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return value ^ UINT32_MAX;
}
static bool decode(const uint8_t *data, save_model_t *model, uint32_t *generation) {
    if (memcmp(data, "F64S", 4) || read32(data + 4) != 1 || !read32(data + 8) ||
        read32(data + 12) != 364 || read32(data + 16) != crc(data) || read32(data + 20) != 0x434f4d54)
        return false;
    for (unsigned i = 24; i < 32; i++) if (data[i]) return false;
    for (unsigned i = 396; i < SAVE_SNAPSHOT_BYTES; i++) if (data[i]) return false;
    *model = (save_model_t){0};
    for (unsigned i = 0; i < SAVE_PROFILES; i++) {
        const uint8_t *p = data + 32 + i * 52;
        model->profiles[i].feathers = read32(p); model->profiles[i].coins = read32(p + 4);
        memcpy(model->profiles[i].treasure, p + 8, 35); memcpy(model->profiles[i].map, p + 43, 9);
        for (unsigned j = 0; j < 9; j++) if (model->profiles[i].map[j] > 1) return false;
        for (unsigned j = 0; j < 35; j++) if (model->profiles[i].treasure[j] > 33) return false;
    }
    *generation = read32(data + 8);
    return true;
}
static void encode(uint8_t *data, const save_model_t *model, uint32_t generation) {
    memset(data, 0, SAVE_SNAPSHOT_BYTES);
    memcpy(data, "F64S", 4); write32(data + 4, 1); write32(data + 8, generation);
    write32(data + 12, 364); write32(data + 20, 0x434f4d54);
    for (unsigned i = 0; i < SAVE_PROFILES; i++) {
        uint8_t *p = data + 32 + i * 52;
        write32(p, model->profiles[i].feathers); write32(p + 4, model->profiles[i].coins);
        memcpy(p + 8, model->profiles[i].treasure, 35); memcpy(p + 43, model->profiles[i].map, 9);
    }
    write32(data + 16, crc(data));
}

save_status_t save_load(save_store_t *store, save_backend_t backend) {
    *store = (save_store_t){.backend = backend, .active_slot = -1, .status = SAVE_IO_ERROR};
    if (!backend.read || !backend.write) return store->status;
    bool blank = true;
    for (unsigned slot = 0; slot < 2; slot++) {
        uint8_t data[SAVE_SNAPSHOT_BYTES];
        save_model_t model; uint32_t generation;
        if (!backend.read(backend.context, slot * SAVE_SLOT_STRIDE, data, sizeof(data))) return store->status = SAVE_IO_ERROR;
        bool erased = true, zero = true;
        for (unsigned i = 0; i < sizeof(data); i++) { erased &= data[i] == 255; zero &= data[i] == 0; }
        blank &= erased || zero;
        if (decode(data, &model, &generation) && (store->active_slot < 0 || generation > store->generation)) {
            store->model = model; store->generation = generation; store->active_slot = (int)slot;
        }
    }
    return store->status = store->active_slot >= 0 ? SAVE_VALID : blank ? SAVE_BLANK : SAVE_CORRUPT;
}

bool save_commit(save_store_t *store, const save_model_t *model) {
    if (!store || !model || (store->status != SAVE_VALID && store->status != SAVE_BLANK)) return false;
    if (store->generation == UINT32_MAX) { store->status = SAVE_FULL; return false; }
    uint8_t data[SAVE_SNAPSHOT_BYTES], verified[SAVE_SNAPSHOT_BYTES];
    save_model_t checked; uint32_t generation;
    encode(data, model, store->generation + 1);
    if (!decode(data, &checked, &generation)) return false;
    unsigned slot = store->active_slot == 0 ? 1 : 0;
    if (!store->backend.write(store->backend.context, slot * SAVE_SLOT_STRIDE, data, sizeof(data)) ||
        !store->backend.read(store->backend.context, slot * SAVE_SLOT_STRIDE, verified, sizeof(verified)) ||
        memcmp(data, verified, sizeof(data))) { store->status = SAVE_IO_ERROR; return false; }
    store->active_slot = (int)slot; store->generation++; store->model = *model; store->status = SAVE_VALID;
    return true;
}

bool save_add_feather(save_store_t *store, unsigned profile) {
    if (!store || profile >= SAVE_PROFILES || store->model.profiles[profile].feathers == UINT32_MAX) return false;
    save_model_t model = store->model;
    model.profiles[profile].feathers++;
    return save_commit(store, &model);
}
