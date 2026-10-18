// A native film may use an isolated plane only when every leaf has a
// destination-independent operation and binary coverage. Otherwise its
// children draw through the inverse transform stack against the real target.
#if DG_D10
static bool film_image_binary(cached_image_t *source,bool mask) {
  unsigned checked=mask?4u:1u,binary=checked<<1;
  if (source->coverage_flags&checked) return (source->coverage_flags&binary)!=0;
  bool safe=true;
  unsigned width=dg_fdi_be16((const uint8_t *)source->data+8);
  unsigned height=dg_fdi_be16((const uint8_t *)source->data+10);
  if (!width || !height) return false;
  if (mask || source->format==IMAGE_RGBA32 || source->alpha_offset) {
    for (unsigned y=0;y<height&&safe;y++) for (unsigned x=0;x<width;x++) {
      uint32_t pixel=image_pixel(source,width,height,x,y);
      unsigned coverage=mask?dg_mask_alpha(pixel,255,false):pixel>>24;
      if (coverage!=0&&coverage!=255) {safe=false;break;}
    }
  }
  source->coverage_flags|=(uint8_t)(checked|(safe?binary:0));
  return safe;
}
static bool film_leaf_safe(void *context,dg_runtime_t *d,const dg_sprite_t *sprite,
                            const dg_member_t *member,const dg_film_layer_t *layer) {
  (void)context;
  dg_member_t direct;
  if (layer->asset&&*layer->asset) {
    direct=(dg_member_t){.type=1,.asset=layer->asset,
        .width=layer->value.width,.height=layer->value.height};
    member=&direct;
  }
  if (!member) return !sprite->value.member&&sprite->value.type<16;
  if (member->source_xtra || dg_member_image(d,member->id)) return false;
  if (member->type==4||member->type==6||member->type==11||member->type==14) return true;
  if (member->type==8 && !dg_shape_texture(d,member,dg_member_pattern(d,member))) return true;
  if (member->type!=1&&member->type!=2&&member->type!=5&&member->type!=8&&member->type!=12)
    return false;
  dg_member_t plane=*member;
  unsigned ink=sprite->value.ink;
  if (member->type==2) {
    plane.asset=dg_film_asset(member,sprite);plane.type=1;
  } else if (member->type==8) {
    plane.asset=dg_shape_texture(d,member,dg_member_pattern(d,member));ink=0;
  }
  if (ink==9) ink=8;
  else if (ink==36&&member->type!=12) ink=0;
  cached_image_t *source=image(&plane,ink);
  if (!source || !film_image_binary(source,false)) return false;
  if (sprite->value.ink==9 && member->type==1) {
    const dg_member_t *mask=dg_member(d,member->id+1);
    if (mask && mask->type==1 && mask_pair(member,mask)) {
      if (dg_member_image(d,mask->id)) return false;
      cached_image_t *coverage=image(mask,8);
      if (!coverage || !film_image_binary(coverage,true)) return false;
    }
  }
  return true;
}
#endif
