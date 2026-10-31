// Original transition cast data, independently checked before publication.
static bool load_transitions(loader_t *l) {
  reader_t *r = l->reader;
  unsigned records = count(r, 12, 65535, "transition cast");
  for (unsigned i = 0; i < records && !r->failed; i++) {
    uint32_t id = u32(r);
    dg_member_t *member = NULL;
    for (unsigned movie = 0; movie < l->package->movie_count && !member; movie++) {
      const dg_movie_t *owner = l->package->registry[movie];
      if (owner->id != DG_ID_FILE(id)) continue;
      for (unsigned k = 0; k < owner->member_count; k++)
        if (owner->members[k].id == id) {
          member = (dg_member_t *)&owner->members[k];
          break;
        }
    }
    unsigned duration = u16(r), type = u8(r), chunk = u8(r), area = u8(r);
    unsigned reserved = u8(r) | u8(r) | u8(r);
    if (!member || member->type != 14 || member->transition_present ||
        type > 52 || area > 1 || reserved)
      return fail(r, "invalid, repeated or absent transition cast member");
    member->transition_ms = (uint16_t)duration;
    member->transition_type = (uint8_t)type;
    member->transition_chunk = (uint8_t)chunk;
    member->transition_area = (uint8_t)area;
    member->transition_present = true;
  }
  if (!r->failed && r->at != r->length)
    return fail(r, "trailing transition cast bytes");
  return !r->failed;
}
