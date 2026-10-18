#ifndef DIRECTOR64_FLASH_RUNTIME_H
#define DIRECTOR64_FLASH_RUNTIME_H
#include "avm1.h"
#include "director.h"
#if DG_D10
typedef struct dg_flash_state dg_flash_state_t;
typedef struct dg_flash_instance dg_flash_instance_t;
typedef struct {
  const dg_member_t *member;
  const dg_film_frame_t *frame;
  const dg_flash_field_t *field;
  lv_t object;
  unsigned sprite, frame_number, frame_count;
  uint32_t id;
  float matrix[6], bounds[4], alpha;
  bool has_cxform;
  int16_t cxform[8];
  bool visible, playing;
} dg_flash_view_t;
typedef struct {
  const dg_film_layer_t *layer;
  dg_flash_instance_t *instance;
  unsigned depth;
} dg_flash_layer_view_t;
typedef struct {
  uint32_t id;
  unsigned sprite;
  const dg_flash_sound_t *sound;
  const dg_flash_sound_event_t *event;
  uint32_t start_sample,
      end_sample; // Half-open source interval after clamping.
  uint16_t loops;
  uint64_t position; // Unfolded source samples elapsed since the voice began.
  float gain;
  bool playing, paused;
} dg_flash_audio_t;
typedef struct {
  float x, y;
} dg_flash_point_t;
typedef struct {
  unsigned first, count;
  uint32_t fill, line; // Straight RGBA, red in the low byte.
  float line_width;
  bool filled, stroked;
} dg_flash_path_t;

// Instances are scoped to a root sprite, not to a reusable SWF character.
// Returned pointers remain stable until their owning Flash root is disposed.
dg_flash_instance_t *dg_flash_root(dg_runtime_t *, unsigned sprite);
dg_flash_instance_t *dg_flash_object_instance(dg_runtime_t *, lv_t);
dg_flash_instance_t *dg_flash_child(dg_flash_instance_t *, unsigned depth);
dg_flash_instance_t *dg_flash_parent(dg_flash_instance_t *);
bool dg_flash_view(const dg_flash_instance_t *, dg_flash_view_t *);
const dg_film_layer_t *dg_flash_placement(const dg_flash_instance_t *);
dg_avm1_t *dg_flash_vm(dg_flash_instance_t *);
lv_t dg_flash_object(dg_flash_instance_t *);
dg_avm1_t *dg_flash_vm_for_object(dg_runtime_t *, lv_t);
lv_t dg_flash_target(dg_flash_instance_t *, const char *);
dg_flash_instance_t *dg_flash_named(dg_flash_instance_t *, const char *,
                                    bool fold_case);
// Ordered current display list, including runtime attachments/replacements.
unsigned dg_flash_layer_count(dg_flash_instance_t *);
bool dg_flash_layer_at(dg_flash_instance_t *, unsigned,
                       dg_flash_layer_view_t *);
unsigned dg_flash_graphics(dg_flash_instance_t *, const dg_flash_path_t **,
                           const dg_flash_point_t **);

void dg_flash_sync(dg_runtime_t *);
bool dg_flash_service(dg_runtime_t *, unsigned instruction_budget);
// Advance children once at a Flash frame opportunity. Root targets are
// zero-based unwrapped timeline positions, allowing stream-clock catch-up.
bool dg_flash_advance(dg_runtime_t *, unsigned sprite,
                      unsigned target_root_frame);
void dg_flash_seek(dg_runtime_t *, unsigned sprite, unsigned frame, bool play);
void dg_flash_play(dg_runtime_t *, unsigned sprite, bool play);
void dg_flash_pointer(dg_runtime_t *, int stage_x, int stage_y, bool down);
void dg_flash_key(dg_runtime_t *, unsigned mac_code, unsigned character,
                  bool down);
unsigned dg_flash_audio_count(dg_runtime_t *);
bool dg_flash_audio_at(dg_runtime_t *, unsigned, dg_flash_audio_t *);
void dg_flash_dispose(dg_runtime_t *);
#endif
#endif
