#ifndef DIRECTOR64_FILM_CHANNELS_H
#define DIRECTOR64_FILM_CHANNELS_H
#include "director.h"
#if DG_D10
// Native films usually occupy only a few of the thousand possible channels.
// Keep the union in a small stack bitset, preserving ascending draw order and
// including script-created channels without scanning every absent channel.
typedef struct {
  uint32_t bits[(DG_SPRITES + 31) / 32];
  unsigned word;
} dg_film_channels_t;
static inline void dg_film_channels_add(dg_film_channels_t *set,unsigned channel) {
  if(channel && channel<DG_SPRITES)set->bits[channel>>5]|=UINT32_C(1)<<(channel&31);
}
static inline dg_film_channels_t dg_film_channels(const dg_runtime_t *d,
    const dg_member_t *member,unsigned pose,unsigned parent) {
  dg_film_channels_t set={0};
  for(unsigned i=0;i<member->film_track_count;i++)
    dg_film_channels_add(&set,member->film_tracks[i].channel);
  if(member->film_frames && pose<member->film_count) {
    const dg_film_frame_t *frame=&member->film_frames[pose];
    for(unsigned i=0;i<frame->count;i++)dg_film_channels_add(&set,frame->layers[i].channel);
  }
  if(parent)for(unsigned i=0;i<d->film_child_count;i++) {
    const dg_film_child_t *child=&d->film_children[i];
    if(child->parent==parent && child->owner==member->id)dg_film_channels_add(&set,child->channel);
  }
  return set;
}
static inline unsigned dg_film_channels_next(dg_film_channels_t *set) {
  while(set->word<sizeof(set->bits)/sizeof(*set->bits)) {
    uint32_t bits=set->bits[set->word];
    if(bits) {
      unsigned channel=set->word*32+(unsigned)__builtin_ctz(bits);
      set->bits[set->word]=bits&(bits-1);
      return channel;
    }
    set->word++;
  }
  return 0;
}
#endif
#endif
