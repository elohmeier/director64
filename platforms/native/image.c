#include "image.h"
#include "ink.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool cursor_plane(lv_runtime_t *v, const dg_member_t *m, uint16_t out[256]) {
  const char *work = getenv("DIRECTOR64_WORK_DIR");
  char path[1024];
  if (!work || !m || !m->asset ||
      snprintf(path,sizeof(path),"%s/director/images/%s",work,m->asset)>=(int)sizeof(path)) {
    lv_fail(v,"native cursor bitmap missing");return false;
  }
  FILE *file=fopen(path,"rb");
  if (!file) {lv_fail(v,"native cursor image missing");return false;}
  uint8_t header[32],row[64];
  bool ok=fread(header,1,32,file)==32;
  long length=-1;
  if(ok && !fseek(file,0,SEEK_END)) length=ftell(file);
  bool rgba32=ok && !memcmp(header,"FDI2",4);
  bool planes=ok && !memcmp(header,"FDIA",4);
  unsigned stride=rgba32?4:2;
  uint64_t expected=32u+bitmap_plane_pixels(m->width,m->height)*stride;
  uint32_t alpha_offset;
  // A cursor is at most 16x16, so it is never stored tile-packed and the row
  // reads below stay contiguous. Refuse rather than misread if that changes.
  ok=ok && m->width && m->height && !bitmap_needs_tiles(m->width,m->height) &&
      length>=32 && (uint64_t)length<=UINT32_MAX;
  if(planes) ok=ok && dg_fdia_valid(header,(uint32_t)length,m->width,m->height,&alpha_offset);
  else ok=ok && (rgba32 || !memcmp(header,"FDI1",4)) && expected==(uint64_t)length &&
      dg_fdi_be32(header+4)==expected && dg_fdi_be16(header+8)==m->width &&
      dg_fdi_be16(header+10)==m->height;
  memset(out,0,256*sizeof(*out));
  unsigned width=m->width<16?m->width:16;
  for(unsigned y=0;ok && y<16 && y<m->height;y++) {
    uint64_t offset=32u+(uint64_t)y*m->width*stride;
    ok=offset<=LONG_MAX && !fseek(file,(long)offset,SEEK_SET) &&
        fread(row,stride,width,file)==width;
    for(unsigned x=0;ok && x<width;x++) {
      if(rgba32) {
        unsigned rgb=dg_fdi_be32(row+x*4)>>8;
        ok=!rgb || rgb==0xffffff;
        out[y*16+x]=rgb?0xffff:1;
      } else out[y*16+x]=dg_fdi_be16(row+x*2);
    }
  }
  fclose(file);
  if(!ok) lv_fail(v,"invalid native cursor image");
  return ok;
}
bool native_cursor_bitmap(lv_runtime_t *v, const dg_member_t *m,
                          const dg_member_t *mask, dg_cursor_bitmap_t *out) {
  uint16_t pixels[256],coverage[256];
  if(!cursor_plane(v,m,pixels) || (mask && !cursor_plane(v,mask,coverage))) return false;
  bool ok=dg_cursor_compose(out,pixels,m->width,m->height,mask?coverage:NULL,
      mask?mask->width:0,mask?mask->height:0,m->reg_x,m->reg_y);
  if(!ok) lv_fail(v,"unsupported color cursor bitmap");
  return ok;
}
bool native_image_hit(lv_runtime_t *values, const dg_member_t *m, unsigned ink, int x, int y) {
  if (ink != 8) return true;
  // Coordinates are checked before any multiplication, including hostile or
  // overflowed input coordinates. The Director caller supplies rectangle hits
  // for other inks, so those do not need to load the image at all.
  if (x < 0 || y < 0 || x >= m->width || y >= m->height) return false;
  unsigned width = m->width, height = m->height;
#if DIRECTOR64_DIRECTOR_VERSION >= 10
  // Converted assets are prescaled to the 640x480 stage; map the authored
  // pixel onto the stored texture.
  dg_prescale_dims(m->width, m->height, &width, &height);
  x = (int)((unsigned)x * width / m->width);
  y = (int)((unsigned)y * height / m->height);
  if ((unsigned)x >= width) x = (int)width - 1;
  if ((unsigned)y >= height) y = (int)height - 1;
#endif
  const char *work = getenv("DIRECTOR64_WORK_DIR");
  char path[1024];
  if (!work || snprintf(path, sizeof(path), "%s/director/images/%s", work, m->asset) >= (int)sizeof(path)) {
    lv_fail(values, "native image workspace missing or too long");
    return false;
  }
  FILE *file = fopen(path, "rb");
  if (!file) { lv_fail(values, "probe missing hit image"); return false; }
  unsigned char header[32], pixel[4];
  bool ok = fread(header, 1, sizeof(header), file) == sizeof(header);
  long length = -1;
  if (ok && !fseek(file, 0, SEEK_END)) length = ftell(file);
  bool rgba32 = ok && !memcmp(header, "FDI2", 4);
  bool planes = ok && !memcmp(header, "FDIA", 4);
  unsigned stride = rgba32 ? 4u : 2u;
  uint32_t alpha_offset = 0;
  uint64_t expected = 32u + bitmap_plane_pixels(width, height) * stride;
  ok = ok && length >= 32 && (uint64_t)length <= UINT32_MAX;
  if (planes) {
    ok = ok && dg_fdia_valid(header, (uint32_t)length, width, height,
                             &alpha_offset);
  } else {
    ok = ok && (rgba32 || !memcmp(header, "FDI1", 4)) &&
         expected <= UINT32_MAX && (uint64_t)length == expected &&
         dg_fdi_be32(header + 4) == expected &&
         dg_fdi_be16(header + 8) == width &&
         dg_fdi_be16(header + 10) == height;
  }
  // The stored pixel, which for a wide image is its tile-packed slot: the
  // console reads the same byte through bitmap_tile_index, and a hit test
  // that disagreed with the renderer would answer for the wrong pixel.
  uint64_t index = bitmap_needs_tiles(width, height)
                       ? bitmap_tile_index(width, (unsigned)x, (unsigned)y)
                       : (uint64_t)(unsigned)y * width + (unsigned)x;
  uint64_t offset = planes ? alpha_offset + index : 32u + stride * index;
  if (planes) stride = 1;
  ok = ok && offset <= LONG_MAX && !fseek(file, (long)offset, SEEK_SET) &&
       fread(pixel, 1, stride, file) == stride;
  fclose(file);
  if (!ok) {
    lv_fail(values, "invalid native hit image");
    return false;
  }
  return planes ? pixel[0] != 0 : rgba32 ? pixel[3] != 0 : (pixel[1] & 1) != 0;
}
