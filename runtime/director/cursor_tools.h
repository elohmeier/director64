#ifndef DIRECTOR64_CURSOR_TOOLS_H
#define DIRECTOR64_CURSOR_TOOLS_H

// Standard tool roles documented by Macromedia and present in the selected
// 8.5.1 engine's RT_GROUP_CURSOR directory. IDs and hotspots are format facts;
// the 16-pixel glyphs below are independently drawn target UI, not DLL artwork.
typedef struct { uint16_t id; uint8_t x, y; } dg_cursor_tool_t;
static inline const dg_cursor_tool_t *dg_cursor_tool(int id) {
  static const dg_cursor_tool_t tools[] = {
    {254,3,3}, {256,2,14}, {257,0,0}, {258,7,7}, {259,2,15},
    {260,4,6}, {261,8,8}, {262,8,8}, {263,8,8}, {264,8,8},
    {265,8,8}, {266,8,8}, {267,8,8}, {268,8,8}, {269,8,8},
    {271,8,8}, {272,2,15}, {280,4,0}, {281,0,15},
    {284,7,8}, {285,7,8}, {286,7,8}, {290,4,6}, {291,4,6},
    {292,4,6}, {293,0,0}, {294,7,7}, {295,8,8}, {296,7,7},
    {297,8,8}, {298,8,7}, {299,8,8}, {300,7,4}, {301,0,14},
    {302,5,6}, {303,5,6}, {304,5,6}
  };
  for (unsigned i = 0; i < sizeof(tools) / sizeof(*tools); i++)
    if (tools[i].id == id) return &tools[i];
  return NULL;
}
static inline bool dg_cursor_supported(int resource) {
  return (resource >= -1 && resource <= 4) || resource == 200 ||
         dg_cursor_tool(resource) != NULL;
}
static inline void dg_cursor_dot(uint16_t rows[16], int x, int y) {
  if (x >= 0 && x < 16 && y >= 0 && y < 16)
    rows[y] |= (uint16_t)(0x8000u >> x);
}
static inline void dg_cursor_line(uint16_t rows[16], int x, int y, int xx, int yy) {
  int dx = xx > x ? xx - x : x - xx, sx = x < xx ? 1 : -1;
  int dy = yy > y ? y - yy : yy - y, sy = y < yy ? 1 : -1;
  int error = dx + dy;
  for (;;) {
    dg_cursor_dot(rows, x, y);
    if (x == xx && y == yy) break;
    int next = error * 2;
    if (next >= dy) { error += dy; x += sx; }
    if (next <= dx) { error += dx; y += sy; }
  }
}
static inline void dg_cursor_box(uint16_t rows[16], int l, int t, int r, int b) {
  dg_cursor_line(rows,l,t,r,t); dg_cursor_line(rows,r,t,r,b);
  dg_cursor_line(rows,r,b,l,b); dg_cursor_line(rows,l,b,l,t);
}
static inline void dg_cursor_ring(uint16_t rows[16], int cx, int cy, int radius) {
  int x = radius, y = 0, error = 1 - radius;
  while (x >= y) {
    dg_cursor_dot(rows,cx+x,cy+y); dg_cursor_dot(rows,cx+y,cy+x);
    dg_cursor_dot(rows,cx-y,cy+x); dg_cursor_dot(rows,cx-x,cy+y);
    dg_cursor_dot(rows,cx-x,cy-y); dg_cursor_dot(rows,cx-y,cy-x);
    dg_cursor_dot(rows,cx+y,cy-x); dg_cursor_dot(rows,cx+x,cy-y);
    y++;
    if (error < 0) error += 2*y+1;
    else { x--; error += 2*(y-x)+1; }
  }
}
static inline void dg_cursor_arrowhead(uint16_t rows[16], int x, int y, int dx, int dy) {
  dg_cursor_line(rows,x,y,x-3*dx+3*dy,y-3*dy-3*dx);
  dg_cursor_line(rows,x,y,x-3*dx-3*dy,y-3*dy+3*dx);
}
static inline void dg_cursor_hand(uint16_t rows[16], int resource) {
  // White palms with a black outline and short finger creases.
  static const char *const open[] = {
    "     ##", "  ## #### ##", " ###########", " ###########",
    " ############", " ############", " ############",
    " ############", " ############", " ############",
    " ############", "  ##########", "   ########", "   ########", "", ""
  };
  static const char *const finger[] = {
    "    ##", "   ####", "   ####", "   ####", "   ####",
    "   ########", "   ##########", "   ###########", "   ###########",
    " #############", " #############", "  ###########",
    "   #########", "    #######", "    #######", ""
  };
  static const char *const closed[] = {
    "", "", "", "", "   ########", "  ##########",
    " ############", " ############", " ############", " ############",
    " ############", "  ##########", "   ########", "   ########", "", ""
  };
  const char *const *source = resource == 260 ? open : resource == 280 ? finger : closed;
  uint16_t body[16] = {0};
  for (unsigned y = 0; y < 16; y++)
    for (unsigned x = 0; source[y][x]; x++)
      if (source[y][x] == '#') dg_cursor_dot(body,(int)x,(int)y);
  for (unsigned y = 0; y < 16; y++) {
    uint16_t interior = (uint16_t)(body[y] << 1 & body[y] >> 1);
    interior &= y ? body[y-1] : 0;
    interior &= y < 15 ? body[y+1] : 0;
    rows[y] |= body[y] & (uint16_t)~interior;
  }
  if (resource == 260) {
    dg_cursor_line(rows,4,3,4,7); dg_cursor_line(rows,7,2,7,7);
    dg_cursor_line(rows,10,3,10,7);
  } else if (resource == 280) {
    dg_cursor_line(rows,6,6,6,10); dg_cursor_line(rows,9,7,9,10);
    dg_cursor_line(rows,12,8,12,10);
  } else dg_cursor_line(rows,3,7,10,7);
}
static inline bool dg_cursor_tool_draw(dg_cursor_bitmap_t *out, int resource) {
  const dg_cursor_tool_t *tool = dg_cursor_tool(resource);
  if (!tool) return false;
  uint16_t *rows = out->black;
  out->hot_x = tool->x; out->hot_y = tool->y;
  switch (resource) {
  case 254: // Help: a question mark beside a small pointer.
    dg_cursor_line(rows,1,1,1,8); dg_cursor_line(rows,1,1,6,6);
    dg_cursor_line(rows,1,8,3,6); dg_cursor_line(rows,3,6,6,6);
    dg_cursor_ring(rows,11,4,3); rows[5] &= 0x03ff; rows[6] &= 0x03ff;
    dg_cursor_line(rows,13,5,10,8); dg_cursor_dot(rows,10,10); break;
  case 256: // Pencil.
    dg_cursor_line(rows,2,14,11,2); dg_cursor_line(rows,4,14,13,2);
    dg_cursor_line(rows,11,2,13,2); dg_cursor_line(rows,2,14,4,14);
    dg_cursor_line(rows,9,5,11,7); break;
  case 257: // Eraser.
    dg_cursor_line(rows,1,8,8,1); dg_cursor_line(rows,8,1,14,6);
    dg_cursor_line(rows,14,6,7,13); dg_cursor_line(rows,7,13,1,8);
    dg_cursor_line(rows,4,5,10,10); break;
  case 258: // Selection marquee.
    for (int n=2;n<=12;n+=2) {
      dg_cursor_dot(rows,n,2);dg_cursor_dot(rows,n,12);
      dg_cursor_dot(rows,2,n);dg_cursor_dot(rows,12,n);
    } break;
  case 259: // Bucket and pour.
    dg_cursor_line(rows,3,5,8,1);dg_cursor_line(rows,8,1,13,6);
    dg_cursor_line(rows,13,6,8,11);dg_cursor_line(rows,8,11,3,5);
    dg_cursor_line(rows,3,5,13,6);dg_cursor_line(rows,2,9,2,14);
    dg_cursor_dot(rows,1,14);dg_cursor_dot(rows,3,14); break;
  case 260: case 280: case 290: case 291: case 292:
    dg_cursor_hand(rows,resource);
    if (resource == 291) { dg_cursor_ring(rows,11,11,3);dg_cursor_line(rows,9,9,13,13); }
    if (resource == 292) { dg_cursor_box(rows,9,9,15,15);dg_cursor_line(rows,11,12,13,12);dg_cursor_line(rows,12,11,12,13); }
    break;
  case 261: dg_cursor_box(rows,2,3,13,12); break;
  case 262:
    dg_cursor_line(rows,4,2,11,2);dg_cursor_line(rows,4,13,11,13);
    dg_cursor_line(rows,2,4,2,11);dg_cursor_line(rows,13,4,13,11);
    dg_cursor_dot(rows,3,3);dg_cursor_dot(rows,12,3);dg_cursor_dot(rows,3,12);dg_cursor_dot(rows,12,12); break;
  case 263: dg_cursor_ring(rows,7,7,6); break;
  case 264: dg_cursor_line(rows,2,13,13,2); break;
  case 265: // Rich text T, with a bold stem.
    dg_cursor_box(rows,2,2,13,4);dg_cursor_box(rows,6,4,9,13); break;
  case 266: // Editable text field.
    dg_cursor_box(rows,1,3,14,12);dg_cursor_line(rows,6,5,9,5);
    dg_cursor_line(rows,7,5,7,10);dg_cursor_line(rows,6,10,9,10); break;
  case 267: // Button with a lower/right bevel.
    dg_cursor_box(rows,1,4,13,10);dg_cursor_line(rows,3,12,15,12);
    dg_cursor_line(rows,15,6,15,12); break;
  case 268:
    dg_cursor_box(rows,2,2,13,13);dg_cursor_line(rows,4,7,7,10);dg_cursor_line(rows,7,10,11,5); break;
  case 269:
    dg_cursor_ring(rows,7,7,6);dg_cursor_ring(rows,7,7,2);dg_cursor_dot(rows,7,7); break;
  case 271:
    dg_cursor_ring(rows,8,8,3);dg_cursor_line(rows,1,8,14,8);dg_cursor_line(rows,8,1,8,14); break;
  case 272: // Lasso with its loose end.
    dg_cursor_ring(rows,8,5,4);dg_cursor_line(rows,5,8,2,14);dg_cursor_line(rows,2,14,5,14); break;
  case 281: // Dropper: bulb and narrow diagonal tube.
    dg_cursor_ring(rows,11,3,2);dg_cursor_line(rows,10,5,2,13);
    dg_cursor_line(rows,12,6,3,15);dg_cursor_line(rows,2,13,0,15);
    dg_cursor_line(rows,0,15,3,15);dg_cursor_line(rows,8,5,12,9); break;
  case 284: case 297:
    dg_cursor_line(rows,8,1,8,14);dg_cursor_arrowhead(rows,8,1,0,-1);dg_cursor_arrowhead(rows,8,14,0,1);
    if(resource==284)dg_cursor_line(rows,4,8,12,8);
    break;
  case 285: case 296:
    dg_cursor_line(rows,1,7,14,7);dg_cursor_arrowhead(rows,1,7,-1,0);dg_cursor_arrowhead(rows,14,7,1,0);
    if(resource==285)dg_cursor_line(rows,7,3,7,11);
    break;
  case 286: case 299:
    dg_cursor_line(rows,3,3,12,12);dg_cursor_line(rows,3,3,3,8);dg_cursor_line(rows,3,3,8,3);
    dg_cursor_line(rows,12,12,7,12);dg_cursor_line(rows,12,12,12,7);
    if(resource==286)dg_cursor_dot(rows,7,8);
    break;
  case 293: // Inverse pointer, filled interior supplied by the coverage pass.
    dg_cursor_line(rows,0,0,0,12);dg_cursor_line(rows,0,0,9,9);
    dg_cursor_line(rows,0,12,3,9);dg_cursor_line(rows,3,9,9,9);
    dg_cursor_line(rows,4,10,7,15);dg_cursor_line(rows,6,10,9,15); break;
  case 294:
    dg_cursor_ring(rows,7,7,5);rows[2]&=0x07ff;rows[3]&=0x07ff;
    dg_cursor_line(rows,8,1,12,2);dg_cursor_line(rows,12,2,11,6); break;
  case 295:
    dg_cursor_line(rows,5,3,13,3);dg_cursor_line(rows,13,3,10,12);
    dg_cursor_line(rows,10,12,2,12);dg_cursor_line(rows,2,12,5,3);
    dg_cursor_line(rows,1,1,8,1);dg_cursor_arrowhead(rows,8,1,1,0); break;
  case 298:
    dg_cursor_line(rows,3,12,12,3);dg_cursor_line(rows,3,12,3,7);dg_cursor_line(rows,3,12,8,12);
    dg_cursor_line(rows,12,3,7,3);dg_cursor_line(rows,12,3,12,8); break;
  case 300: // Smear/smooth brush.
    dg_cursor_box(rows,5,1,9,6);dg_cursor_line(rows,5,6,2,12);dg_cursor_line(rows,9,6,6,12);
    dg_cursor_line(rows,2,12,6,12);dg_cursor_line(rows,4,14,12,14); break;
  case 301: // Airbrush and spray dots.
    dg_cursor_line(rows,1,14,8,7);dg_cursor_line(rows,3,15,10,8);dg_cursor_line(rows,8,7,10,8);
    for(int y=1;y<=7;y+=2)for(int x=10;x<=14;x+=2)if((x+y)%4)dg_cursor_dot(rows,x,y);
    break;
  case 302: case 303: case 304:
    dg_cursor_ring(rows,5,6,4);dg_cursor_line(rows,8,9,13,14);dg_cursor_line(rows,9,9,14,14);
    if(resource==304){dg_cursor_line(rows,3,4,7,8);dg_cursor_line(rows,3,8,7,4);}
    else {dg_cursor_line(rows,3,6,7,6);if(resource==302)dg_cursor_line(rows,5,4,5,8);}
    break;
  default: return false;
  }
  // White contrast around black strokes. Fill hand palms and the inverse
  // pointer so they stay readable on a busy game background.
  for(unsigned y=0;y<16;y++) {
    uint16_t row=rows[y];
    if(y)row|=rows[y-1];
    if(y<15)row|=rows[y+1];
    out->opaque[y]=(uint16_t)(row|row<<1|row>>1);
    if(resource==260||resource==280||(resource>=290&&resource<=293)) {
      int first=0,last=15;
      while(first<16&&!(rows[y]&(0x8000u>>first)))first++;
      while(last>first&&!(rows[y]&(0x8000u>>last)))last--;
      for(int x=first;x<=last&&x<16;x++)out->opaque[y]|=(uint16_t)(0x8000u>>x);
    }
  }
  return true;
}
#endif
