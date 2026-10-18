#include "film_track.h"
#include "film_channels.h"
// Director film subchannels expose their authored coordinates. The compositor
// applies the film rectangle origin when placing that plane in its parent.
// Only changed properties override the recorded frame; siblings and subsequent
// authored poses remain live. Empty cast slots retain their address/geometry.
#define FILM_NODE_PARENT 0x8000u
#define FILM_NODE_REF 0x20000000u
#define FILM_NODE_LIMIT 512
#define FILM_CACHE_LIMIT (1024u * 1024u)
typedef struct {
  dg_spec_t value;
  uint32_t offset;
  uint16_t key, frame, pose;
} film_cursor_t;
typedef struct {
  dg_film_layer_t layer;
  uint32_t generation, revision;
} film_visual_t;
typedef struct {
  uint32_t member, generation;
  uint32_t visual_revision, staged_revision;
  uint16_t parent, channel, root, pose;
  bool presented, paused;
  film_cursor_t *cursors;
  unsigned cursor_count;
  film_visual_t *visuals;
  unsigned visual_count, visual_capacity;
  bool visual_valid;
} film_node_t;
struct dg_film_playback {
  film_node_t nodes[FILM_NODE_LIMIT];
  unsigned cache_bytes;
  uint32_t generation;
};
static film_node_t *film_node(dg_runtime_t *d, unsigned id) {
  return d->film_playback && id && id <= FILM_NODE_LIMIT &&
      d->film_playback->nodes[id-1].member ? &d->film_playback->nodes[id-1] : NULL;
}
static unsigned film_node_id(dg_runtime_t *d, const film_node_t *node) {
  return (unsigned)(node-d->film_playback->nodes)+1;
}
static unsigned film_parent_id(dg_runtime_t *d, unsigned parent) {
  return parent & FILM_NODE_PARENT ? parent & ~FILM_NODE_PARENT :
      parent < DG_SPRITES ? d->sprites[parent].film_instance : 0;
}
static uint32_t film_parent_member(dg_runtime_t *d, unsigned parent) {
  if (!(parent & FILM_NODE_PARENT)) return parent < DG_SPRITES ? d->sprites[parent].value.member : 0;
  film_node_t *node = film_node(d, parent & ~FILM_NODE_PARENT);
  return node ? node->member : 0;
}
static bool film_sync_node(dg_runtime_t *, unsigned, bool, unsigned);
static film_node_t *film_root(dg_runtime_t *, unsigned);
static void film_visual_touch(dg_runtime_t *d,film_node_t *node) {
  while(node) {
    node->visual_revision++;
    node=film_node(d,node->parent);
  }
}
static void film_visual_changed(dg_runtime_t *d,unsigned root) {
  dg_sprite_changed(d,root);
  // Root sprite comparison cannot see independent descendant playheads or
  // replacements. Invalidate their composed plane, including linked hosts.
  for(dg_runtime_t *owner=d;owner;owner=owner->linked_parent) {
    owner->visual_revision++;
    owner->mouse_hit_valid=false;
  }
}
static unsigned film_child_parent(lv_t sprite) {
  unsigned id=(uint32_t)lv_id(sprite);
  return ((id >> DG_CHILD_SHIFT) & DG_CHILD_CHANNEL_MASK) | (id & FILM_NODE_REF ? FILM_NODE_PARENT : 0);
}
static unsigned film_child_channel(lv_t sprite) {
  return (uint32_t)lv_id(sprite) & DG_CHILD_CHANNEL_MASK;
}
static dg_film_child_t *film_child_find(dg_runtime_t *d, unsigned parent, unsigned channel) {
  if (!parent || !channel || channel >= DG_SPRITES) return NULL;
  for (unsigned i = 0; i < d->film_child_count; i++) {
    dg_film_child_t *child = &d->film_children[i];
    if (child->parent == parent && child->channel == channel &&
        child->owner == film_parent_member(d, parent)) return child;
  }
  return NULL;
}
static bool film_recorded_layer(dg_runtime_t *d, const dg_member_t *film, unsigned pose,
    unsigned channel, dg_film_layer_t *out, film_node_t *node) {
  *out = (dg_film_layer_t){.channel = (uint16_t)channel, .value = {
      .blend = 100, .back_rgb = 0xffffff, .flags = 48,
      .thickness = DG_HAS_BLEND, .loc_z = (int16_t)channel}};
  if (!film || film->type != 2 || film->source_xtra || !channel || channel >= DG_SPRITES) return false;
  if (film->film_tracks) {
    unsigned low = 0, high = film->film_track_count;
    while (low < high) {
      unsigned mid = low + (high - low) / 2;
      if (film->film_tracks[mid].channel < channel) low = mid + 1; else high = mid;
    }
    if (low == film->film_track_count || film->film_tracks[low].channel != channel) return false;
    const dg_film_track_t *track = &film->film_tracks[low];
    film_cursor_t temporary = {0}, *cursor = &temporary;
    if (node) {
      if (!node->cursors) {
        unsigned bytes = film->film_track_count * sizeof(*node->cursors);
        if (bytes > FILM_CACHE_LIMIT-d->film_playback->cache_bytes) {
          error(d, "film channel state", "memory budget exceeded"); return false;
        }
        node->cursors = calloc(film->film_track_count, sizeof(*node->cursors));
        if (!node->cursors) { error(d, "film channel state", "allocation failed"); return false; }
        node->cursor_count=film->film_track_count; d->film_playback->cache_bytes+=bytes;
      }
      cursor=&node->cursors[low];
      if (pose < cursor->pose) *cursor=(film_cursor_t){0};
    }
    while (cursor->key < track->count) {
      uint32_t offset=cursor->offset;
      uint16_t key_frame;
      dg_spec_t next=cursor->value;
      if (!dg_film_track_read(track,&offset,&key_frame,&next) ||
          key_frame >= film->film_count || (cursor->key && key_frame <= cursor->frame)) {
        error(d,"film track","invalid key data"); return false;
      }
      if (key_frame > pose) break;
      cursor->value=next;cursor->offset=offset;cursor->frame=key_frame;cursor->key++;
    }
    if (cursor->key == track->count && cursor->offset != track->size) {
      error(d,"film track","trailing key data"); return false;
    }
    cursor->pose=(uint16_t)pose;
    out->value=cursor->value;
    const dg_member_t *child = dg_member(d, out->value.member);
    if (child && child->film_count) {
      unsigned frame = child->film_loop ? pose % child->film_count :
          pose < child->film_count ? pose : child->film_count - 1u;
      if (child->flash_stop_count && child->flash_stops[0] &&
          frame >= child->flash_stops[0]) frame = child->flash_stops[0] - 1;
      out->film_frame = (uint16_t)frame;
    }
    return true;
  }
  if (film->film_frames && pose < film->film_count) {
    const dg_film_frame_t *frame = &film->film_frames[pose];
    for (unsigned i = 0; i < frame->count; i++)
      if (frame->layers[i].channel == channel) { *out = frame->layers[i]; return true; }
  }
  return false;
}
bool dg_film_recorded_layer(dg_runtime_t *d, const dg_member_t *film, unsigned pose,
    unsigned channel, dg_film_layer_t *out) {
  return film_recorded_layer(d,film,pose,channel,out,NULL);
}
static bool film_native_member(dg_runtime_t *d,uint32_t id) {
  const dg_member_t *member=dg_member(d,id);
  return member && member->type==2 && !member->source_xtra && member->film_count;
}
bool dg_film_child_layer(dg_runtime_t *d, unsigned parent, unsigned channel, dg_film_layer_t *out) {
  if (!parent || !channel || channel >= DG_SPRITES) return false;
  const dg_member_t *film = dg_member(d, film_parent_member(d,parent));
  if (!film || film->type != 2 || film->source_xtra) return false;
  film_node_t *node=film_node(d,film_parent_id(d,parent));
  unsigned pose=parent < DG_SPRITES ? dg_film_storage_pose(film,&d->sprites[parent]) : node ? node->pose : 0;
  if(node)node->pose=(uint16_t)pose;
  bool recorded=film_recorded_layer(d,film,pose,channel,out,node);
  const dg_film_child_t *child = film_child_find(d, parent, channel);
  if (!child) {
    if (node && film_native_member(d,out->value.member)) for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
      film_node_t *nested=&d->film_playback->nodes[i];
      if(nested->member && nested->parent==film_node_id(d,node) && nested->channel==channel) {
        out->film_frame=nested->pose;out->placement_generation=i+1;break;
      }
    }
    return recorded;
  }
  dg_spec_t *v = &out->value;
  const dg_spec_t *edit = &child->value;
  unsigned mask = child->mask;
  if (mask & DG_MEMBER) {
    if (v->member != edit->member) out->film_frame = 0;
    v->member = edit->member;
    v->type = v->member ? 16 : 0;
    if (!v->stretch) {
      const dg_member_t *member = dg_member(d, v->member);
      const dg_pixel_image_t *image = dg_member_image(d, v->member);
      v->width = image ? image->width : member ? member->width : 0;
      v->height = image ? image->height : member ? member->height : 0;
    }
  }
  if (mask & DG_POSITION) { v->x = edit->x; v->y = edit->y; }
  if (mask & DG_WIDTH) v->width = edit->width;
  if (mask & DG_HEIGHT) v->height = edit->height;
  if (mask & DG_CHILD_STRETCH) v->stretch = edit->stretch;
  if (mask & DG_INK) v->ink = edit->ink;
  if (mask & DG_BLEND) { v->blend = edit->blend; v->thickness |= DG_HAS_BLEND; }
  if (mask & DG_FORE) { v->fore = edit->fore; v->fore_rgb = edit->fore_rgb; v->flags = (v->flags & ~16) | (edit->flags & 16); }
  if (mask & DG_BACK) { v->back = edit->back; v->back_rgb = edit->back_rgb; v->flags = (v->flags & ~32) | (edit->flags & 32); }
  if (mask & DG_TYPE) v->type = edit->type;
  if (mask & DG_ROTATION) v->rotation = edit->rotation;
  if (mask & DG_SKEW) v->skew = edit->skew;
  if ((mask & DG_CHILD_VISIBLE) && !child->visible) v->type = 0;
  if (node && film_native_member(d,v->member)) for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
    film_node_t *nested=&d->film_playback->nodes[i];
    if(nested->member==v->member && nested->parent==film_node_id(d,node) && nested->channel==channel) {
      out->film_frame=nested->pose;out->placement_generation=i+1;break;
    }
  }
  return true;
}
static bool film_layer_visible(dg_runtime_t *d,const dg_film_layer_t *layer) {
  const dg_spec_t *v=&layer->value;
  dg_sprite_t sprite={.value=*v};
  bool direct=layer->asset&&*layer->asset;
  return v->type&&(dg_opacity(&sprite)||
      (!direct&&dg_film_container_effects(dg_member(d,v->member))))&&
      (v->member||v->type<16||(layer->asset&&*layer->asset)||(layer->name&&*layer->name));
}
static bool film_string_equal(const char *a,const char *b) {
  return a==b||!strcmp(a?a:"",b?b:"");
}
static bool film_visual_equal(dg_runtime_t *d,const dg_film_layer_t *a,const dg_film_layer_t *b) {
  const dg_spec_t *x=&a->value,*y=&b->value;
  bool visible=film_layer_visible(d,a);
  if(visible!=film_layer_visible(d,b))return false;
  if(!visible)return true;
  dg_sprite_t sx={.value=*x},sy={.value=*y};
  // Explicit fields avoid C structure padding and exclude score-only data.
#define SAME(field) (x->field==y->field)
  if(!(SAME(member)&&SAME(x)&&SAME(y)&&SAME(ink)&&SAME(type)&&SAME(flags)&&
       SAME(fore)&&SAME(back)&&SAME(stretch)&&SAME(rotation)&&SAME(skew)))return false;
  // sprite_rect ignores spec size only when an unstretched member supplies
  // its own dimensions. Primitive shapes and direct planes have no such
  // source: their authored width/height remain visible even without stretch.
  if(!(SAME(width)&&SAME(height))) {
    if(x->stretch||(a->asset&&*a->asset))return false;
    const dg_member_t *member=dg_member(d,x->member);
    bool member_size=member&&((member->width&&member->height)||member->type==1);
    if(!member_size)return false;
  }
#undef SAME
  if(dg_opacity(&sx)!=dg_opacity(&sy)||(x->thickness&15)!=(y->thickness&15))return false;
  if((x->flags&16)&&x->fore_rgb!=y->fore_rgb)return false;
  if((x->flags&32)&&x->back_rgb!=y->back_rgb)return false;
  return a->channel==b->channel&&a->film_frame==b->film_frame&&
      film_string_equal(a->asset,b->asset)&&film_string_equal(a->name,b->name)&&
      film_string_equal(a->mask_asset,b->mask_asset)&&
      a->origin_x==b->origin_x&&a->origin_y==b->origin_y&&a->affine==b->affine&&
      (!a->affine||!memcmp(a->matrix,b->matrix,sizeof(a->matrix)))&&
      a->has_cxform==b->has_cxform&&
      (!a->has_cxform||!memcmp(a->cxform,b->cxform,sizeof(a->cxform)));
}
static bool film_visual_refresh(dg_runtime_t *d,film_node_t *node) {
  const dg_member_t *member=dg_member(d,node->member);
  unsigned parent=node->parent?FILM_NODE_PARENT|film_node_id(d,node):node->channel;
  dg_film_channels_t channels=dg_film_channels(d,member,node->pose,parent);
  unsigned capacity=0;
  for(unsigned i=0;i<sizeof(channels.bits)/sizeof(*channels.bits);i++)
    capacity+=(unsigned)__builtin_popcount(channels.bits[i]);
  if(capacity>node->visual_capacity) {
    unsigned extra=(capacity-node->visual_capacity)*sizeof(*node->visuals);
    if(extra>FILM_CACHE_LIMIT-d->film_playback->cache_bytes) {
      error(d,"film visual state","memory budget exceeded");return false;
    }
    film_visual_t *grown=realloc(node->visuals,capacity*sizeof(*grown));
    if(!grown){error(d,"film visual state","allocation failed");return false;}
    node->visuals=grown;node->visual_capacity=capacity;
    d->film_playback->cache_bytes+=extra;
  }
  bool changed=!node->visual_valid;
  unsigned count=0;
  for(unsigned channel;(channel=dg_film_channels_next(&channels));) {
    film_visual_t state={0};
    if(!dg_film_child_layer(d,parent,channel,&state.layer))continue;
    dg_spec_t *value=&state.layer.value;
    // Empty template channels are addressable, but do not contribute pixels.
    if(!film_layer_visible(d,&state.layer))continue;
    if(film_native_member(d,value->member)) {
      film_node_t *child=film_node(d,state.layer.placement_generation);
      if(child) {
        state.generation=child->generation;state.revision=child->visual_revision;
        const dg_member_t *nested=dg_member(d,value->member);
        // Retained child frames/tracks have their own semantic visual state.
        // Legacy baked planes still select an image directly by pose.
        if(nested->film_tracks||nested->film_frames)state.layer.film_frame=0;
      }
    }
    if(count>=node->visual_count||!film_visual_equal(d,&node->visuals[count].layer,&state.layer)||
        node->visuals[count].generation!=state.generation||node->visuals[count].revision!=state.revision)
      changed=true;
    node->visuals[count++]=state;
  }
  changed|=count!=node->visual_count;
  node->visual_count=count;node->visual_valid=true;
  if(changed)node->visual_revision++;
  return changed;
}
static const dg_member_t *film_child_context(dg_runtime_t *d, lv_t sprite) {
  unsigned parent = film_child_parent(sprite);
  const dg_member_t *film = dg_member(d,film_parent_member(d,parent));
  if (!film || film->type != 2 || film->source_xtra || !film_child_channel(sprite) ||
      film_child_channel(sprite) >= DG_SPRITES) {
    error(d, "film child reference", "outside an active Director film loop");
    return NULL;
  }
  return film;
}
static lv_t film_child_get(dg_runtime_t *d, unsigned ln, lv_t sprite) {
  lv_runtime_t *r = d->values;
  const dg_member_t *film = film_child_context(d, sprite);
  if (!film) return (lv_t){0};
  dg_film_layer_t layer;
  film_node_t *node=film_node(d,film_parent_id(d,film_child_parent(sprite)));
  if(node&&!node->presented)layer=(dg_film_layer_t){0};
  else dg_film_child_layer(d, film_child_parent(sprite), film_child_channel(sprite), &layer);
  const dg_spec_t *v = &layer.value;
  dg_sprite_t effective = {.value = *v, .stretch = v->stretch, .visible = true};
  int left, top, right, bottom;
  dg_sprite_bounds(d, &effective, &left, &top, &right, &bottom);
  if (ln == LN_MEMBER) return lv_make(LV_MEMBER, (int32_t)v->member);
  if (ln == LN_CASTNUM) return lv_num(v->member ? ((member_cast(d, v->member)-1)<<17) | (v->member&65535) : 0);
  if (ln == LN_CASTLIBNUM) return lv_num(v->member ? member_cast(d, v->member) : 0);
  if (ln == LN_MEMBERNUM) return lv_num(v->member & 65535);
  if (ln == LN_SPRITENUM) return lv_num(film_child_channel(sprite));
  if (ln == LN_LOC) return lv_point(r, v->x, v->y);
  if (ln == LN_LOCH) return lv_num(v->x);
  if (ln == LN_LOCV) return lv_num(v->y);
  if (ln == LN_INK) return lv_num(v->ink);
  if (ln == LN_BLEND) return lv_num(v->blend);
  if (ln == LN_WIDTH || ln == LN_HEIGHT) {
    int l, t, rr, b; sprite_rect(d, &effective, &l, &t, &rr, &b);
    return lv_num(ln == LN_WIDTH ? rr - l : b - t);
  }
  if (ln == LN_LEFT) return lv_num(left);
  if (ln == LN_TOP) return lv_num(top);
  if (ln == LN_RIGHT) return lv_num(right);
  if (ln == LN_BOTTOM) return lv_num(bottom);
  if (ln == LN_RECT) return lv_rect(r, left, top, right, bottom);
  if (ln == LN_PUPPET) {
    const dg_film_child_t *c = film_child_find(d, film_child_parent(sprite), film_child_channel(sprite));
    return lv_num(c && c->puppet);
  }
  if (ln == LN_STRETCH) return lv_num(v->stretch);
  if (ln == LN_TYPE) return lv_num(v->type);
  if (ln == LN_ROTATION) return lv_num(v->rotation / 100.0);
  if (ln == LN_SKEW) return lv_num(v->skew / 100.0);
  if (ln == LN_VISIBLE) {
    const dg_film_child_t *c = film_child_find(d, film_child_parent(sprite), film_child_channel(sprite));
    return lv_num(!c || !(c->mask & DG_CHILD_VISIBLE) || c->visible);
  }
  if (ln == LN_COLOR || ln == LN_BGCOLOR) {
    bool fore = ln == LN_COLOR;
    return v->flags & (fore ? 16 : 32)
        ? lv_make(LV_COLOR, fore ? v->fore_rgb : v->back_rgb) : lv_num(fore ? v->fore : v->back);
  }
  error(d, "film child property read", "unsupported property");
  return (lv_t){0};
}
static void film_child_set(dg_runtime_t *d, unsigned ln, lv_t sprite, lv_t value) {
  lv_runtime_t *r = d->values;
  const dg_member_t *film = film_child_context(d, sprite);
  if (!film) return;
  unsigned parent = film_child_parent(sprite), channel = film_child_channel(sprite);
  dg_film_layer_t layer;
  dg_film_child_layer(d, parent, channel, &layer);
  dg_film_child_t *child = film_child_find(d, parent, channel);
  if (!child) {
    if (d->film_child_count == DG_FILM_CHILDREN) {
      error(d, "film loop channel capacity", "exceeded"); return;
    }
    child = &d->film_children[d->film_child_count++];
    *child = (dg_film_child_t){.parent = (uint16_t)parent, .channel = (uint16_t)channel,
        .owner = film_parent_member(d,parent), .value = layer.value, .visible = true};
  }
  dg_spec_t *v = &child->value;
  if (ln == LN_MEMBER || ln == LN_MEMBERNUM || ln == LN_CASTNUM) {
    lv_t library = {0};
    if (ln == LN_MEMBERNUM && lv_type(value) == LV_NUMBER && lv_integer(r, value) < 131072)
      library = lv_num(layer.value.member ? member_cast(d, layer.value.member) : 1);
    v->member = (uint32_t)lv_id(reference(r, "member", value, library));
    v->type = v->member ? 16 : 0;
    if (!layer.value.stretch) {
      const dg_member_t *m = dg_member(d, v->member);
      const dg_pixel_image_t *image = dg_member_image(d, v->member);
      v->width = image ? image->width : m ? m->width : 0;
      v->height = image ? image->height : m ? m->height : 0;
    }
    child->mask |= DG_MEMBER;
  } else if (ln == LN_PUPPET) {
    child->puppet = lv_truth(r, value);
    child->mask = child->puppet ? DG_ALL | DG_CHILD_STRETCH : 0;
    child->value = layer.value;
  } else if (ln == LN_LOC) {
    if (lv_type(value) != LV_LIST || lv_count(r, value) != 2) {
      error(d, "film child loc", "requires a point"); return;
    }
    v->x = (int16_t)(lv_integer(r, lv_at(r, value, 1)));
    v->y = (int16_t)(lv_integer(r, lv_at(r, value, 2)));
    child->mask |= DG_POSITION;
  } else if (ln == LN_LOCH || ln == LN_LOCV) {
    v->x = ln == LN_LOCH ? (int16_t)(lv_integer(r, value)) : layer.value.x;
    v->y = ln == LN_LOCV ? (int16_t)(lv_integer(r, value)) : layer.value.y;
    child->mask |= DG_POSITION;
  } else if (ln == LN_INK) { v->ink = (uint8_t)lv_integer(r, value); child->mask |= DG_INK;
  } else if (ln == LN_BLEND) {
    int b = lv_integer(r, value);
    v->blend = (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b); child->mask |= DG_BLEND;
  } else if (ln == LN_WIDTH || ln == LN_HEIGHT) {
    if (ln == LN_WIDTH) { v->width = (int16_t)lv_integer(r, value); child->mask |= DG_WIDTH; }
    else { v->height = (int16_t)lv_integer(r, value); child->mask |= DG_HEIGHT; }
    v->stretch = 1; child->mask |= DG_CHILD_STRETCH;
  } else if (ln == LN_VISIBLE) { child->visible = lv_truth(r, value); child->mask |= DG_CHILD_VISIBLE;
  } else if (ln == LN_STRETCH) { v->stretch = lv_truth(r, value); child->mask |= DG_CHILD_STRETCH;
  } else if (ln == LN_TYPE) { v->type = (uint8_t)lv_integer(r, value); child->mask |= DG_TYPE;
  } else if (ln == LN_ROTATION) { v->rotation = (int32_t)(lv_number(r, value) * 100); child->mask |= DG_ROTATION;
  } else if (ln == LN_SKEW) { v->skew = (int32_t)(lv_number(r, value) * 100); child->mask |= DG_SKEW;
  } else if (ln == LN_COLOR || ln == LN_BGCOLOR) {
    unsigned color = (unsigned)lv_integer(r, value), flag = ln == LN_COLOR ? 16 : 32;
    bool rgb = lv_type(value) == LV_COLOR || color > 255;
    v->flags = (v->flags & ~flag) | (rgb ? flag : 0);
    if (ln == LN_COLOR) { v->fore = (uint8_t)color; v->fore_rgb = color & 0xffffff; child->mask |= DG_FORE; }
    else { v->back = (uint8_t)color; v->back_rgb = color & 0xffffff; child->mask |= DG_BACK; }
  } else if (ln == LN_RECT) {
    if (lv_type(value) != LV_LIST || lv_count(r, value) != 4) {
      error(d, "film child rect", "requires four coordinates"); return;
    }
    int l = lv_integer(r, lv_at(r, value, 1)), t = lv_integer(r, lv_at(r, value, 2));
    v->width = (int16_t)(lv_integer(r, lv_at(r, value, 3)) - l);
    v->height = (int16_t)(lv_integer(r, lv_at(r, value, 4)) - t);
    const dg_member_t *m = dg_member(d, layer.value.member);
    v->x = (int16_t)(l + (m && m->width ? m->reg_x * v->width / m->width : v->width / 2));
    v->y = (int16_t)(t + (m && m->height ? m->reg_y * v->height / m->height : v->height / 2));
    v->stretch = 1; child->mask |= DG_SIZE | DG_POSITION | DG_CHILD_STRETCH;
  } else { error(d, "film child property write", "unsupported property"); return; }
  film_node_t *node=film_node(d,film_parent_id(d,parent));
  // Before first presentation, any edit also materializes the tell context.
  // Afterward only membership edits (including puppet release) can create
  // or retire playheads; geometry/appearance are read from the override.
  if(node && (!node->presented || ln==LN_MEMBER || ln==LN_MEMBERNUM ||
              ln==LN_CASTNUM || ln==LN_PUPPET))
    film_sync_node(d,film_node_id(d,node),false,0);
  dg_film_layer_t after;
  dg_film_child_layer(d,parent,channel,&after);
  if(!film_visual_equal(d,&layer,&after)||layer.placement_generation!=after.placement_generation) {
    film_visual_touch(d,node);
    film_visual_changed(d,node ? node->root : parent);
  }
}
static void film_drop(dg_runtime_t *d,unsigned id) {
  film_node_t *node=film_node(d,id);if(!node)return;
  for(unsigned i=0;i<FILM_NODE_LIMIT;i++)
    if(d->film_playback->nodes[i].member && d->film_playback->nodes[i].parent==id)film_drop(d,i+1);
  for(unsigned i=0;i<d->film_child_count;)
    if(d->film_children[i].parent==(FILM_NODE_PARENT|id))
      d->film_children[i]=d->film_children[--d->film_child_count];
    else i++;
  d->film_playback->cache_bytes-=node->cursor_count*sizeof(*node->cursors);
  d->film_playback->cache_bytes-=node->visual_capacity*sizeof(*node->visuals);
  free(node->visuals);free(node->cursors);*node=(film_node_t){0};
}
void dg_films_dispose(dg_runtime_t *d) {
  if(d->film_playback) {
    for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
      free(d->film_playback->nodes[i].cursors);
      free(d->film_playback->nodes[i].visuals);
    }
    free(d->film_playback);d->film_playback=NULL;
  }
  for(unsigned i=1;i<DG_SPRITES;i++)d->sprites[i].film_instance=0;
}
void dg_film_reset(dg_runtime_t *d,unsigned channel) {
  if(!channel||channel>=DG_SPRITES)return;
  if(d->film_playback)for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
    film_node_t *node=&d->film_playback->nodes[i];
    if(node->member&&!node->parent&&node->channel==channel)film_drop(d,i+1);
  }
  d->sprites[channel].film_instance=0;
}
static film_node_t *film_allocate(dg_runtime_t *d,uint32_t member,unsigned parent,unsigned channel,unsigned root,unsigned pose) {
  if(!d->film_playback)d->film_playback=calloc(1,sizeof(*d->film_playback));
  if(!d->film_playback){error(d,"film instances","allocation failed");return NULL;}
  for(unsigned i=0;i<FILM_NODE_LIMIT;i++)if(!d->film_playback->nodes[i].member) {
    film_node_t *node=&d->film_playback->nodes[i];
    if(d->film_playback->generation==0x7fffffffu){error(d,"film generation","counter exhausted");return NULL;}
    *node=(film_node_t){.member=member,.generation=++d->film_playback->generation,.parent=(uint16_t)parent,.channel=(uint16_t)channel,
      .root=(uint16_t)root,.pose=(uint16_t)pose};return node;
  }
  error(d,"film instances","capacity exceeded");return NULL;
}
static film_node_t *film_root(dg_runtime_t *d,unsigned channel) {
  if(!channel||channel>=DG_SPRITES)return NULL;
  dg_sprite_t *sprite=&d->sprites[channel];
  const dg_member_t *member=dg_member(d,sprite->value.member);
  if(!member||member->type!=2||member->source_xtra||!member->film_count)return NULL;
  film_node_t *node=film_node(d,sprite->film_instance);
  if(node&&node->member!=member->id){dg_film_reset(d,channel);node=NULL;}
  if(!node){
    node=film_allocate(d,member->id,0,channel,channel,dg_film_pose(member,sprite));
    if(node)sprite->film_instance=(uint16_t)film_node_id(d,node);
  }
  return node;
}
static unsigned film_next_pose(const dg_member_t *member,unsigned pose) {
  return member->film_loop ? (pose+1)%member->film_count : pose+(pose+1<member->film_count);
}
static bool film_sync_node(dg_runtime_t *d,unsigned id,bool advance,unsigned depth) {
  film_node_t *node=film_node(d,id);if(!node||d->values->failed)return false;
  if(depth>=16){error(d,"film nesting","temporal recursion exceeds sixteen active levels");return false;}
  const dg_member_t *member=dg_member(d,node->member);
  if(!member)return false;
  unsigned parent=node->parent ? FILM_NODE_PARENT|id : node->channel;
  dg_film_channels_t channels=dg_film_channels(d,member,node->pose,parent);
  uint16_t existing[DG_SPRITES]={0};
  for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
    film_node_t *child=&d->film_playback->nodes[i];
    if(child->member&&child->parent==id) {
      existing[child->channel]=(uint16_t)(i+1);
      // A sparse expanded frame can omit a formerly occupied channel. It
      // must still be visited once so its old instance is retired.
      dg_film_channels_add(&channels,child->channel);
    }
  }
  for(unsigned channel;(channel=dg_film_channels_next(&channels))&&!d->values->failed;) {
    dg_film_layer_t layer;
    bool has=dg_film_child_layer(d,parent,channel,&layer);
    const dg_member_t *child=has?dg_member(d,layer.value.member):NULL;
    if(child&&(child->type!=2||child->source_xtra||!child->film_count))child=NULL;
    film_node_t *nested=film_node(d,existing[channel]);
    if(nested&&(!child||nested->member!=child->id)) {
      film_drop(d,film_node_id(d,nested));nested=NULL;
    }
    if(!child)continue;
    bool fresh=!nested;
    if(fresh) {
      unsigned pose=node->pose+1;
      if(pose>=child->film_count)pose=child->film_loop?pose%child->film_count:child->film_count-1u;
      nested=film_allocate(d,child->id,id,channel,node->root,pose);
      if(!nested)return false;
    } else if(advance&&!nested->paused) {
      unsigned pose=film_next_pose(child,nested->pose);
      nested->pose=(uint16_t)pose;
    }
    film_sync_node(d,film_node_id(d,nested),advance&&!fresh,depth+1);
  }
  return film_visual_refresh(d,node);
}
static void film_mark_presented(dg_runtime_t *d,unsigned id) {
  film_node_t *node=film_node(d,id);if(!node)return;
  node->presented=true;
  for(unsigned i=0;i<FILM_NODE_LIMIT;i++)
    if(d->film_playback->nodes[i].member&&d->film_playback->nodes[i].parent==id)
      film_mark_presented(d,i+1);
}
unsigned dg_film_prepare(dg_runtime_t *d,unsigned parent,const dg_sprite_t *sprite,const dg_member_t *member) {
  if(!member||member->source_xtra||member->type!=2)return 0;
  film_node_t *node=NULL;
  if(parent>0&&parent<DG_SPRITES&&sprite==&d->sprites[parent])node=film_root(d,parent);
  else node=film_node(d,sprite->film_instance);
  if(!node)return 0;
  if(!node->parent)node->pose=(uint16_t)dg_film_pose(member,sprite);
  film_sync_node(d,film_node_id(d,node),false,0);
  return node->parent?FILM_NODE_PARENT|film_node_id(d,node):node->channel;
}
bool dg_film_visual_state(dg_runtime_t *d,unsigned parent,const dg_sprite_t *sprite,
    const dg_member_t *member,uint32_t *instance,uint32_t *revision) {
  unsigned context=dg_film_prepare(d,parent,sprite,member);
  film_node_t *node=film_node(d,film_parent_id(d,context));
  if(!node||d->values->failed)return false;
  *instance=node->generation;*revision=node->visual_revision;return true;
}
static bool film_picture_changed(dg_runtime_t *d,const dg_sprite_t *sprite) {
  film_node_t *node=film_node(d,sprite->film_instance);
  if(!node)return false;
  const dg_member_t *member=dg_member(d,node->member);
  if(!node->parent&&node->pose!=dg_film_pose(member,sprite))
    dg_film_prepare(d,node->channel,sprite,member);
  return node->visual_revision!=node->staged_revision;
}
static void film_picture_committed(dg_runtime_t *d,const dg_sprite_t *sprite) {
  film_node_t *node=film_node(d,sprite->film_instance);
  if(node)node->staged_revision=node->visual_revision;
}
void dg_films_present(dg_runtime_t *d,bool advance_roots) {
  for(unsigned channel=1;channel<DG_SPRITES&&!d->values->failed;channel++) {
    if(!d->sprites[channel].value.type)continue;
    film_node_t *node=film_root(d,channel);if(!node)continue;
    dg_sprite_t *sprite=&d->sprites[channel];
    const dg_member_t *member=dg_member(d,node->member);
    bool existed=node->presented;
    unsigned previous_pose=node->pose;
    if(advance_roots&&existed&&!sprite->film_paused)
      sprite->film_frame=film_next_pose(member,dg_film_pose(member,sprite));
    node->pose=(uint16_t)dg_film_pose(member,sprite);
    bool changed=film_sync_node(d,film_node_id(d,node),existed,0);
    film_mark_presented(d,film_node_id(d,node));
    if(!existed||changed||(!member->film_tracks&&!member->film_frames&&previous_pose!=node->pose))
      film_visual_changed(d,channel);
  }
}
static film_node_t *film_ref_node(dg_runtime_t *d,lv_t reference) {
  unsigned id=(uint32_t)lv_id(reference);
  if(!(id&DG_CHILD_SPRITE))return film_root(d,id);
  unsigned parent=film_child_parent(reference),channel=film_child_channel(reference);
  film_node_t *owner=film_node(d,film_parent_id(d,parent));
  if(!owner)return NULL;
  unsigned owner_id=film_node_id(d,owner);
  for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
    film_node_t *node=&d->film_playback->nodes[i];
    if(node->member&&node->parent==owner_id&&node->channel==channel)return node;
  }
  return NULL;
}
lv_t dg_film_tell(dg_runtime_t *d,lv_t reference,unsigned channel) {
  unsigned id=(uint32_t)lv_id(reference);
  film_node_t *node=film_ref_node(d,reference);
  if(!channel||channel>=DG_SPRITES){error(d,"film sprite tell channel","outside profile");return 0;}
  if(!node)return lv_make(LV_SPRITE,(int32_t)channel);
  if(id&DG_CHILD_SPRITE) {
    id=film_node_id(d,node);
    return lv_make(LV_SPRITE,(int32_t)(DG_CHILD_SPRITE|FILM_NODE_REF|(id<<DG_CHILD_SHIFT)|channel));
  }
  return lv_make(LV_SPRITE,(int32_t)(DG_CHILD_SPRITE|(id<<DG_CHILD_SHIFT)|channel));
}
unsigned dg_film_frame(dg_runtime_t *d,lv_t reference,bool last) {
  film_node_t *node=film_ref_node(d,reference);if(!node)return 0;
  const dg_member_t *member=dg_member(d,node->member);
  return last?member->film_count:node->parent?node->pose+1u:dg_film_pose(member,&d->sprites[node->channel])+1u;
}
bool dg_film_go(dg_runtime_t *d,lv_t reference,unsigned frame) {
  film_node_t *node=film_ref_node(d,reference);if(!node)return false;
  const dg_member_t *member=dg_member(d,node->member);
  if(!frame)return true;
  unsigned previous_pose=node->pose;
  bool existed=node->presented;
  node->pose=(uint16_t)(frame<=member->film_count?frame-1u:member->film_count-1u);
  if(!node->parent)d->sprites[node->channel].film_frame=node->pose;
  bool changed=film_sync_node(d,film_node_id(d,node),existed,0);
  film_mark_presented(d,film_node_id(d,node));
  if(!existed||changed||(!member->film_tracks&&!member->film_frames&&previous_pose!=node->pose))
    film_visual_changed(d,node->root);
  return true;
}

static uint32_t film_node_sound(dg_runtime_t *d,unsigned id,unsigned channel,unsigned *pose,unsigned *source,unsigned depth) {
  film_node_t *node=film_node(d,id);if(!node||depth>=16)return 0;
  const dg_member_t *member=dg_member(d,node->member);
  if(!member||!(member->film_flags&8u))return 0;
  uint32_t sound=member->film_sounds?member->film_sounds[node->pose*2+channel]:0;
  if(sound){*pose=node->pose;*source=node->generation;return sound;}
  unsigned highest=DG_SPRITES;
  while(highest) {
    film_node_t *selected=NULL;
    for(unsigned i=0;i<FILM_NODE_LIMIT;i++) {
      film_node_t *child=&d->film_playback->nodes[i];
      if(child->member&&child->parent==id&&child->channel<highest&&
          (!selected||child->channel>selected->channel))selected=child;
    }
    if(!selected)break;
    highest=selected->channel;
    sound=film_node_sound(d,film_node_id(d,selected),channel,pose,source,depth+1);
    if(sound)return sound;
  }
  return 0;
}
uint32_t dg_film_sound(dg_runtime_t *d,unsigned root,unsigned channel,unsigned *pose,unsigned *source) {
  *pose=*source=0;
  if(!root||root>=DG_SPRITES||channel>=2)return 0;
  const dg_member_t *member=dg_member(d,d->sprites[root].value.member);
  if(!member||!(member->film_flags&8u)||member->type!=2||!member->film_count)return 0;
  // Allocating the root establishes identity without presenting it or creating
  // descendants. The first sound selection and all later selections therefore
  // use the same generation, including before the first render.
  film_node_t *node=film_root(d,root);
  if(!node)return 0;
  if(node->presented)return film_node_sound(d,film_node_id(d,node),channel,pose,source,0);
  *pose=dg_film_pose(member,&d->sprites[root]);
  *source=node->generation;
  return member->film_sounds?member->film_sounds[*pose*2+channel]:0;
}
