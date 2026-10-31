// Live straight vector polygons use retained source style, not recolored or
// stretched raster artwork. Bezier/gradient descriptors remain explicit errors
// when an authored script attempts to edit them.
static bool imaging_vector_get(dg_runtime_t *d, uint32_t member, lv_t *out) {
  const dg_member_t *m = dg_member(d, member);
  if (!m || m->source_xtra != 2) return false;
  dg_image_entry_t *live = imaging_member(d, member);
  const dg_vector_shape_t *v = m->vector_shape;
  const double *points = live && live->vertices ? live->vertices : v ? v->vertices : NULL;
  unsigned count = live && live->vertices ? live->vertex_count : v ? v->vertex_count : 0;
  if (!points) { lv_fail(d->values, "vector vertexList requires retained straight-polygon metadata"); return true; }
  lv_t vertices[1024];
  for (unsigned i = 0; i < count; i++) {
    lv_t pair[] = {lv_symbol(d->values, "vertex"), lv_point(d->values, points[i*2], points[i*2+1])};
    vertices[i] = lv_list(d->values, 2, pair, true);
  }
  *out = lv_list(d->values, count, vertices, false);
  return true;
}
static bool imaging_polygon_inside(const double *points, unsigned count, double x, double y) {
  bool inside = false;
  for (unsigned i = 0, j = count - 1; i < count; j = i++) {
    double xi = points[i*2], yi = points[i*2+1], xj = points[j*2], yj = points[j*2+1];
    if ((yi > y) != (yj > y) && x < xi + (y-yi) * (xj-xi) / (yj-yi)) inside = !inside;
  }
  return inside;
}
static bool imaging_polygon_stroke(const double *points, unsigned count, bool closed,
                                    double radius, double x, double y) {
  if (radius <= 0) return false;
  for (unsigned i = 0; i < count - (closed ? 0 : 1); i++) {
    unsigned j = (i + 1) % count;
    double ax = points[i*2], ay = points[i*2+1], dx = points[j*2]-ax, dy = points[j*2+1]-ay;
    double length = dx*dx + dy*dy;
    double along = length ? ((x-ax)*dx + (y-ay)*dy) / length : 0;
    if (along < 0) along = 0;
    if (along > 1) along = 1;
    double ex = x-ax-along*dx, ey = y-ay-along*dy;
    if (ex*ex + ey*ey <= radius*radius) return true;
  }
  return false;
}
static bool imaging_vector_render(dg_runtime_t *d,uint32_t member,double *points,
 unsigned count,uint32_t stroke_rgb,uint32_t fill_rgb) {
  const dg_vector_shape_t *v=dg_member(d,member)->vector_shape;
  double l=points[0],r=l,t=points[1],b=t;
  for(unsigned i=1;i<count;i++){l=fmin(l,points[i*2]);r=fmax(r,points[i*2]);t=fmin(t,points[i*2+1]);b=fmax(b,points[i*2+1]);}
  // Original8.5.1r104 reserves a full stroke width of exterior padding,
  // although the centered stroke itself extends by half that width.
  double pad = v->stroke_width;
  int left = (int)floor(l-pad), top = (int)floor(t-pad);
  int right = (int)ceil(r+pad), bottom = (int)ceil(b+pad);
  if ((uint64_t)(right-left) * (unsigned)(bottom-top) * 4 +
      imaging_bytes(d) + (size_t)count * 2 * sizeof(*points) > 64u * 1024u * 1024u) {
    free(points); lv_fail(d->values, "vector image memory budget exhausted"); return true;
  }
  dg_image_entry_t *p = imaging_new(d, right-left, bottom-top, 32);
  if (!p) { free(points); return true; }
  p->image.use_alpha = true;
  p->image.reg_x = (right-left)/2; p->image.reg_y = (bottom-top)/2;
  p->vertices = points; p->vertex_count = count;
  p->vector_stroke=stroke_rgb;p->vector_fill=fill_rgb;p->vector_colored=true;
  uint32_t fill = imaging_rgb(d, lv_make(LV_COLOR, fill_rgb));
  uint32_t stroke = imaging_rgb(d, lv_make(LV_COLOR, stroke_rgb));
  // Four subpixels in each dimension preserve edge coverage for extractAlpha
  // and the subsequent copyPixels/setAlpha chain used by imaging puzzles.
  for (unsigned y = 0; y < p->image.height; y++) for (unsigned x = 0; x < p->image.width; x++) {
    unsigned sums[4] = {0};
    for (unsigned sy = 0; sy < 4; sy++) for (unsigned sx = 0; sx < 4; sx++) {
      double px = (double)left+x+(sx+.5)/4, py = (double)top+y+(sy+.5)/4;
      bool edge = imaging_polygon_stroke(points,count,v->closed,v->stroke_width/2,px,py);
      bool inside = v->filled && imaging_polygon_inside(points,count,px,py);
      if (!edge && !inside) continue;
      uint32_t pixel = edge ? stroke : fill;
      sums[3]++;
      for (unsigned k = 0; k < 3; k++) sums[k] += (pixel >> (k*8)) & 255u;
    }
    uint32_t pixel = 0xffffffu;
    if (sums[3]) {
      pixel = ((sums[3]*255+8)/16) << 24;
      for (unsigned k = 0; k < 3; k++) pixel |= ((sums[k]+sums[3]/2)/sums[3]) << (k*8);
    }
    p->image.pixels[(size_t)y*p->image.width+x] = pixel;
  }
  dg_image_entry_t *old = imaging_member(d, member);
  if (old) old->member = 0;
  p->member = member;
  imaging_changed(d,p);
  return true;
}
static bool imaging_vector_set(dg_runtime_t *d, uint32_t member, lv_t value) {
  const dg_member_t *m = dg_member(d, member);
  if (!m || m->source_xtra != 2) { lv_fail(d->values, "vertexList requires a vector shape member"); return true; }
  const dg_vector_shape_t *v = m->vector_shape;
  if (!v) { lv_fail(d->values, "vector vertexList edit requires retained straight-polygon metadata"); return true; }
  unsigned count = lv_count(d->values, value);
  if (lv_type(value) != LV_LIST || count < 2 || count > 1024) {
    lv_fail(d->values, "invalid vector vertexList"); return true;
  }
  double *points = malloc((size_t)count * 2 * sizeof(*points));
  if (!points) { lv_fail(d->values, "vector vertex allocation failed"); return true; }
  for (unsigned i = 0; i < count; i++) {
    lv_t vertex = lv_at(d->values, value, i+1);
    lv_t point = imaging_option(d, vertex, "vertex");
    if (lv_type(vertex) != LV_PROPLIST || lv_count(d->values, vertex) != 1 ||
        lv_type(point) != LV_LIST || lv_count(d->values, point) != 2) {
      free(points); lv_fail(d->values, "vector vertexList supports straight vertex points without curve handles"); return true;
    }
    double x = lv_number(d->values, lv_at(d->values, point, 1));
    double y = lv_number(d->values, lv_at(d->values, point, 2));
    if (!isfinite(x) || !isfinite(y) || fabs(x) > 65535 || fabs(y) > 65535) {
      free(points); lv_fail(d->values, "invalid vector vertex coordinate"); return true;
    }
    points[i*2] = x; points[i*2+1] = y;
  }
  dg_image_entry_t *old=imaging_member(d,member);
  return imaging_vector_render(d,member,points,count,
    old&&old->vector_colored?old->vector_stroke:v->stroke,
    old&&old->vector_colored?old->vector_fill:v->fill);
}

static bool imaging_vector_color_get(dg_runtime_t *d,uint32_t member,unsigned property,lv_t *out) {
  if(property!=LN_STROKECOLOR&&property!=LN_FILLCOLOR)return false;
  const dg_member_t *m=dg_member(d,member);if(!m||m->source_xtra!=2)return false;
  if(!m->vector_shape){lv_fail(d->values,"vector color query requires retained editable descriptor");return true;}
  dg_image_entry_t *old=imaging_member(d,member);
  uint32_t rgb=property==LN_STROKECOLOR?
    (old&&old->vector_colored?old->vector_stroke:m->vector_shape->stroke):
    (old&&old->vector_colored?old->vector_fill:m->vector_shape->fill);
  *out=lv_make(LV_COLOR,rgb);return true;
}
static bool imaging_vector_color_set(dg_runtime_t *d,uint32_t member,unsigned property,lv_t value) {
  lv_t previous;if(!imaging_vector_color_get(d,member,property,&previous))return false;
  if(d->values->failed)return true;
  uint32_t pixel=imaging_rgb(d,value);
  uint32_t rgb=((pixel&255)<<16)|(pixel&0xff00)|((pixel>>16)&255);
  if(rgb==(uint32_t)lv_integer(d->values,previous))return true;
  const dg_vector_shape_t *v=dg_member(d,member)->vector_shape;
  dg_image_entry_t *old=imaging_member(d,member);
  unsigned count=old&&old->vertices?old->vertex_count:v->vertex_count;
  const double *source=old&&old->vertices?old->vertices:v->vertices;
  double *points=malloc((size_t)count*2*sizeof(*points));
  if(!points){lv_fail(d->values,"vector color geometry allocation failed");return true;}
  memcpy(points,source,(size_t)count*2*sizeof(*points));
  uint32_t stroke=old&&old->vector_colored?old->vector_stroke:v->stroke;
  uint32_t fill=old&&old->vector_colored?old->vector_fill:v->fill;
  if(property==LN_STROKECOLOR)stroke=rgb;else fill=rgb;
  return imaging_vector_render(d,member,points,count,stroke,fill);
}
