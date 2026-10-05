#ifndef DIRECTOR64_DIRECTOR_H
#define DIRECTOR64_DIRECTOR_H
#include "lingo_runtime.h"
#include "cursor.h"

#if DG_D5
#define DG_SPRITES 49
#define DG_FILES 4
#define DG_SOUND_CHANNELS 4
#define DG_MAX_TEMPO 120
#elif DG_D10
// The D10 corpus authors sprite channels through 983 in 1,006-channel scores,
// and its window movie keeps its code and cast archives loaded beside a
// stage movie that links eleven external cast files.
#define DG_SPRITES 1001
#define DG_FILES 25
#define DG_SOUND_CHANNELS 8
#define DG_MAX_TEMPO 999
#elif DG_CAP_WIDE
#define DG_SPRITES 801
#define DG_FILES 13
#define DG_SOUND_CHANNELS 8
#define DG_MAX_TEMPO 999
#elif DG_EXTENDED
#define DG_SPRITES 121
#define DG_FILES 6
#define DG_SOUND_CHANNELS 4
#define DG_MAX_TEMPO 120
#else
#define DG_SPRITES 121
#define DG_FILES 4
#define DG_SOUND_CHANNELS 2
#define DG_MAX_TEMPO 120
#endif
#define DG_SCORE_CHANNELS (DG_SPRITES + 5)
#if DG_D10
// The exercises' initStamps rewrites a per-letter stamp member set; the
// mutable field table holds every one of them beside the ordinary fields.
#define DG_FIELDS 192
#else
#define DG_FIELDS 64
#endif
// Slot 0 is the search-path list. Slot 1 holds whatever is half-built and not
// yet reachable from anywhere else — a marker list being filled in, a
// behavior's parameter proplist — so a collection cannot take it. Channel
// zero's behavior root served as that scratch until the script channel
// claimed it for its own frame behaviors.
#define DG_SCRATCH_ROOT 1
#define DG_FIELD_TEXT_ROOT 2
#define DG_FIELD_COLOR_ROOT (DG_FIELD_TEXT_ROOT + DG_FIELDS)
#define DG_TRAILS 256
// Stage commits no longer yield per object. Bound service work so dense script
// loops leave VR4300 time for input, audio and drawing at the 60 Hz service
// rate.
#define DG_SERVICE_BUDGET 288
#define DG_MEMBER 1u
#define DG_POSITION 2u
#define DG_WIDTH 4u
#define DG_INK 8u
#define DG_BLEND 16u
#define DG_TYPE 32u
#define DG_HEIGHT 64u
#define DG_FORE 128u
#define DG_BACK 256u
#define DG_THICKNESS 512u
#define DG_MOVEABLE 1024u
#define DG_SIZE (DG_WIDTH | DG_HEIGHT)
#define DG_FLAGS (DG_TYPE | DG_MOVEABLE)
#if DG_MODERN
#define DG_ROTATION 2048u
#define DG_SKEW 4096u
#define DG_ALL 8191u
#else
#define DG_ALL 2047u
#endif
// Set on a score delta that rewrites its channel's whole record, which is how
// Director separates a new sprite span from a change inside the current one.
// It rides above the property bits rather than in its own field because a
// corpus movie holds tens of thousands of deltas. src/director64/director.py
// mirrors the value.
#define DG_SPAN 0x8000u
// Set in seek_now's change masks for every channel a delta wrote, whatever
// the delta's own mask says, so the frame pass can skip the rest.
#define DG_TOUCHED 0x4000u
#define DG_BEHAVIOR_ROOT (DG_FIELD_COLOR_ROOT + DG_FIELDS)
#if DG_D10
// Sixteen authored timeout objects; each slot roots its callback target.
// The slot after the sprite behavior roots holds the actorList.
#define DG_TIMEOUTS 16
#define DG_TIMEOUT_ROOT (DG_BEHAVIOR_ROOT + DG_SPRITES + 1)
_Static_assert(DG_TIMEOUT_ROOT + DG_TIMEOUTS < LV_ROOTS,
               "timeout roots must fit in the Lingo root table");
// Named objects inside a Flash sprite's timeline, addressed by exercise
// scripts (a text field and its ActionScript geometry). Each slot roots one
// property bag; the exercises address a handful per screen.
#define DG_FLASH_OBJECTS 64
#define DG_FLASH_ROOT (DG_TIMEOUT_ROOT + DG_TIMEOUTS)
_Static_assert(DG_FLASH_ROOT + DG_FLASH_OBJECTS < LV_ROOTS,
               "flash object roots must fit in the Lingo root table");
#endif
// Every sprite's behavior root plus the actor-list slot must stay inside
// the Lingo root table; the D10 bring-up overran this silently on host.
// The plain D6 profile never touches behavior roots.
#if DG_MODERN
_Static_assert(DG_BEHAVIOR_ROOT + DG_SPRITES < LV_ROOTS,
               "behavior roots exceed the Lingo root table");
#endif
#define DG_HAS_BLEND 16u

#if DG_MODERN
typedef struct {
  const char *font_name;
  uint8_t font_id, size, align;
  int16_t ascent, descent, leading, line_height;
  uint32_t color;
#if DG_EXTENDED
  const uint8_t *advances;
  const int16_t *kerning; // Printable ASCII pairs and font64 signed kerning amounts.
  unsigned kerning_count;
  // Advance entries counted from codepoint 32. The measured D10 tables cover
  // 32..255; the D6 controller tables cover printable ASCII (95 entries).
  unsigned advance_count;
#endif
} dg_text_style_t;
#endif
typedef struct {
  const char *name;
  uint16_t frame;
} dg_label_t;
#if DG_D10
// A named edit-text field recorded from a converted Flash member: authored
// scripts address it by instance name (sprite(n).my_txt) or bound variable
// (setVariable("text1", …)) and the prompt renders as a text overlay above
// the flattened frame. Geometry is member-local SWF pixels.
typedef struct {
  const char *name, *variable, *text;
  int16_t x, y, width, height;
  int16_t margin_left, margin_right, indent;
  uint8_t align, word_wrap, multiline;
  int8_t leading;
  const dg_text_style_t *style;
} dg_flash_field_t;
#endif
#if DG_EXTENDED
// One authored cue point in a sound member: where it falls, and the name the
// score's cuePassed handlers compare against.
typedef struct {
  uint32_t milliseconds;
  const char *name;
} dg_cue_t;
#endif
typedef struct {
  uint32_t id;
  uint16_t number, cast, type, width, height;
  int16_t reg_x, reg_y;
  const char *name, *asset, *text;
  uint32_t samples, rate, loop_start, loop_end;
  uint16_t shape, pattern;
  uint8_t filled, line_width;
  uint8_t looping, film_count, film_loop;
  const char *const *film_assets;
  uint8_t line_direction;
#if DG_EXTENDED
  bool editable;
  uint32_t text_bytes, source_bytes;
  uint8_t cue_count;
  const dg_cue_t *cues;
#endif
#if DG_MODERN
  const dg_text_style_t *text_style, *text_insert_style;
#endif
#if DG_D5
  const char *video_audio;
  uint32_t video_flags;
  const uint32_t *film_sounds;
#endif
#if DG_D10
  uint8_t source_xtra; // 0 native, 1 Flash, 2 vectorShape — authored type checks.
  // The recorded dynamic Flash surface: authored frame labels over the
  // flattened film timeline, and the named edit-text fields whose prompts
  // render as text overlays.
  uint8_t flash_label_count, flash_field_count;
  const dg_label_t *flash_labels;
  const dg_flash_field_t *flash_fields;
#endif
} dg_member_t;
typedef struct {
  const char *name;
  uint16_t file, cast;
} dg_cast_t;
#if DG_EXTENDED
typedef struct {
  uint32_t script;
  const char *parameters;
} dg_behavior_t;
#endif
typedef struct {
  uint32_t member, script;
  int16_t x, y, width, height;
  // blend is normalized opacity; thickness retains D6's high flag bits.
  uint8_t ink, blend, type, flags, fore, back, thickness, stretch, trails;
#if DG_MODERN
  // loc_z sits with type on the record's second cache line: the draw order
  // reads exactly those two of every active channel each tick.
  int16_t loc_z;
  uint16_t tempo, delay;
  uint32_t fore_rgb, back_rgb;
  int32_t rotation, skew; // degrees in hundredths, as stored in D8 scores
#endif
#if DG_EXTENDED
  const dg_behavior_t *behaviors;
  uint8_t behavior_count;
#endif
} dg_spec_t;
typedef struct {
  uint16_t channel, mask;
  dg_spec_t value;
} dg_delta_t;
typedef struct {
  uint32_t first;
  uint16_t count;
} dg_frame_t;
// The member table sorted by a case-folded hash of the member name, so a name
// search binary-searches four bytes per step instead of striding a table of
// ~80-byte records. Entries for equal hashes keep their (cast, number) order,
// which is the order a linear search would have found them in.
typedef struct {
  uint32_t hash;
  uint16_t member; // index into dg_movie_t.members
} dg_member_index_t;
// The order member_index is built in. src/director64/director.py mirrors this
// exactly; tests/test_director.py compares the two implementations.
uint32_t dg_member_name_hash(const char *);
typedef struct {
  const lv_movie_t *code;
  uint16_t id, tempo, cast_count, member_count, frame_count, label_count;
  const dg_cast_t *casts;
  const dg_member_t *members;
  const dg_member_index_t *member_index;
  const dg_frame_t *frames;
  const dg_delta_t *deltas;
  const dg_label_t *labels;
  const uint32_t *palette;
#if DG_MODERN
  uint32_t stage_color;
#endif
} dg_movie_t;
typedef struct {
  dg_spec_t value;
  // The flags the frame advance and the hit test read share the 16-byte
  // line after the score record; the quad, cursor and constraint that only
  // a custom-quad or cursor path reads come last, so the passes over the
  // table touch four lines of a record, not eight (2026-09-26 profile).
  bool puppet, visible, stretch, trails;
  uint8_t moveable;
#if DG_MODERN
  bool flip_h, flip_v, custom_quad;
#endif
  unsigned auto_mask;
  unsigned film_frame;
#if DG_MODERN
  float quad[8]; // offsets from sprite location, for authored script quads
#endif
  dg_cursor_t cursor;
#if DG_CAP_CONSTRAINTS
  int32_t constraint;
#endif
#if DG_D5
  double video_time, video_rate;
  unsigned video_start, video_stop, video_serial, video_volume;
  uint32_t media_member;
  unsigned media_frame;
#endif
} dg_sprite_t;
typedef struct {
  unsigned channel;
  dg_sprite_t sprite;
} dg_trail_t;
typedef struct {
  bool used, write, append;
#if DG_MODERN
  bool opened, dirty;
  int status;
#endif
  unsigned file, position, length;
  char data[16384];
} dg_file_t;
typedef struct {
  // Read/write an explicitly named virtual save file; never a host path.
  bool (*read_file)(void *, const char *, char *, unsigned, unsigned *);
  bool (*write_file)(void *, const char *, const char *, unsigned);
  const char *(*nth_file)(void *, const char *, int);
  bool (*sound_busy)(void *, unsigned);
  void (*sound)(void *, unsigned, const dg_member_t *);
  void (*trace)(void *, const char *);
  bool (*hit)(void *, const dg_member_t *, unsigned, int, int);
  const char *(*long_date)(void *);
#if DG_EXTENDED
  // How far into the sound on this channel playback has reached, in
  // milliseconds. Authored cue points fire off the mixer's own position
  // rather than a frame count, so a frame that holds for a line of speech
  // ends when the speech does. NULL leaves cue points undelivered.
  unsigned (*sound_position)(void *, unsigned);
#endif
#if DG_D10
  // Stream a converted external audio file on a sound channel; the name is
  // a virtual "folder/stem" pair, never a host path. NULL stops the channel.
  void (*play_file)(void *, unsigned, const char *);
  // Resolve a registered movie by authored stem (case-insensitive, extension
  // ignored) or, with a NULL stem, by scene file id. The platform keeps the
  // returned movie resident until the next stage transition recomputes the
  // resident set.
  const dg_movie_t *(*find_movie)(void *, const char *, unsigned);
  // Read a shipped read-only data file by its virtual "folder/name" path
  // (the exercise task databases). Never a host path, and never writable:
  // mutable records live in the save archive behind read_file.
  bool (*read_data)(void *, const char *, char *, unsigned, unsigned *);
#endif
} dg_platform_t;
typedef struct {
  lv_runtime_t *values;
  dg_platform_t platform;
  void *context;
  const dg_movie_t *movie, *loaded[DG_FILES];
  unsigned loaded_count;
  dg_spec_t score[DG_SCORE_CHANNELS];
  dg_sprite_t sprites[DG_SPRITES];
  dg_sprite_t staged[DG_SPRITES];
  bool stage_dirty[DG_SPRITES];
  // Live graphics or pending sprite mutations; avoids scanning empty channels.
  uint32_t active_sprites[(DG_SPRITES + 31) / 32];
  uint16_t stage_channels[DG_SPRITES];
  // seek_now's change masks, resident rather than a 3 KB stack array.
  uint16_t seek_changed[DG_SPRITES];
  // The draw order, kept from one dg_draw_order call to the next while no
  // sprite changed: the hit test and the renderer each asked for it every
  // tick, and each walk read the type and depth of every active channel.
  uint16_t order_cache[DG_SPRITES];
  unsigned order_count;
  uint32_t order_serial, order_cached;
  bool order_valid;
  unsigned stage_count;
  dg_trail_t trails[DG_TRAILS];
  unsigned trail_count;
  uint32_t ticks, timer, resume_tick, frame_tick, frame_serial;
  // One clock unit is 1/60 microsecond; a service tick is exactly 1,000,000.
  uint64_t clock, frame_deadline;
  unsigned clock_tempo, frame_remainder;
  bool frame_stalled;
  unsigned frame, next_frame, phase, event_cursor, tempo, puppet_tempo,
      click_on;
  int mouse_x, mouse_y;
  // Counts every committed change to what the stage shows, so a platform can
  // recognize a frame it has already composited.
  unsigned visual_revision;
#if DG_CAP_KEYBOARD
  // Director uses Macintosh virtual key codes, including the arrow keys.
  uint8_t key_chars[128];
  uint32_t keys_down[4];
  struct { uint8_t code, character; bool down; } key_events[32];
  unsigned key_first, key_count, key_event_tick;
  uint8_t key_code, key_character;
#if DG_D7_OR_D10
  bool mouse_hit_valid;
  int mouse_hit_x, mouse_hit_y;
  unsigned mouse_hit_result;
#endif
#if DG_D10
  // The one authored controller window: resident code, unticked score. The
  // requested movie stem awaits the platform's overlay load; attachment
  // completes through dg_window_attach. The first attachment is the home
  // controller whose code stays resident beside the focused window movie,
  // and a navigation issued while a load is pending queues behind it.
  const dg_movie_t *window_movie, *window_home;
  char window_movie_name[40], window_movie_queue[40];
  bool tell_window, window_open, window_visible;
  int window_rect[4];
  // Channel-0 frame behaviors receive beginSprite when their span begins.
  // The span is identified by its parameter block, not by the script member:
  // consecutive spans routinely re-use one behavior with different authored
  // properties, which is how the castle introduction names each of its twelve
  // narration cues.
  uint32_t frame_script_member;
  bool frame_script_begin_pending;
#if DG_EXTENDED
  const dg_behavior_t *frame_behaviors;
#endif
  // Authored castLib fileName swaps: a named cast library rebinds to another
  // cast archive (class-specific Structure casts). Members of the swapped
  // library resolve into the target archive's single External cast.
  struct {
    char name[20], stem[20];
    const dg_movie_t *target;
  } cast_swaps[4];
  unsigned cast_swap_count;
  // Mutable save-cast storage: castLib fileName can retarget a cast
  // archive's records to a virtual save file ("save", "save01"…), the
  // per-player profile mechanism.
  struct {
    uint32_t file;
    char name[24];
  } cast_stores[4];
  unsigned cast_store_count;
  // Authored timeout objects: each period the runtime calls the rooted
  // target's handler. Timeouts survive movie changes; authored cleanup
  // forgets the non-persistent ones through `the timeoutList`.
  struct {
    char name[24], handler[24];
    uint32_t period, next;
    bool active, persistent;
  } timeouts[DG_TIMEOUTS];
  unsigned timeout_cursor;
  struct {
    unsigned sprite;
    char name[24];
  } flash_objects[DG_FLASH_OBJECTS];
  unsigned flash_object_count;
  // The sprite holding keyboard focus, or zero for none.
  unsigned keyboard_focus;
#endif
#endif
  unsigned drag_sprite;
  int drag_offset_x, drag_offset_y;
  bool mouse_down, mouse_pressed, mouse_released, mouse_up_dispatch,
      await_release, quit, transitioned;
  uint32_t sound_serial[DG_SOUND_CHANNELS], sounds[DG_SOUND_CHANNELS];
  bool sound_puppet[DG_SOUND_CHANNELS];
#if DG_EXTENDED
  // Cue-point delivery per sound channel. The serial is the one the channel
  // held when this state was taken, so replacing what a channel plays starts
  // its cue list again without the sound path having to say so.
  struct {
    const dg_member_t *member;
    uint32_t serial;
    // The service loop runs hundreds of times between ticks and playback
    // cannot move in between, so the channel's position is read once a tick
    // and held here.
    uint32_t polled;
    unsigned position;
    uint8_t next;
  } cue_state[DG_SOUND_CHANNELS];
#endif
  unsigned transition_type, transition_serial;
  // Requested visual parameters: duration in ticks (quarter-second minimum,
  // as the original), chunk size in pixels. The platform renderer decides
  // whether it can animate the type; the score itself never blocks.
  unsigned transition_duration, transition_chunk;
#if DG_EXTENDED
  uint16_t behavior_channel, behavior_cursor;
  char behavior_event[32];
  struct { dg_member_t member; char name[64]; } dynamic_members[32];
  unsigned dynamic_count;
  bool restored_casts[64];
  bool field_editable[DG_FIELDS];
  int8_t sprite_editable[DG_SPRITES]; // 0 inherits the member, +/-1 overrides it.
  unsigned sel_start, sel_end, text_sprite, text_key, text_key_code, text_index;
  char text_replacement[21];
  bool text_pending, pass_event, cleanup_active, ending_sprites, movie_ready, quit_requested;
  unsigned input_stage, input_channel;
  uint32_t idle_tick, hover_tick;
  bool right_mouse_down, notice_armed;
  char notice[160];
#endif
#if DG_MODERN
  char next_movie_label[80];
#endif
#if DG_D10
  // The room engine drives its characters through actorList stepFrame.
  unsigned actor_cursor;
#endif
#if DG_D5
  unsigned score_wait, rollover_sprite, actor_cursor, mouse_up_sprite;
  unsigned input_event, input_stage, input_channel;
  bool pass_event;
  bool native_dialog, window_open, window_cleanup, source_paused, stopped_movie;
  void *suspended_stage;
  char mouse_down_script[80];
  char alert_text[160];
  unsigned alert_until;
#endif
#if DG_MODERN
  // Any cast member or non-VOID behavior root is event-eligible. Keep this
  // compact index live on every mutation instead of rereading every large sprite.
  uint32_t event_channels[(DG_SPRITES + 31) / 32];
  // Missing standard events only; IDs survive overlay relocation, and enter
  // clears these facts before any newly loaded movie can use them.
  struct {
    uint32_t member;
    uint8_t missing;
  } event_misses[
#if DG_CAP_WIDE
      512
#else
      128
#endif
  ];
  uint16_t score_delay, begin_cursor, hover_sprite, current_event_sprite;
  bool start_movie_pending;
  bool exit_lock, search_current_folder;
  uint32_t stage_color;
  uint16_t channel_volume[DG_SOUND_CHANNELS], fade_volume[DG_SOUND_CHANNELS];
  uint32_t fade_start[DG_SOUND_CHANNELS], fade_duration[DG_SOUND_CHANNELS];
  uint32_t loop_members[64];
  bool loop_values[64];
  unsigned loop_count;
  char field_fonts[DG_FIELDS][64];
  bool field_text_changed[DG_FIELDS];
  int field_scroll[DG_FIELDS];
  bool begin_pending[DG_SPRITES];
#endif
  char next_movie[32], mouse_up_script[64];
#if DG_EXTENDED
  char mouse_down_script[64], key_down_script[64];
  bool mouse_down_dispatch;
#endif
  unsigned next_movie_frame;
  dg_file_t files[4];
  char file_names[4][32];
  uint32_t field_members[DG_FIELDS];
  unsigned field_count;
  dg_cursor_t cursor;
  int sound_level, color_depth, float_precision;
  // Whether the code for a member or behavior script declares any mouse
  // handler. That is a fact about the generated handler table, but reading it
  // walks every handler the owning movie declares, and hit testing asks it
  // about several sprites on every tick. Direct-mapped by member/script ID;
  // enter drops these before newly loaded overlays can answer differently.
  // Appended so the sprite and score tables above keep their offsets.
  struct {
    uint32_t id;
    uint8_t known, mouse;
  } mouse_memo[64];
#if DG_MODERN
  // Whether a channel's current member is a film loop. Every frame asks this
  // of every member-bearing channel — eighty-odd at the train station, of
  // which a handful animate — and answering it means resolving the member
  // through its movie. The member ID is the whole key: a different ID
  // recomputes, and enter clears the table because IDs belong to the set of
  // movies loaded then. Appended so the tables above keep their offsets.
  uint32_t film_member[DG_SPRITES];
  uint8_t film_loop_channel[DG_SPRITES];
  // The channels whose member is a film loop, kept current by
  // event_channel_changed, with the member each holds: the frame advance
  // walks these bits instead of reading every member-bearing sprite.
  uint32_t film_channels[(DG_SPRITES + 31) / 32];
  const dg_member_t *film_loop_member[DG_SPRITES];
  // Channels known to start nothing for a standard frame event, indexed by
  // that event's bit position. This is the per-channel view of event_misses:
  // the same fact, indexed so a phase skips a silent channel in the bit scan
  // instead of resolving its member and hashing it. A station frame asks
  // eighty-odd channels for three events and almost none of them answer.
  // Only channels without a behavior list are recorded — a list's instances
  // can gain a handler without the channel changing — and
  // event_channel_changed drops a channel's bits the moment its member or
  // its list does, which is the same funnel event_channels relies on.
  uint32_t event_quiet[3][(DG_SPRITES + 31) / 32];
#endif
} dg_runtime_t;

void dg_init(dg_runtime_t *, lv_runtime_t *, dg_platform_t, void *,
             const char *const *, unsigned, uint32_t);
bool dg_enter(dg_runtime_t *, const dg_movie_t *, unsigned,
              const dg_movie_t *const *, unsigned);
dg_cursor_t dg_cursor_at(dg_runtime_t *, int, int);
dg_cursor_t dg_cursor_current(dg_runtime_t *);
// Also called before a platform unloads old audio resources; idempotent.
void dg_stop_sounds(dg_runtime_t *);
unsigned dg_opacity(const dg_sprite_t *);
unsigned dg_film_pose(const dg_member_t *, const dg_sprite_t *);
bool dg_tick(dg_runtime_t *, int, int, bool, unsigned);
#if DG_EXTENDED
bool dg_edit_text(dg_runtime_t *, unsigned sprite, const char *text);
bool dg_text_editable(dg_runtime_t *, unsigned sprite);
#endif
bool dg_transition_ready(const dg_runtime_t *);
#if DG_CAP_KEYBOARD
void dg_key(dg_runtime_t *, unsigned code, unsigned character, bool down);
#endif
// Service scripts and score events with bounded work, without advancing time.
bool dg_service(dg_runtime_t *, unsigned);
// Rational 60 Hz platform clock; bounded work retains all elapsed time.
bool dg_clock_advance(uint64_t *, uint64_t, unsigned *);
bool dg_seek(dg_runtime_t *, unsigned);
const dg_member_t *dg_member(dg_runtime_t *, uint32_t);
const dg_movie_t *dg_loaded(dg_runtime_t *, unsigned);
bool dg_event(dg_runtime_t *, const char *, unsigned);
unsigned dg_hit(dg_runtime_t *, int, int);
unsigned dg_mouse_hit(dg_runtime_t *, int, int);
void dg_bounds(dg_runtime_t *, unsigned, int *, int *, int *, int *);
void dg_sprite_bounds(dg_runtime_t *, const dg_sprite_t *, int *, int *, int *,
                      int *);
// Clockwise screen-space corners, in source order TL, TR, BR, BL.
void dg_sprite_quad(dg_runtime_t *, const dg_sprite_t *, float[8]);
bool dg_quad_uv(const float[8], float, float, float *, float *);
// The channels to draw, back to front; the array belongs to the runtime.
unsigned dg_draw_order(dg_runtime_t *, const uint16_t **);
// Commit the logical stage, including trails; never wait for a framebuffer.
void dg_update_stage(dg_runtime_t *);
// Required after direct sprite or behavior-root mutations outside the Lingo
// property services; refreshes D8 event eligibility even if already stage-dirty.
void dg_sprite_changed(dg_runtime_t *, unsigned);
#if DG_D10
// The named field of a Flash member, matched case-insensitively against the
// recorded instance name or bound variable; NULL when the name is not a field.
const dg_flash_field_t *dg_flash_field_find(const dg_member_t *, const char *);
// Wrap UTF-8 field text into `width` member-local pixels using the field
// style's measured advances (half an em per character without a table, as
// the metrics fallback), at `size` points (0 keeps the descriptor size).
// Writes '\n'-separated lines into out and returns the laid-out text height
// in the same pixels.
unsigned dg_flash_field_wrap(const dg_flash_field_t *, unsigned, const char *,
                             int, char *, unsigned);
// Complete a requested controller-window movie attachment once its overlay
// is resident; the window's code then survives every stage movie change.
void dg_window_attach(dg_runtime_t *, const dg_movie_t *);
// Attach the pending window movie when the current movie satisfies it; the
// focused-window boot enters window movies in the single context.
void dg_window_loaded(dg_runtime_t *);
#endif
#endif
