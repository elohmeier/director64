#include "mixer.h"
#include "decode.h"
#include "audio_position.h"
#include "flash_audio.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "director64", __VA_ARGS__)

enum { CHANNELS = 8, VIDEO_VOICES = 8, FLASH_VOICES = 64,
       VOICES = CHANNELS + VIDEO_VOICES + FLASH_VOICES, JOBS = 80 };
// Decoded sounds are many times their stored size; keep the recent ones.
#define CACHE_BYTES (48u * 1024u * 1024u)

typedef struct {
  sound_t *sound;
  double position, rate;
  bool looping, paused;
  double paused_at, paused_total;
  double pending_offset, pending_at;
  double requested, release_offset; // active seconds before loop release; -1 while looping
  double loop_start, loop_end; // in decoded frames
  // A start waiting for its sound to decode: the decoder's answer applies
  // only while the voice still waits for it.
  unsigned pending;
#if DG_D10
  bool flash;
  uint32_t flash_rate,flash_start,flash_end;
  uint16_t flash_loops;
  dg_flash_sound_event_t event;
#endif
} voice_t;

typedef struct {
  char *name;
  sound_t *sound;
  unsigned last;
} cached_t;

typedef struct {
  unsigned voice, token;
  const uint8_t *bytes;
  uint64_t offset, length;
  bool looping, stream;
  uint32_t loop_start, loop_end;
  char name[64];
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static voice_t voices[VOICES];
static float gains[VOICES];
static _Atomic float master = 1.0f;
static unsigned video_sprite[VIDEO_VOICES];
#if DG_D10
static uint32_t flash_ids[FLASH_VOICES];
#endif
static unsigned tokens;
static cached_t *cache;
static unsigned cache_count, cache_cap, cache_clock;
static size_t cache_bytes;
static const pack_t *sound_pack, *stream_pack;
static AAudioStream *stream;
static int32_t output_rate = 48000;
static atomic_bool disconnected, suspended;
static double suspended_at, suspended_total; // guarded by lock
static job_t jobs[JOBS];
static unsigned job_count;
static pthread_t worker;
static bool worker_running, stopping;
static char failure[512]; // guarded by lock

static void fail(const char *reason, const char *name) {
  if (!*failure)
    snprintf(failure, sizeof(failure), "%s: %s", reason, name);
}

bool audio_error(char *out, size_t capacity) {
  pthread_mutex_lock(&lock);
  bool failed = *failure != 0;
  if (capacity) snprintf(out, capacity, "%s", failure);
  pthread_mutex_unlock(&lock);
  return failed;
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1e9;
}

// This clock advances only while the whole host is running. Decoder jobs,
// per-channel pauses and loop releases all use the same active-time domain.
static double active_now(void) {
  return (atomic_load(&suspended) ? suspended_at : now()) - suspended_total;
}
static double pending_position(const voice_t *voice, double stamp) {
  return voice->pending_offset + (voice->paused ? 0 : stamp - voice->pending_at) * voice->rate;
}
static void pause_voice(voice_t *voice, bool paused) {
  if (voice->paused == paused) return;
  double stamp = active_now();
  if (voice->pending) {
    voice->pending_offset = pending_position(voice, stamp);
    voice->pending_at = stamp;
  }
  if (paused) voice->paused_at = stamp;
  else voice->paused_total += stamp - voice->paused_at;
  voice->paused = paused;
}

// ---- Output ----

static aaudio_data_callback_result_t render(AAudioStream *s, void *user, void *data,
                                            int32_t frames) {
  (void)s;
  (void)user;
  float *out = data;
  memset(out, 0, (size_t)frames * 2 * sizeof(float));
  pthread_mutex_lock(&lock);
  if (atomic_load(&suspended)) {
    pthread_mutex_unlock(&lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  float level = atomic_load(&master);
  for (unsigned v = 0; v < VOICES; v++) {
    voice_t *voice = &voices[v];
    sound_t *sound = voice->sound;
    if (!sound || voice->paused)
      continue;
    float gain = gains[v] * level;
    double step = voice->rate * sound->rate / output_rate;
    double end = voice->looping ? voice->loop_end : sound->frames;
    const int16_t *pcm = sound->pcm;
    unsigned ch = sound->channels;
#if DG_D10
    if(voice->flash) {
      if(!dg_flash_mix_pcm(pcm,sound->frames,ch,sound->rate,voice->flash_rate,
          voice->flash_start,voice->flash_end,voice->flash_loops,&voice->event,
          &voice->position,out,(unsigned)frames,(unsigned)output_rate,gain)) {
        voice->sound=NULL;sound_release(sound);
      }
      continue;
    }
#endif
    for (int32_t i = 0; i < frames; i++) {
      if (voice->position >= end) {
        if (!voice->looping || voice->loop_end <= voice->loop_start) {
          voice->sound = NULL;
          sound_release(sound);
          break;
        }
        voice->position -= voice->loop_end - voice->loop_start;
      }
      uint32_t at = (uint32_t)voice->position;
      float frac = (float)(voice->position - at);
      uint32_t next = at + 1 < sound->frames ? at + 1 : at;
      float l = pcm[at * ch] + (pcm[next * ch] - pcm[at * ch]) * frac;
      float r = ch == 2 ? pcm[at * 2 + 1] + (pcm[next * 2 + 1] - pcm[at * 2 + 1]) * frac : l;
      out[i * 2] += l * (gain / 32768.0f);
      out[i * 2 + 1] += r * (gain / 32768.0f);
      voice->position += step;
    }
  }
  pthread_mutex_unlock(&lock);
  for (int32_t i = 0; i < frames * 2; i++)
    out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void failed(AAudioStream *s, void *user, aaudio_result_t error) {
  (void)s;
  (void)user;
  LOG("audio output error %d", error);
  atomic_store(&disconnected, true);
}

static void open_output(void) {
  AAudioStreamBuilder *builder;
  if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
    return;
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(builder, 2);
  AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
  AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SONIFICATION);
  AAudioStreamBuilder_setDataCallback(builder, render, NULL);
  AAudioStreamBuilder_setErrorCallback(builder, failed, NULL);
  if (AAudioStreamBuilder_openStream(builder, &stream) != AAUDIO_OK) {
    LOG("no audio output");
    stream = NULL;
  } else {
    output_rate = AAudioStream_getSampleRate(stream);
    AAudioStream_requestStart(stream);
    LOG("audio output %d Hz", output_rate);
  }
  AAudioStreamBuilder_delete(builder);
}

static void close_output(void) {
  if (stream) {
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream = NULL;
  }
}

void audio_poll(void) {
  if (atomic_exchange(&disconnected, false)) {
    close_output();
    open_output();
  }
}

void audio_suspend(bool on) {
  // The callback outputs silence while held. It never advances voice
  // positions, and no UI thread waits for an AAudio control operation.
  pthread_mutex_lock(&lock);
  if (atomic_load(&suspended) != on) {
    double stamp = now();
    if (on) suspended_at = stamp;
    else suspended_total += stamp - suspended_at;
    atomic_store(&suspended, on);
  }
  pthread_mutex_unlock(&lock);
}

// ---- Sounds ----

// The decoded sound of a cached name, retained; NULL when not cached.
static sound_t *cached(const char *name) {
  for (unsigned i = 0; i < cache_count; i++)
    if (!strcmp(cache[i].name, name)) {
      cache[i].last = ++cache_clock;
      return sound_retain(cache[i].sound);
    }
  return NULL;
}

static void keep(const char *name, sound_t *sound) {
  if (cache_count == cache_cap) {
    unsigned cap = cache_cap ? cache_cap * 2 : 64;
    cached_t *grown = realloc(cache, cap * sizeof(cached_t));
    if (!grown)
      return;
    cache = grown;
    cache_cap = cap;
  }
  cache[cache_count++] = (cached_t){strdup(name), sound_retain(sound), ++cache_clock};
  cache_bytes += sound_bytes(sound);
  while (cache_bytes > CACHE_BYTES && cache_count > 1) {
    unsigned oldest = 0;
    for (unsigned i = 1; i < cache_count; i++)
      if (cache[i].last < cache[oldest].last)
        oldest = i;
    cache_bytes -= sound_bytes(cache[oldest].sound);
    sound_release(cache[oldest].sound);
    free(cache[oldest].name);
    cache[oldest] = cache[--cache_count];
  }
}

// Starts a voice, `offset` seconds in. Called with the lock held.
static void start(unsigned v, sound_t *sound, bool looping, uint32_t loop_start,
                  uint32_t loop_end, double offset) {
  voice_t *voice = &voices[v];
  if (voice->sound)
    sound_release(voice->sound);
  voice->sound = NULL;
  voice->pending = 0;
#if DG_D10
  if(voice->flash) {
    double duration=(double)(voice->flash_end-voice->flash_start)*voice->flash_loops/voice->flash_rate;
    if(offset>=duration){sound_release(sound);return;}
    voice->position=offset*sound->rate;voice->rate=1;voice->sound=sound;return;
  }
#endif
  audio_start_position_t at = audio_start_position(sound->frames, sound->rate,
      looping, loop_start, loop_end, offset, voice->release_offset);
  voice->looping = at.looping;
  voice->loop_start = at.loop_start;
  voice->loop_end = at.loop_end;
  if (!at.looping && at.position >= sound->frames) { sound_release(sound); return; }
  voice->position = at.position;
  voice->rate = voice->rate > 0 ? voice->rate : 1.0;
  voice->sound = sound;
}

static void stop_voice(unsigned v) {
  voice_t *voice = &voices[v];
  voice->pending = 0;
  voice->paused = false;
  voice->paused_total = 0;
  voice->release_offset = -1;
  if (voice->sound)
    sound_release(voice->sound);
  voice->sound = NULL;
#if DG_D10
  free((void *)voice->event.envelopes);memset(&voice->event,0,sizeof(voice->event));voice->flash=false;
#endif
}

static void *decoder(void *arg) {
  (void)arg;
  pthread_mutex_lock(&lock);
  while (!stopping) {
    if (!job_count) {
      pthread_cond_wait(&wake, &lock);
      continue;
    }
    job_t job = jobs[0];
    memmove(jobs, jobs + 1, --job_count * sizeof(job_t));
    if (voices[job.voice].pending != job.token)
      continue;
    pthread_mutex_unlock(&lock);
    sound_t *sound = NULL;
    char decode_error[160] = "could not decode sound";
    if (job.bytes && is_wav(job.bytes, job.length))
      sound = decode_wav(job.bytes, job.length);
    else
      sound = decode_media(job.stream ? stream_pack->fd : sound_pack->fd, job.offset,
                           job.length, decode_error, sizeof(decode_error));
    if (!sound)
      LOG("could not decode %s", job.name);
    pthread_mutex_lock(&lock);
    if (!sound && voices[job.voice].pending == job.token)
      fail(decode_error, job.name);
    if (sound && !job.stream)
      keep(job.name, sound);
    if (sound && voices[job.voice].pending == job.token)
      start(job.voice, sound_retain(sound), job.looping, job.loop_start, job.loop_end,
            pending_position(&voices[job.voice], active_now()));
    sound_release(sound);
  }
  pthread_mutex_unlock(&lock);
  return NULL;
}

// Plays cached entries immediately; other sounds decode on the worker.
// Their eventual position includes only active, unpaused playback time.
static void play_entry(unsigned v, const pack_t *pack, const pack_entry_t *entry,
                       const char *name, bool stream_entry, bool looping, uint32_t loop_start,
                       uint32_t loop_end, double offset) {
  sound_t *sound = stream_entry ? NULL : cached(name);
  const uint8_t *bytes = pack_data(pack, entry);
  if (sound) {
    start(v, sound, looping, loop_start, loop_end, offset);
    return;
  }
  if (job_count == JOBS) {
    fail("audio decode queue full", name);
    return;
  }
  unsigned token = ++tokens ? tokens : ++tokens;
  voices[v].pending = token;
  voices[v].requested = active_now();
  voices[v].pending_at = voices[v].requested;
  voices[v].pending_offset = offset;
  job_t *job = &jobs[job_count++];
  *job = (job_t){.voice = v, .token = token, .bytes = bytes, .offset = pack->base + entry->offset,
                 .length = entry->length, .looping = looping, .stream = stream_entry,
                 .loop_start = loop_start, .loop_end = loop_end};
  snprintf(job->name, sizeof(job->name), "%s", name);
  pthread_cond_signal(&wake);
}

void audio_init(const pack_t *sounds, const pack_t *streams) {
  pthread_mutex_lock(&lock);
  failure[0] = 0;
  atomic_store(&suspended, false);
  suspended_at = suspended_total = 0;
  atomic_store(&disconnected, false);
  sound_pack = sounds;
  stream_pack = streams;
  for (unsigned i = 0; i < VOICES; i++) {
    gains[i] = 1.0f;
    voices[i].rate = 1.0;
  }
  stopping = false;
  worker_running = pthread_create(&worker, NULL, decoder, NULL) == 0;
  if (!worker_running) fail("could not start audio decoder", "worker");
  pthread_mutex_unlock(&lock);
  open_output();
}

void audio_shutdown(void) {
  close_output();
  pthread_mutex_lock(&lock);
  stopping = true;
  job_count = 0;
  pthread_cond_broadcast(&wake);
  pthread_mutex_unlock(&lock);
  if (worker_running)
    pthread_join(worker, NULL);
  worker_running = false;
  for (unsigned i = 0; i < VOICES; i++)
    stop_voice(i);
  for (unsigned i = 0; i < cache_count; i++) {
    sound_release(cache[i].sound);
    free(cache[i].name);
  }
  free(cache);
  cache = NULL;
  cache_count = cache_cap = 0;
  cache_bytes = 0;
  memset(video_sprite, 0, sizeof(video_sprite));
#if DG_D10
  memset(flash_ids, 0, sizeof(flash_ids));
#endif
}

void audio_play(unsigned channel, const char *name, bool looping, uint32_t loop_start,
                uint32_t loop_end) {
  if (channel >= CHANNELS)
    return;
  pthread_mutex_lock(&lock);
  stop_voice(channel);
  const pack_entry_t *entry = name ? pack_find(sound_pack, name) : NULL;
  if (entry)
    play_entry(channel, sound_pack, entry, name, false, looping, loop_start, loop_end, 0);
  else if (name) {
    LOG("missing sound %s", name);
    fail("missing sound", name);
  }
  pthread_mutex_unlock(&lock);
}

uint32_t audio_play_stream(unsigned channel, const char *name) {
  if (channel >= CHANNELS)
    return 0;
  pthread_mutex_lock(&lock);
  stop_voice(channel);
  const pack_entry_t *entry = name ? pack_find(stream_pack, name) : NULL;
  if (entry)
    play_entry(channel, stream_pack, entry, name, true, false, 0, 0, 0);
  else if (name) {
    LOG("missing stream %s", name);
    fail("missing sound stream", name);
  }
  pthread_mutex_unlock(&lock);
  return entry ? entry->extra : 0;
}

void audio_pause(unsigned channel, bool paused) {
  if (channel >= CHANNELS)
    return;
  pthread_mutex_lock(&lock);
  voice_t *voice = &voices[channel];
  if (voice->sound || voice->pending) pause_voice(voice, paused);
  pthread_mutex_unlock(&lock);
}

void audio_stop(unsigned channel) {
  if (channel >= CHANNELS)
    return;
  pthread_mutex_lock(&lock);
  stop_voice(channel);
  pthread_mutex_unlock(&lock);
}
void audio_break_loop(unsigned channel) {
  if (channel >= CHANNELS) return;
  pthread_mutex_lock(&lock);
  voice_t *voice = &voices[channel];
  if (voice->pending && voice->release_offset < 0)
    voice->release_offset = fmax(0, (voice->paused ? voice->paused_at : active_now()) - voice->requested - voice->paused_total);
  if (voice->sound && voice->looping) {
    if (voice->position >= voice->loop_end && voice->loop_end > voice->loop_start)
      voice->position = voice->loop_start + fmod(voice->position - voice->loop_start, voice->loop_end - voice->loop_start);
    voice->looping = false;
  }
  pthread_mutex_unlock(&lock);
}

void audio_gain(unsigned channel, float gain) {
  pthread_mutex_lock(&lock);
  if (channel < CHANNELS) gains[channel] = gain;
  pthread_mutex_unlock(&lock);
}

void audio_volume(float volume) { atomic_store(&master, volume); }

void audio_stop_all(void) {
  pthread_mutex_lock(&lock);
  for (unsigned i = 0; i < VOICES; i++)
    stop_voice(i);
  memset(video_sprite, 0, sizeof(video_sprite));
#if DG_D10
  memset(flash_ids,0,sizeof(flash_ids));
#endif
  pthread_mutex_unlock(&lock);
}

double audio_duration(const char *name) {
  const pack_entry_t *entry = pack_find(sound_pack, name);
  return entry ? wav_duration(pack_data(sound_pack, entry), entry->length) : 0;
}

// The voice that plays a video sprite's sound; `claim` takes a free one.
static int video_voice(unsigned sprite, bool claim) {
  for (unsigned i = 0; i < VIDEO_VOICES; i++)
    if (video_sprite[i] == sprite)
      return CHANNELS + (int)i;
  if (!claim)
    return -1;
  for (unsigned i = 0; i < VIDEO_VOICES; i++)
    if (!video_sprite[i]) {
      video_sprite[i] = sprite;
      return CHANNELS + (int)i;
    }
  return -1;
}

void audio_play_video(unsigned sprite, const char *name, double offset) {
  pthread_mutex_lock(&lock);
  int v = video_voice(sprite, true);
  const pack_entry_t *entry = pack_find(sound_pack, name);
  if (v >= 0) {
    stop_voice((unsigned)v);
    if (entry)
      play_entry((unsigned)v, sound_pack, entry, name, false, false, 0, 0, offset);
  }
  pthread_mutex_unlock(&lock);
}

void audio_video_rate(unsigned sprite, double rate, float gain) {
  pthread_mutex_lock(&lock);
  int v = video_voice(sprite, false);
  if (v >= 0) {
    if (voices[v].pending) {
      double stamp = active_now();
      voices[v].pending_offset = pending_position(&voices[v], stamp);
      voices[v].pending_at = stamp;
    }
    voices[v].rate = rate > 0 ? rate : 1.0;
    gains[v] = gain;
  }
  pthread_mutex_unlock(&lock);
}

void audio_stop_video(unsigned sprite) {
  pthread_mutex_lock(&lock);
  int v = video_voice(sprite, false);
  if (v >= 0) {
    stop_voice((unsigned)v);
    video_sprite[v - CHANNELS] = 0;
  }
  pthread_mutex_unlock(&lock);
}
#if DG_D10
void audio_flash_update(uint32_t id,const dg_flash_audio_t *request) {
  pthread_mutex_lock(&lock);
  unsigned slot=0;while(slot<FLASH_VOICES&&flash_ids[slot]!=id)slot++;
  if(!request) {
    if(slot<FLASH_VOICES){stop_voice(CHANNELS+VIDEO_VOICES+slot);flash_ids[slot]=0;}
    pthread_mutex_unlock(&lock);return;
  }
  bool fresh=slot==FLASH_VOICES;
  if(fresh){slot=0;while(slot<FLASH_VOICES&&flash_ids[slot])slot++;}
  if(slot==FLASH_VOICES){fail("concurrent Flash event sound capacity exceeded","");pthread_mutex_unlock(&lock);return;}
  unsigned index=CHANNELS+VIDEO_VOICES+slot;voice_t *voice=&voices[index];
  if(fresh) {
    const pack_entry_t *entry=pack_find(sound_pack,request->sound->asset);
    if(!entry){fail("missing Flash event sound",request->sound->asset);pthread_mutex_unlock(&lock);return;}
    stop_voice(index);flash_ids[slot]=id;voice->flash=true;
    voice->flash_rate=request->sound->rate;voice->flash_start=request->start_sample;
    voice->flash_end=request->end_sample;voice->flash_loops=request->loops;
    if(request->event)voice->event=*request->event;
    voice->event.envelopes=NULL;
    if(voice->event.envelope_count) {
      dg_flash_envelope_t *points=malloc(voice->event.envelope_count*sizeof(*points));
      if(!points){fail("Flash event envelope allocation failed","");flash_ids[slot]=0;pthread_mutex_unlock(&lock);return;}
      memcpy(points,request->event->envelopes,voice->event.envelope_count*sizeof(*points));voice->event.envelopes=points;
    }
    play_entry(index,sound_pack,entry,request->sound->asset,false,false,0,0,
        (double)request->position/request->sound->rate);
  }
  gains[index]=request->gain;
  pause_voice(voice, request->paused);
  pthread_mutex_unlock(&lock);
}
#endif
