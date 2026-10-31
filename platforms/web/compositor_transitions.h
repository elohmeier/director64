// Independent software transition renderer. Effect numbers/geometry are
// Director format facts; no player implementation is incorporated here.
// Dissolves use a maximal-length Galois register (Xilinx XAPP052, table3).
// The original-projector probes verify timing; its precise dissolve ordering
// and indexed-colour bit planes remain separate visual-comparison work.
static unsigned transition_extent(unsigned total, unsigned step, unsigned steps) {
  return (unsigned)((uint64_t)total * step / steps);
}
static unsigned transition_bits(unsigned n) {
  unsigned bits = 1;
  while (n >>= 1) bits++;
  return bits;
}
static uint32_t transition_polynomial(unsigned bits) {
  // Exponents of primitive polynomials, from the independent Xilinx table.
  static const uint8_t exponents[][4] = {
    {0}, {1}, {2,1}, {3,2}, {4,3}, {5,3}, {6,5}, {7,6},
    {8,6,5,4}, {9,5}, {10,7}, {11,9}, {12,6,4,1}, {13,4,3,1},
    {14,5,3,1}, {15,14}, {16,15,13,4}, {17,14}, {18,11},
    {19,6,2,1}, {20,17}, {21,19}, {22,21}, {23,18}, {24,23,22,17}
  };
  uint32_t polynomial = 0;
  for (unsigned i=0;i<4 && exponents[bits][i];i++)
    polynomial |= 1u << (exponents[bits][i]-1);
  return polynomial;
}
static bool transition_random(unsigned type) {
  return (type>=23 && type<=28 && type!=26) || type>=50;
}
static void transition_grid(wc_t *c, const dg_runtime_t *d) {
  unsigned width=(unsigned)(c->transition_r-c->transition_l);
  unsigned height=(unsigned)(c->transition_b-c->transition_t);
  unsigned chunk=d->transition_chunk;
  c->transition_cell_w=c->transition_cell_h=1;
  c->transition_bit_group=8;
  if (d->transition_type==24 || d->transition_type==25) {
    c->transition_cell_w=c->transition_cell_h=chunk;
    if(d->transition_type==24) {
      if(width>height)c->transition_cell_w=width*chunk/height;
      else c->transition_cell_h=height*chunk/width;
    }
  } else if(d->transition_type==27) {
    c->transition_cell_w=width;c->transition_cell_h=chunk;
  } else if(d->transition_type==28) {
    c->transition_cell_w=chunk;c->transition_cell_h=height;
  } else if(d->transition_type==50 || d->transition_type==52) {
    if(chunk>=8)c->transition_cell_w=chunk>=32?4:chunk>=16?2:1;
    else c->transition_bit_group=chunk>=4?4:chunk>=2?2:1;
  }
  c->transition_grid_w=(width+c->transition_cell_w-1)/c->transition_cell_w;
  c->transition_grid_h=(height+c->transition_cell_h-1)/c->transition_cell_h;
  c->transition_grid_w*=8/c->transition_bit_group;
  unsigned bits=transition_bits(c->transition_grid_w)+transition_bits(c->transition_grid_h);
  c->transition_polynomial=transition_polynomial(bits);
  c->transition_random_state=c->transition_polynomial;
  c->transition_random_done=0;
  c->transition_random_total=(1u<<bits)-1;
}
static void transition_dissolve(wc_t *c, unsigned step, unsigned steps) {
  uint32_t limit=(uint32_t)(((uint64_t)c->transition_random_total*step+steps-1)/steps);
  unsigned ybits=transition_bits(c->transition_grid_h);
  unsigned ymask=(1u<<ybits)-1;
  while(c->transition_random_done<limit) {
    uint32_t state=c->transition_random_state;
    unsigned gx=(state-1)>>ybits,gy=(state-1)&ymask;
    if(gx<c->transition_grid_w && gy<c->transition_grid_h) {
      unsigned groups=8/c->transition_bit_group;
      unsigned bit=(gx%groups)*c->transition_bit_group;
      unsigned mask=((1u<<c->transition_bit_group)-1)<<bit;
      uint32_t color_mask=mask | mask<<8 | mask<<16;
      int l=c->transition_l+(int)(gx/groups*c->transition_cell_w);
      int t=c->transition_t+(int)(gy*c->transition_cell_h);
      int r=l+(int)c->transition_cell_w,b=t+(int)c->transition_cell_h;
      if(r>c->transition_r)r=c->transition_r;
      if(b>c->transition_b)b=c->transition_b;
      for(int y=t;y<b;y++)for(int x=l;x<r;x++) {
        unsigned at=(unsigned)y*WC_WIDTH+(unsigned)x;
        c->transition_previous[at]=(c->transition_previous[at]&~color_mask)|
                                   (c->transition_next[at]&color_mask);
      }
    }
    c->transition_random_state=(state>>1)^((state&1)?c->transition_polynomial:0);
    c->transition_random_done++;
  }
}
static void transition_compose(wc_t *c,const dg_runtime_t *d) {
  unsigned age=d->ticks-d->transition_start,duration=d->transition_duration;
  if(!duration || age>=duration) {memcpy(target(c),c->transition_next,sizeof(c->scene));return;}
  unsigned type=d->transition_type;
  int l=c->transition_l,t=c->transition_t,r=c->transition_r,b=c->transition_b;
  unsigned w=(unsigned)(r-l),h=(unsigned)(b-t);
  if(!w || !h) {memcpy(target(c),c->transition_next,sizeof(c->scene));return;}
  unsigned steps=duration;
  if(type<23 || (type>=29 && type<=49)) {
    unsigned chunk=d->transition_chunk;
    unsigned axis=(type==3 || type==4 || type==7 || type==8 || type==13 || type==14)?h:w;
    if(type==9 || type==10)axis=w<h?w:h;
    unsigned count=axis/chunk;
    if(type==37 || type==49) {
      unsigned stripe=((type==37?h:w)+11)/12;
      count=(stripe+chunk-1)/chunk;
    } else if(type==38) {
      unsigned side=((w>h?w:h)+15)/16;
      count=2*((side+chunk-1)/chunk)+2;
    } else if(type>=39 && type<=46) {
      unsigned axis=(type==39 || type==40 || type==45 || type==46)?h:w;
      count=2*((axis+chunk-1)/chunk);
    }
    if(!count)count=1;
    if(count<steps)steps=count;
  }
  unsigned step=transition_extent(steps,age,duration);
  if(transition_random(type)) {
    transition_dissolve(c,step,steps);
    memcpy(target(c),c->transition_previous,sizeof(c->scene));return;
  }
  memcpy(target(c),c->transition_next,sizeof(c->scene));
  unsigned dx=transition_extent(w,step,steps),dy=transition_extent(h,step,steps);
  int old_dx=0,old_dy=0,new_dx=0,new_dy=0;
  bool slide_old=type>=11 && type<=22,slide_new=(type>=11 && type<=14)||(type>=29 && type<=36);
  if(slide_old) {
    if(type==11 || type==20 || type==21 || type==22)old_dx=-(int)dx;
    if(type==12 || type==16 || type==17 || type==18)old_dx=(int)dx;
    if(type==13 || type==18 || type==19 || type==20)old_dy=(int)dy;
    if(type==14 || type==15 || type==16 || type==22)old_dy=-(int)dy;
  }
  if(slide_new) {
    if(type==11 || type==30 || type==32 || type==35)new_dx=(int)(w-dx);
    if(type==12 || type==31 || type==33 || type==36)new_dx=-(int)(w-dx);
    if(type==13 || type==29 || type==30 || type==31)new_dy=-(int)(h-dy);
    if(type==14 || type==34 || type==35 || type==36)new_dy=(int)(h-dy);
  }
  for(int y=t;y<b;y++)for(int x=l;x<r;x++) {
    unsigned xx=(unsigned)(x-l),yy=(unsigned)(y-t);
    bool reveal=false;
    if(type==1)reveal=xx<dx;
    else if(type==2)reveal=xx>=w-dx;
    else if(type==3)reveal=yy<dy;
    else if(type==4)reveal=yy>=h-dy;
    else if(type>=5 && type<=10) {
      bool center_x=xx>=(w-dx)/2 && xx<(w+dx)/2;
      bool center_y=yy>=(h-dy)/2 && yy<(h+dy)/2;
      reveal=(type==5 || type==6)?center_x:(type==7 || type==8)?center_y:center_x&&center_y;
      if(!(type&1))reveal=!reveal;
    } else if(type==26) {
      unsigned rank=0;
      for(unsigned bit=0;bit<3;bit++) {
        unsigned a=(xx>>bit)&1,bb=(yy>>bit)&1;
        rank=rank*4+2*(a^bb)+bb;
      }
      reveal=rank<transition_extent(64,step,steps);
    } else if(type==37 || type==49) {
      unsigned position=type==37?yy:xx,total=type==37?h:w;
      unsigned stripe=(total+11)/12;
      reveal=position%stripe<transition_extent(stripe,step,steps);

    } else if(type==38) {
      unsigned side=((w>h?w:h)+15)/16;
      unsigned parity=(xx/side+yy/side)&1;
      unsigned progress=transition_extent(side*2,step,steps);
      reveal=progress>parity*side && yy%side<progress-parity*side;
    } else if(type>=39 && type<=46) {
      bool vertical=type==39 || type==40 || type==45 || type==46;
      unsigned axis=vertical?h:w;
      unsigned across=vertical?w:h;
      unsigned stripe=(across+15)/16;
      unsigned index=(vertical?xx:yy)/stripe;
      bool reverse_order=type==39 || type==42 || type==44 || type==45;
      unsigned offset=(reverse_order?15-index:index)*((axis+15)/16);
      unsigned progress=transition_extent(axis*2,step,steps);
      unsigned position=vertical?yy:xx;
      if(type==39 || type==40 || type==43 || type==44)position=axis-1-position;
      reveal=progress>offset && position<progress-offset;
    }
    uint32_t value=c->transition_previous[(unsigned)y*WC_WIDTH+(unsigned)x];
    if(slide_old) {
      int sx=x-old_dx,sy=y-old_dy;
      if(sx>=l && sx<r && sy>=t && sy<b)value=c->transition_previous[(unsigned)sy*WC_WIDTH+(unsigned)sx];
      else value=c->transition_next[(unsigned)y*WC_WIDTH+(unsigned)x];
    }
    if(slide_new) {
      int sx=x-new_dx,sy=y-new_dy;
      if(sx>=l && sx<r && sy>=t && sy<b)value=c->transition_next[(unsigned)sy*WC_WIDTH+(unsigned)sx];
    } else if(!slide_old && reveal)value=c->transition_next[(unsigned)y*WC_WIDTH+(unsigned)x];
    target(c)[(unsigned)y*WC_WIDTH+(unsigned)x]=value;
  }
}
