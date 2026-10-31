// Optional source vector polygons; MOVI remains compatible with packages that
// only carry static SWF artwork. All numbers are independently range checked.
static bool load_vectors(loader_t *l) {
  reader_t *r = l->reader;
  unsigned records = count(r, 24, 65535, "vector shape");
  for (unsigned i = 0; i < records && !r->failed; i++) {
    uint32_t id = u32(r);
    dg_member_t *member = NULL;
    unsigned file=DG_ID_FILE(id);
    if(file && file<=l->package->movie_count) {
      const dg_movie_t *m=l->package->registry[file-1];
      unsigned first=0,end=m->member_count;
      while(first<end) {
        unsigned middle=first+(end-first)/2;
        uint32_t key=m->members[middle].id;
        if(key==id) {member=(dg_member_t *)&m->members[middle];break;}
        if(key<id)first=middle+1;else end=middle;
      }
    }
    if (!member || member->source_xtra != 2 || member->vector_shape)
      return fail(r, "vector shape member is missing, repeated or not a vector");
    dg_vector_shape_t *v = allocate(l, 1, sizeof(*v));
    if (!v) return false;
    unsigned vertices = u16(r), flags = u8(r), reserved = u8(r);
    v->stroke_width = f64(r); v->stroke = u32(r); v->fill = u32(r);
    if (vertices < 2 || vertices > 1024 || flags > 3 || reserved ||
        !isfinite(v->stroke_width) || v->stroke_width < 0 || v->stroke_width > 255 ||
        v->stroke > 0xffffff || v->fill > 0xffffff || !need(r, (size_t)vertices * 16))
      return fail(r, "invalid vector shape properties");
    double *points = allocate(l, (size_t)vertices * 2, sizeof(*points));
    if (!points) return false;
    for (unsigned k = 0; k < vertices * 2 && !r->failed; k++) {
      points[k] = f64(r);
      if (!isfinite(points[k]) || fabs(points[k]) > 65535)
        return fail(r, "invalid vector shape coordinate");
    }
    v->vertex_count = vertices; v->vertices = points;
    v->filled = (flags & 1) != 0; v->closed = (flags & 2) != 0;
    member->vector_shape = v;
  }
  if (!r->failed && r->at != r->length) return fail(r, "trailing vector shape bytes");
  return !r->failed;
}
