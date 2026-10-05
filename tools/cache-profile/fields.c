#include <stddef.h>
#include "director.h" // compile with the target toolchain; see README.md
#define O(t, f) char off_##t##__##f[offsetof(t, f) + 1]; char len_##t##__##f[sizeof(((t *)0)->f) + 1];
char size__lv_runtime_t[sizeof(lv_runtime_t)]; char size__dg_runtime_t[sizeof(dg_runtime_t)];
O(lv_runtime_t, services) O(lv_runtime_t, context) O(lv_runtime_t, global_names) O(lv_runtime_t, global_count) O(lv_runtime_t, current) O(lv_runtime_t, shared) O(lv_runtime_t, shared_count)
O(lv_runtime_t, call_cache) O(lv_runtime_t, globals) O(lv_runtime_t, roots) O(lv_runtime_t, result) O(lv_runtime_t, the_result)
O(lv_runtime_t, script_objects) O(lv_runtime_t, script_count) O(lv_runtime_t, item_delimiter) O(lv_runtime_t, frames) O(lv_runtime_t, stack)
O(lv_runtime_t, depth) O(lv_runtime_t, objects) O(lv_runtime_t, object_count) O(lv_runtime_t, heap) O(lv_runtime_t, text_scratch) O(lv_runtime_t, heap_used)
O(lv_runtime_t, heap_high_water) O(lv_runtime_t, steps) O(lv_runtime_t, random_state) O(lv_runtime_t, collect_passes) O(lv_runtime_t, clock_us) O(lv_runtime_t, collect_us)
O(lv_runtime_t, atomic_depth) O(lv_runtime_t, arithmetic_depth) O(lv_runtime_t, allocations_since_gc) O(lv_runtime_t, object_hint) O(lv_runtime_t, failed) O(lv_runtime_t, script_error)
O(lv_runtime_t, error) O(lv_runtime_t, last_script_error) O(lv_runtime_t, symbol_cache) O(lv_runtime_t, name_id_for)
O(dg_runtime_t, values) O(dg_runtime_t, platform) O(dg_runtime_t, movie) O(dg_runtime_t, loaded) O(dg_runtime_t, score) O(dg_runtime_t, sprites) O(dg_runtime_t, staged) O(dg_runtime_t, stage_dirty)
O(dg_runtime_t, active_sprites) O(dg_runtime_t, stage_channels) O(dg_runtime_t, trails) O(dg_runtime_t, ticks) O(dg_runtime_t, clock) O(dg_runtime_t, frame) O(dg_runtime_t, mouse_x)
O(dg_runtime_t, key_chars) O(dg_runtime_t, drag_sprite) O(dg_runtime_t, sound_serial) O(dg_runtime_t, transition_type) O(dg_runtime_t, next_movie_label)
O(dg_runtime_t, event_channels) O(dg_runtime_t, event_misses) O(dg_runtime_t, score_delay) O(dg_runtime_t, loop_members) O(dg_runtime_t, field_fonts) O(dg_runtime_t, begin_pending)
O(dg_runtime_t, next_movie) O(dg_runtime_t, files) O(dg_runtime_t, file_names) O(dg_runtime_t, field_members) O(dg_runtime_t, cursor) O(dg_runtime_t, mouse_memo) O(dg_runtime_t, film_member) O(dg_runtime_t, film_loop_channel) O(dg_runtime_t, event_quiet)
