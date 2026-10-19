#ifndef DIRECTOR64_DIRECTOR_H
#define DIRECTOR64_DIRECTOR_H
#include "lingo_runtime.h"
#include "cursor.h"
#include "imaging.h"

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
// The longest sound playlist a channel keeps (Löwenzahn Optik queues
// spoken sentences a few words at a time).
#define DG_SOUND_QUEUE 16
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
// loops leave time for input, audio and drawing at the 60 Hz service rate.
#define DG_SERVICE_BUDGET 288
// A member id packs its file, its cast within that file and its number:
// file << 21 | cast << 16 | number. Five cast bits hold the 17 cast
// libraries a Findus 8.5 movie keeps in one file.
#define DG_ID_FILE_SHIFT 21
#define DG_ID_CAST_MASK 31u
#define DG_ID(file, cast, number) \
  ((uint32_t)(file) << DG_ID_FILE_SHIFT | (uint32_t)(cast) << 16 | (uint32_t)(number))
#define DG_ID_FILE(id) ((uint32_t)(id) >> DG_ID_FILE_SHIFT)
#define DG_ID_CAST(id) (((uint32_t)(id) >> 16) & DG_ID_CAST_MASK)
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
// The Amplitude Xtra's loudness curve (ampSetCurve), one per runtime.
#define DG_AMP_ROOT (DG_FLASH_ROOT + DG_FLASH_OBJECTS)
_Static_assert(DG_AMP_ROOT < LV_ROOTS, "the amplitude root must fit in the Lingo root table");
// The result a suspended handler is about to read, held across the begin
// fan-out that runs before it resumes.
#define DG_HELD_RESULT_ROOT (DG_AMP_ROOT + 1)
_Static_assert(DG_HELD_RESULT_ROOT < LV_ROOTS, "the held result root must fit in the Lingo root table");
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
  const int16_t *kerning; // Printable ASCII pairs and signed kerning amounts (1/127 em).
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
typedef struct dg_film_frame dg_film_frame_t;
typedef struct dg_film_track dg_film_track_t;
typedef struct dg_flash_clip dg_flash_clip_t;
typedef struct dg_flash_button dg_flash_button_t;
typedef struct {
  uint16_t frame; // One-based frame; zero initializes a definition/placement.
  uint32_t event, length; // Event zero is a frame action; other bits retain SWF conditions.
  const uint8_t *code;
  uint8_t key; // ClipEventKeyPress key; button key conditions remain in event.
} dg_flash_action_t;
typedef struct {uint16_t clip_id; dg_flash_action_t action;} dg_flash_initializer_t;
typedef struct {
  uint32_t position; // Envelope positions retain SWF's 44.1 kHz time base.
  uint16_t left, right;
} dg_flash_envelope_t;
typedef struct {
  uint16_t frame, sound, loops;
  uint8_t flags, envelope_count;
  uint32_t in_point, out_point;
  const dg_flash_envelope_t *envelopes;
} dg_flash_sound_event_t;
typedef struct { uint16_t id; const char *asset; uint32_t samples, rate; } dg_flash_sound_t;
typedef struct { uint16_t id; const char *name; } dg_flash_export_t;
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
  int32_t local_bounds[4]; // 16.16 left,top,width,height before SWF placement.
  uint16_t id; // Character definition ID; the placement owns its live value.
  bool embed_fonts; // DefineEditText UseOutlines; device text is not mask geometry.
} dg_flash_field_t;
typedef struct {
  const char *name, *target, *property;
  uint8_t operation, argument; // 1 add, 2 set, 3 font size, 4 unescaped text.
  bool method; // Receiver is target field rather than the containing sprite.
} dg_flash_helper_t;
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
  uint8_t looping, film_loop;
  uint16_t film_count;
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
#if DG_D5 || DG_D10
  // A digital video (D5) or DirectMedia video (D10, source Xtra 3): its
  // sound's asset and the authored flags (8 sound, 16 loop, 256 paused,
  // 512 no picture).
  const char *video_audio;
  uint32_t video_flags;
#endif
#if DG_D5 || DG_D10
  // Interleaved sound1/sound2 member ids, two entries per film frame.
  // NULL when the cast's sound-enable flag is clear.
  const uint32_t *film_sounds;
#endif
#if DG_D10
  uint8_t source_xtra; // 0 native, 1 Flash, 2 vectorShape, 3 DirectMedia, 4 QuickTime video, 5 color cursor.
  // The recorded dynamic Flash surface: authored frame labels over the
  // flattened film timeline, and the named edit-text fields whose prompts
  // render as text overlays.
  uint8_t flash_label_count, flash_field_count;
  const dg_label_t *flash_labels;
  const dg_flash_field_t *flash_fields;
  // A linked Director movie: a registry identity, independent child stage
  // origin, and the authored movie-cast flags (scripts16, sound8, noLoop32,
  // noCrop2, center1).
  uint16_t linked_movie;
  uint32_t linked_flags;
  int16_t linked_left, linked_top;
  // Recorded film frames requiring live composition (text or nested
  // runtime media). Frames use the same playhead as flattened film poses.
  const dg_film_frame_t *film_frames;
  int16_t film_left, film_top;
  uint32_t film_flags; // Native film cast: sound8, no-loop32, scale2, center1.
  uint16_t film_track_count;
  const dg_film_track_t *film_tracks;
  uint16_t flash_stop_count;
  const uint16_t *flash_stops; // Authored one-based Stop action frames.
  uint16_t flash_helper_count;
  const dg_flash_helper_t *flash_helpers;
  uint8_t flash_variant_count; // Zero/one: ordinary poses; >one: frame-major phase variants.
  uint32_t flash_rate; // Intrinsic SWF frames per second, 16.16 fixed point.
  const uint32_t *flash_audio_positions; // PCM offset per root frame; UINT32_MAX means no block.
  uint8_t flash_playback_mode; // 0 Normal, 1 Fixed, 2 Lock-Step.
  uint32_t flash_fixed_rate; // Fixed playback frames per second, 16.16.
  uint8_t flash_direct_to_stage, flash_version;
  uint16_t flash_clip_count, flash_button_count, flash_sound_count, flash_export_count;
  const dg_flash_clip_t *flash_clips;
  const dg_flash_button_t *flash_buttons;
  const dg_flash_sound_t *flash_sounds;
  const dg_flash_export_t *flash_exports;
  uint16_t flash_initializer_count;
  const dg_flash_initializer_t *flash_initializers;
  // Exact styles needed by script-created fields or TextFormat objects.
  uint16_t flash_font_count;
  const dg_text_style_t *const *flash_fonts;
#endif
  uint16_t palette_count; // Exact authored CLUT size, 0 when absent.
  const uint32_t *palette;
} dg_member_t;
typedef struct {
  const char *name;
  uint16_t file, cast;
#if DG_MODERN
  const char *const *shape_patterns;
#endif
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
#if DG_D10
struct dg_film_track {
  uint16_t channel, count;
  uint32_t size;
  const uint8_t *data; // LE frame/mask, then changed fields; film_track.h.
};
// Script edits overlay the current recorded film frame, per parent instance.
// Keep the owning member so reusing a stage channel cannot leak old edits.
#define DG_FILM_CHILDREN 256
#define DG_CHILD_SPRITE 0x40000000u
#define DG_CHILD_SHIFT 10
#define DG_CHILD_CHANNEL_MASK 1023u
#define DG_CHILD_VISIBLE 0x10000u
#define DG_CHILD_STRETCH 0x20000u
typedef struct {
  uint16_t parent, channel;
  uint32_t owner, mask;
  dg_spec_t value;
  bool visible, puppet;
} dg_film_child_t;
#endif
#if DG_D10
typedef struct {
  uint16_t channel, film_frame;
  dg_spec_t value;
  // Direct Flash depth planes use member=0. Origins retain the authored
  // object's translation in 16.16 stage pixels, independent of pixel crop.
  const char *asset, *name;
  // Geometric Flash masks ignore fill opacity, color transforms and strokes.
  // Every retained raster leaf supplies coverage, sharing asset when equal.
  const char *mask_asset;
  int32_t origin_x, origin_y;
  bool affine;
  int32_t matrix[6]; // 16.16 a,b,c,d,tx,ty; live text's authored placement.
  uint16_t clip_id, button_id, field_id, clip_depth;
  uint32_t placement_generation;
  bool has_cxform;
  int16_t cxform[8]; // RGBA multipliers divided by256, then RGBA offsets.
  uint16_t action_count;
  const dg_flash_action_t *actions;
} dg_film_layer_t;
struct dg_film_frame {
  uint16_t count;
  const dg_film_layer_t *layers;
};
struct dg_flash_clip {
  uint16_t id, frame_count, action_count, label_count, sound_event_count;
  int32_t local_bounds[4]; // Geometry origin and extent; never an implicit clip.
  const dg_film_frame_t *frames; // ID0 uses the member's root film_frames.
  const dg_flash_action_t *actions;
  const dg_label_t *labels;
  const dg_flash_sound_event_t *sound_events;
};
struct dg_flash_button {
  uint16_t id, action_count;
  bool track_as_menu;
  int32_t local_bounds[4];
  dg_film_frame_t states[4]; // Up, Over, Down, HitTest.
  const dg_flash_action_t *actions;
};
#endif
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
#if DG_D10
// A text member's colour runs (first character, 0xRRGGBB) and hyperlinks
// (1-based first and last character, data): Optik's lexicons colour their
// link words and its Walk of Fame links by range. Only members that have
// any are listed, in a small per-movie table.
typedef struct {
  uint32_t offset, rgb;
} dg_text_run_t;
typedef struct {
  uint32_t first, last;
  const char *data;
} dg_link_t;
typedef struct {
  uint32_t member;
  uint16_t run_count, link_count;
  const dg_text_run_t *runs;
  const dg_link_t *links;
} dg_text_extra_t;
#endif
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
#if DG_D10
  uint16_t text_extra_count;
  const dg_text_extra_t *text_extras;
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
  bool film_paused;
#if DG_D10
  uint16_t film_instance; // Transient native-film playback instance, zero for a fresh stage sprite.
  uint8_t flash_variant, flash_clock; // One-based initial choice; independent nested clock modulo variants.
  double flash_phase; // Fractional Flash clock; absolute root frame for streamed audio.
  uint32_t flash_fixed_rate;
  uint8_t flash_mode, flash_settings; // Sprite overrides: mode bit1, fixed-rate bit2.
#endif
#if DG_MODERN
  float quad[8]; // offsets from sprite location, for authored script quads
#endif
  dg_cursor_t cursor;
#if DG_CAP_CONSTRAINTS
  int32_t constraint;
#endif
#if DG_D5 || DG_D10
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
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Birthday's room descriptions run to 18 KB of XML.
  char data[32768];
#else
  char data[16384];
#endif
} dg_file_t;
struct dg_runtime;
struct dg_flash_instance;
typedef struct {
  // Read/write an explicitly named virtual save file; never a host path.
  bool (*read_file)(void *, const char *, char *, unsigned, unsigned *);
  bool (*write_file)(void *, const char *, const char *, unsigned);
  const char *(*nth_file)(void *, const char *, int);
  bool (*sound_busy)(void *, unsigned);
  void (*sound)(void *, unsigned, const dg_member_t *);
  // Pause/resume a channel without losing its position or busy state.
  void (*sound_pause)(void *, unsigned, bool);
  // Disable looping without seeking; continue through the sound's tail.
  void (*sound_break_loop)(void *, unsigned);
  void (*trace)(void *, const char *);
  bool (*hit)(void *, const dg_member_t *, unsigned, int, int);
  const char *(*long_date)(void *);
#if DG_D10
  // The folder `the moviePath` answers; NULL answers the drive root.
  const char *(*movie_path)(void *);
  // The shipped data folder ("SPIEL/XML") FileIO falls back to for a file
  // the save archive does not hold; NULL for none.
  const char *(*data_folder)(void *);
#endif
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
  // Keep what the stage shows now as the member's image (`member.image =
  // (the stage).image`); NULL leaves such members showing their own image.
  void (*capture_stage)(void *, uint32_t member);
  // Copy an authored member (including text), or the stage for member 0,
  // into independently owned pixels. Failure is a script error.
  bool (*read_image)(void *, uint32_t member, dg_pixel_image_t *);
  // Embedded Flash text used as a geometric mask: sample glyph coverage
  // in field-local authored coordinates, independent of object opacity.
  bool (*flash_field_hit)(void *, struct dg_runtime *,
                          struct dg_flash_instance *, float x, float y);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Which character of a text member's box lies at (x, y) in that box, as
  // the host set it: 0-based in Unicode characters, -1 for none. `sprite`
  // names the channel showing it (its rectangle is the box), 0 for the
  // member's own size. NULL where no host lays text out.
  int (*text_char_at)(void *, uint32_t member, unsigned sprite, int x, int y);
#endif
#endif
} dg_platform_t;
typedef struct dg_runtime {
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
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // The frame each channel's live sprite span began on, and seek_now's
  // record of the last span start it replayed: a backward go that stays
  // inside a span keeps the span's behavior instances.
  uint16_t span_start[DG_SPRITES], seek_span[DG_SPRITES];
#endif
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
    uint32_t member;
    uint16_t overridden;
    char name[24];
  } flash_objects[DG_FLASH_OBJECTS];
  unsigned flash_object_count;
  // The sprite holding keyboard focus, or zero for none.
  unsigned keyboard_focus;
  bool key_dispatch_down;
  struct { uint32_t member, fixed_rate; uint8_t mode; bool paused; } flash_playback[DG_FIELDS];
  unsigned flash_playback_count;
  struct dg_flash_state *flash_state;
#endif
#endif
  unsigned drag_sprite;
  int drag_offset_x, drag_offset_y;
  bool mouse_down, mouse_pressed, mouse_released, mouse_up_dispatch,
      await_release, quit, transitioned;
  uint32_t sound_serial[DG_SOUND_CHANNELS], sounds[DG_SOUND_CHANNELS];
  bool sound_paused[DG_SOUND_CHANNELS];
  bool sound_puppet[DG_SOUND_CHANNELS];
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Each sound object's playlist (queue/setPlaylist): members still to
  // play, and whether the channel moves on to the next one by itself.
  uint32_t sound_queue[DG_SOUND_CHANNELS][DG_SOUND_QUEUE];
  uint8_t sound_queue_count[DG_SOUND_CHANNELS];
  bool sound_chain[DG_SOUND_CHANNELS];
  // Each channel's stereo pan (-100..100), kept for scripts that read it
  // back; the mixer plays every channel centred.
  int8_t channel_pan[DG_SOUND_CHANNELS];
  // The legacy idle timeout: the handler named by the timeoutScript runs
  // once no input arrived for timeoutLength ticks.
  uint32_t timeout_length, last_input_tick;
  // The Amplitude Xtra: curve samples per second and the channel it follows.
  uint16_t amp_resolution, amp_channel;
  // Whether the edited field's own keyDown handlers passed the typed key.
  bool text_passed;
  char timeout_script[32];
#endif
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
  uint16_t fade_target[DG_SOUND_CHANNELS];
  bool fade_restore[DG_SOUND_CHANNELS];
  uint32_t fade_start[DG_SOUND_CHANNELS], fade_duration[DG_SOUND_CHANNELS];
  uint32_t loop_members[64];
  bool loop_values[64];
  unsigned loop_count;
  char field_fonts[DG_FIELDS][64];
  bool field_text_changed[DG_FIELDS], field_font_changed[DG_FIELDS];
  int field_scroll[DG_FIELDS];
  bool begin_pending[DG_SPRITES];
#endif
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Folder-qualified movie names run long (LEXIKON_TEIL1__LEXIKON_M_TRAEGHEIT.DXR).
  char next_movie[64], mouse_up_script[64];
#else
  char next_movie[32], mouse_up_script[64];
#endif
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // A click since the frame began: the tempo channel's wait for one.
  bool tempo_clicked;
  // The path `go to movie` named ("@::lexikon:moor"): ports whose files
  // are folder-qualified resolve its folder (web_runtime.c find_path).
  char next_movie_path[96];
  // `play movie` returns: the calling movie and frame `play done` resumes.
  char play_return[4][64];
  unsigned play_return_frame[4], play_depth;
#endif
#if DG_D10
  dg_film_child_t film_children[DG_FILM_CHILDREN];
  unsigned film_child_count;
  struct dg_film_playback *film_playback;
  // The name `the movieName` answers when the platform entered the movie
  // by an authored long name its 8.3 file truncates ("Baumhoehle.dxr");
  // empty answers the file's own name.
  char movie_alias[40];
#endif
#if DG_EXTENDED
  char mouse_down_script[64], key_down_script[64];
  bool mouse_down_dispatch;
#endif
  unsigned next_movie_frame;
  dg_file_t files[4];
  char file_names[4][32];
  uint32_t field_members[DG_FIELDS];
  unsigned field_count;
  uint8_t field_patterns[DG_FIELDS];
  uint32_t active_palette[256];
  bool palette_active, palette_puppet;
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
#if DG_D10
  // Owned Imaging Lingo surfaces, reclaimed by script reachability.
  dg_imaging_t *imaging;
  struct dg_linked_state *linked_state;
  struct dg_linked_child *linked_children;
  struct dg_runtime *linked_parent;
  struct dg_runtime *linked_sound_owner[DG_SOUND_CHANNELS];
  uint32_t linked_flags;
  // Per-boot instance identity for platform resources; never reuse a removed
  // child's video/audio handle even if the allocator reuses its address.
  uint32_t linked_instance, linked_next_instance;
  uint32_t linked_frame_serial;
  unsigned linked_depth, linked_focus;
  bool linked_paused;
  uint16_t film_sound_sprite[2], film_sound_pose[2];
  uint32_t film_sound_instance[2];
  uint32_t linked_channels[(DG_SPRITES + 31) / 32];
  uint32_t flash_direct_channels[(DG_SPRITES + 31) / 32];

#endif
} dg_runtime_t;
static inline const uint32_t *dg_stage_palette(const dg_runtime_t *d) {
#if DG_D10
  while (d->linked_parent) d = d->linked_parent;
#endif
  return d->palette_active ? d->active_palette : d->movie ? d->movie->palette : NULL;
}

unsigned dg_member_pattern(const dg_runtime_t *, const dg_member_t *);
const char *dg_shape_texture(const dg_runtime_t *, const dg_member_t *, unsigned pattern);

// Stable during an instance's lifetime and distinct across linked movies.
static inline unsigned dg_video_player_id(const dg_runtime_t *d, unsigned channel) {
#if DG_D10
  return d->linked_instance * DG_SPRITES + channel;
#else
  (void)d;
  return channel;
#endif
}

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
unsigned dg_film_storage_pose(const dg_member_t *, const dg_sprite_t *);
const char *dg_film_asset(const dg_member_t *, const dg_sprite_t *);
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
#if DG_D5 || DG_D10
// Converted native video, including QuickTime and DirectMedia Xtras.
static inline bool dg_is_video(const dg_member_t *m) {
#if DG_D5
  return m && m->type == 10;
#else
  return m && (m->type == 10 || (m->type == 15 && (m->source_xtra == 3 || m->source_xtra == 4)));
#endif
}
// Flash stream sound uses the same per-instance media players as video,
// while its picture continues through the live Flash compositor.
static inline bool dg_is_flash_audio(const dg_member_t *m) {
#if DG_D10
  return m && m->source_xtra == 1 && m->flash_audio_positions &&
         m->video_audio && *m->video_audio && m->rate;
#else
  (void)m; return false;
#endif
}
static inline bool dg_has_media(const dg_member_t *m) {
  return dg_is_video(m) || dg_is_flash_audio(m);
}
static inline double dg_media_duration(const dg_member_t *m) {
  return dg_is_flash_audio(m) ? (double)m->samples * 600 / m->rate : m->samples;
}
static inline bool dg_media_sound(const dg_member_t *m) {
  return dg_is_flash_audio(m) || (m->video_flags & 8);
}
#endif
const dg_movie_t *dg_loaded(dg_runtime_t *, unsigned);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
// A text member's colour runs and hyperlinks, NULL when it has none.
const dg_text_extra_t *dg_text_extra(dg_runtime_t *, uint32_t member);
#endif
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
uint32_t dg_film_sound(dg_runtime_t *, unsigned root, unsigned channel, unsigned *pose, unsigned *source);
void dg_films_dispose(dg_runtime_t *);
void dg_film_reset(dg_runtime_t *, unsigned stage_channel);
void dg_films_present(dg_runtime_t *, bool advance_roots);
unsigned dg_film_prepare(dg_runtime_t *, unsigned parent, const dg_sprite_t *, const dg_member_t *);
// Playback/override visual identity only; renderer caches also track mutable
// source members, text, images, masks and palette dependencies. Never presents.
bool dg_film_visual_state(dg_runtime_t *, unsigned parent, const dg_sprite_t *,
                         const dg_member_t *, uint32_t *instance, uint32_t *revision);
lv_t dg_film_tell(dg_runtime_t *, lv_t parent, unsigned channel);
bool dg_film_go(dg_runtime_t *, lv_t parent, unsigned frame);
unsigned dg_film_frame(dg_runtime_t *, lv_t parent, bool last);
bool dg_film_recorded_layer(dg_runtime_t *, const dg_member_t *, unsigned pose, unsigned channel, dg_film_layer_t *);
bool dg_film_child_layer(dg_runtime_t *, unsigned parent, unsigned channel, dg_film_layer_t *);
#endif
#if DG_D10
// NULL until image pixels are changed or assigned. Mutations invalidate the stage.
const dg_pixel_image_t *dg_member_image(const dg_runtime_t *, uint32_t member);
// Cached exterior-white matte for owned mutable images; NULL on failure.
const uint8_t *dg_image_matte(dg_runtime_t *, const dg_pixel_image_t *);
// Direct stage painting lasts until the next ordinary stage change.
const dg_pixel_image_t *dg_stage_image(const dg_runtime_t *);
// Enter/leave the child's shared-heap interpreter context for composition.
dg_runtime_t *dg_linked_begin(dg_runtime_t *, unsigned channel);
void dg_linked_end(dg_runtime_t *);
const dg_runtime_t *dg_linked_movie(const dg_runtime_t *, unsigned channel);
void dg_linked_dispose(dg_runtime_t *);

// Release all owned image storage before discarding or reinitializing a runtime.
void dg_images_dispose(dg_runtime_t *);
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
