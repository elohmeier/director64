// Mutable bitmap casts use native, bounded records, never serialized pointers
// or executable Director cast bytes. Text-only archives retain D6TF version1.
static unsigned cast_images_count(dg_runtime_t *d, const dg_cast_t *cast) {
  unsigned count = 0;
  for (dg_image_entry_t *p = imaging_store(d) ? imaging_store(d)->head : NULL;
       p; p = p->next)
    if (p->owner == d && p->member && p->changed &&
        DG_ID_FILE(p->member) == cast->file &&
        DG_ID_CAST(p->member) == cast->cast) {
      const dg_member_t *m = dg_member(d, p->member);
      if (m && m->type == 1)
        count++;
    }
  return count;
}
static void cast_images_erase(dg_runtime_t *d, uint32_t member) {
  dg_image_entry_t *p = imaging_member(d, member);
  if (p)
    p->member = 0;
}
static bool cast_images_write(dg_runtime_t *d, const dg_cast_t *cast,
                              uint8_t *data, unsigned capacity,
                              unsigned *offset) {
  unsigned count = cast_images_count(d, cast), at = *offset;
  if (!count)
    return true;
  if (count > 32 || at > capacity || capacity - at < 8)
    return false;
  memcpy(data + at, "D8IM\1\0", 6);
  d6_put16(data + at + 6, count);
  at += 8;
  for (dg_image_entry_t *p = imaging_store(d)->head; p; p = p->next) {
    if (p->owner != d || !p->member || !p->changed ||
        DG_ID_FILE(p->member) != cast->file ||
        DG_ID_CAST(p->member) != cast->cast)
      continue;
    const dg_member_t *m = dg_member(d, p->member);
    if (!m || m->type != 1)
      continue;
    const dg_pixel_image_t *im = &p->image;
    unsigned n = m ? (unsigned)strlen(m->name) : 0;
    uint64_t bytes = (uint64_t)im->width * im->height * 4u;
    if (!m || m->type != 1 || n > 63 || !im->width || !im->height ||
        im->width > 4096 || im->height > 4096 || bytes > 32u * 1024u * 1024u ||
        at > capacity || 16u + n + bytes > capacity - at)
      return false;
    d6_put16(data + at, m->number);
    d6_put16(data + at + 2, im->width);
    d6_put16(data + at + 4, im->height);
    d6_put16(data + at + 6, n);
    d6_put16(data + at + 8, (uint16_t)im->reg_x);
    d6_put16(data + at + 10, (uint16_t)im->reg_y);
    d6_put16(data + at + 12,
             (im->use_alpha ? 1u : 0u) | (im->grayscale ? 2u : 0u));
    d6_put16(data + at + 14, im->depth);
    at += 16;
    memcpy(data + at, m->name, n);
    at += n;
    for (uint64_t i = 0; i < bytes / 4; i++) {
      uint32_t pixel = im->pixels[i];
      for (unsigned c = 0; c < 4; c++)
        data[at++] = (uint8_t)(pixel >> (24 - 8 * c));
    }
  }
  *offset = at;
  return true;
}
static bool cast_images_restore(dg_runtime_t *d, const dg_cast_t *cast,
                                const uint8_t *data, unsigned length,
                                unsigned at, unsigned reserved, bool apply) {
  if (at > length || length - at < 8 || memcmp(data + at, "D8IM\1\0", 6))
    return false;
  unsigned count = d6_u16(data + at + 6);
  at += 8;
  if (count > 32)
    return false;
  unsigned numbers[32], name_at[32], names[32], new_members = 0;
  uint64_t bytes = 0;
  for (unsigned i = 0; i < count; i++) {
    if (at > length || length - at < 16)
      return false;
    unsigned number = d6_u16(data + at), width = d6_u16(data + at + 2),
             height = d6_u16(data + at + 4), n = d6_u16(data + at + 6);
    int reg_x = (int16_t)d6_u16(data + at + 8),
        reg_y = (int16_t)d6_u16(data + at + 10);
    unsigned flags = d6_u16(data + at + 12), depth = d6_u16(data + at + 14);
    at += 16;
    uint64_t pixels = (uint64_t)width * height, body = n + pixels * 4u;
    if (!number || !width || !height || width > 4096 || height > 4096 ||
        n > 63 || flags > 3 ||
        (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 &&
         depth != 32) ||
        pixels * 4 > 32u * 1024u * 1024u || body > length - at ||
        memchr(data + at, 0, n))
      return false;
    unsigned field_at = 8;
    for (unsigned f = 0; f < d6_u16(data + 6); f++) {
      unsigned field_number = d6_u16(data + field_at),
               field_name = d6_u16(data + field_at + 4),
               field_size = d6_u16(data + field_at + 6);
      if (field_number == number ||
          (n && field_name == n && !memcmp(data + field_at + 8, data + at, n)))
        return false;
      field_at += 8 + field_name + field_size;
    }
    for (unsigned j = 0; j < i; j++)
      if (numbers[j] == number ||
          (n && names[j] == n && !memcmp(data + name_at[j], data + at, n)))
        return false;
    numbers[i] = number;
    name_at[i] = at;
    names[i] = n;
    uint32_t id = DG_ID(cast->file, cast->cast, number);
    const dg_member_t *existing = dg_member(d, id);
    if (existing && existing->type != 0 && existing->type != 1)
      return false;
    bool dynamic = false;
    for (unsigned j = 0; j < d->dynamic_count; j++)
      dynamic |= d->dynamic_members[j].member.id == id;
    new_members += !dynamic;
    bytes += pixels * 4;
    if (bytes > 64u * 1024u * 1024u)
      return false;
    if (apply) {
      dg_member_t *m = NULL;
      unsigned slot = 32;
      for (unsigned j = 0; j < d->dynamic_count; j++)
        if (d->dynamic_members[j].member.id == id) {
          slot = j;
          break;
        }
      if (slot == 32) {
        for (unsigned j = 0; j < d->dynamic_count; j++)
          if (!d->dynamic_members[j].member.type) {
            slot = j;
            break;
          }
      }
      if (slot == 32) {
        if (d->dynamic_count == 32)
          return false;
        slot = d->dynamic_count++;
      }
      m = &d->dynamic_members[slot].member;
      *m = (dg_member_t){.id = id,
                         .number = number,
                         .cast = cast->cast,
                         .type = 1,
                         .name = d->dynamic_members[slot].name,
                         .text = "",
                         .asset = "",
                         .width = (uint16_t)width,
                         .height = (uint16_t)height,
                         .reg_x = (int16_t)reg_x,
                         .reg_y = (int16_t)reg_y};
      memcpy(d->dynamic_members[slot].name, data + at, n);
      d->dynamic_members[slot].name[n] = 0;
      dg_image_entry_t *old = imaging_member(d, id);
      if (old)
        old->member = 0;
      dg_image_entry_t *image = imaging_new(d, (int)width, (int)height, depth);
      if (!image)
        return false;
      unsigned pos = at + n;
      for (uint64_t p = 0; p < pixels; p++) {
        uint32_t pixel = 0;
        for (unsigned c = 0; c < 4; c++)
          pixel = pixel << 8 | data[pos++];
        image->image.pixels[p] = pixel;
      }
      image->member = id;
      image->image.reg_x = reg_x;
      image->image.reg_y = reg_y;
      image->image.use_alpha = flags & 1;
      image->image.grayscale = flags & 2;
      imaging_changed(d, image);
    }
    at += (unsigned)body;
  }
  unsigned free_slots = 32 - d->dynamic_count;
  for (unsigned i = 0; i < d->dynamic_count; i++)
    free_slots += !d->dynamic_members[i].member.type;
  return at == length && (apply || (new_members + reserved <= free_slots));
}
