// Lernerfolg's probe state line for the web runtime's probe
// (platforms/web/web_runtime.c), which includes it after defining values, director, enter(), find() and the output helpers.
static void state(void) {
  printf("{\"movie\":"); quoted(director.movie ? director.movie->code->name : "");
  printf(",\"frame\":%u,\"tick\":%u,\"depth\":%u,\"quit\":%s,\"text_pending\":%s,\"sounds\":%u,\"save_generation\":%u", director.frame, director.ticks, values.depth, director.quit ? "true":"false", director.text_pending ? "true":"false", sound_count, game_save_generation());
  printf(",\"window\":"); quoted(director.window_movie ? director.window_movie->code->name : director.window_movie_name);
  printf(",\"window_visible\":%s,\"error\":", director.window_visible ? "true":"false");
  quoted(values.failed ? values.error : "");
  printf(",\"window_frame\":%u,\"window_tick\":%u,\"window_fields\":[",
      director.window_runtime ? director.window_runtime->frame : 0,
      director.window_runtime ? director.window_runtime->ticks : 0);
  int window_mouse_line=-1;unsigned window_mouse_hit=0;
  dg_runtime_t *window=dg_window_begin(&director);
  if(window) {
    for(unsigned i=0;i<window->field_count;i++) {
      if(i)putchar(',');
      printf("{\"member\":%u,\"text\":",window->field_members[i]);
      quoted(lv_cstr(&values,lv_get(&values,NULL,"text",lv_make(LV_MEMBER,(int32_t)window->field_members[i]))));
      putchar('}');
    }
    printf("],\"window_editable\":[");bool first=true;
    for(unsigned i=1;i<DG_SPRITES;i++) {
      if(!window->sprites[i].value.type || !window->sprites[i].visible || !dg_text_editable(window,i))continue;
      int l,t,r,b;dg_bounds(window,i,&l,&t,&r,&b);
      if(!first)putchar(',');first=false;
      printf("{\"id\":%u,\"bounds\":[%d,%d,%d,%d]}",DG_WINDOW_SPRITE|i,
          l+director.window_rect[0],t+director.window_rect[1],r+director.window_rect[0],b+director.window_rect[1]);
    }
    window_mouse_line=lv_integer(&values,lv_get(&values,NULL,"mouseline",(lv_t){0}));
    window_mouse_hit=dg_mouse_hit(window,window->mouse_x,window->mouse_y);
    dg_window_end(&director);
  }
  putchar(']');
  unsigned window_sprites=0;
  if(director.window_runtime)
    for(unsigned i=1;i<DG_SPRITES;i++)
      window_sprites+=director.window_runtime->sprites[i].value.type && director.window_runtime->sprites[i].visible;
  printf(",\"window_sprites\":%u",window_sprites);
  printf(",\"window_mouse_line\":%d,\"window_mouse_hit\":%u",window_mouse_line,window_mouse_hit);
  printf(",\"frame_script\":%u,\"event_sprite\":%u",director.score[0].script,director.current_event_sprite);
  printf(",\"script_errors\":%u,\"last_script_error\":", values.script_error_count);
  quoted(values.last_script_error); printf(",\"stack\":[");
  for (unsigned i=0;i<values.depth;i++) { if(i)putchar(','); printf("{\"handler\":");quoted(values.frames[i].handler->name);printf(",\"line\":%u,\"self\":%d,\"locals\":[",values.frames[i].line,lv_id(values.frames[i].self));
    for(unsigned j=0;j<values.frames[i].handler->locals;j++) {
      if(j)putchar(',');
      lv_t local=values.frames[i].locals[j];
      if(lv_type(local)==LV_NUMBER)printf("%.10g",lv_numeric(local));else printf("null");
    }
    printf("]}"); }
  unsigned handles = 0; for (unsigned i=0;i<LV_OBJECTS;i++) handles += values.objects[i].type != 0;
  printf("],\"heap_bytes\":%u,\"heap_high_water\":%u,\"object_handles\":%u,\"globals\":{", values.heap_used, values.heap_high_water, handles); bool comma=false;
  for(unsigned i=0;i<values.global_count;i++) if(lv_type(values.globals[i])) {
    char text[16384]; if(!lv_format(&values,values.globals[i],text,sizeof(text))) break;
    if(comma)putchar(',');
    comma=true; quoted(global_names[i]);putchar(':');quoted(text);
  }
  printf("},\"objects\":{"); comma=false;
  for(unsigned i=0;i<values.global_count;i++) if(lv_type(values.globals[i]) == LV_INSTANCE) {
    lv_t instance = values.globals[i]; unsigned count = values.objects[lv_id(instance)].count;
    if(comma)putchar(',');
    comma=true;quoted(global_names[i]);printf(":{");
    for(unsigned j=1;j<count;j+=2) {
      char text[16384];lv_t key=lv_at(&values,instance,j), value=lv_at(&values,instance,j+1);
      if(!lv_format(&values,value,text,sizeof(text)))break;
      if(j>1)putchar(',');
      quoted_key(lv_cstr(&values,key));putchar(':');quoted(text);
    }
    putchar('}');
  }
  printf("}");
  // The Flash overlay surface: journeys assert the exercise prompt text is
  // written into its named field and laid out before calling it visible.
  printf(",\"flash\":["); comma=false;
  for(unsigned i=0;i<director.flash_object_count;i++) {
    lv_t bag=values.roots[DG_FLASH_ROOT+i];
    if(lv_type(bag)!=LV_PROPLIST)continue;
    unsigned channel=director.flash_objects[i].sprite;
    const dg_member_t *m=dg_member(&director,director.sprites[channel].value.member);
    const dg_flash_field_t *field=dg_flash_field_find(m,director.flash_objects[i].name);
    if(comma){putchar(',');}
    comma=true;
    printf("{\"sprite\":%u,\"name\":",channel);
    quoted(director.flash_objects[i].name);
    lv_t visible=lv_get(&values,NULL,"_visible",bag);
    lv_t text=lv_get(&values,NULL,"text",bag);
    lv_t height=lv_get(&values,NULL,"textheight",bag);
    printf(",\"field\":%s,\"visible\":%s,\"text\":",field?"true":"false",
           lv_truth(&values,visible)?"true":"false");
    quoted(lv_type(text)==LV_STRING?lv_cstr(&values,text):"");
    printf(",\"textheight\":%d}",lv_type(height)==LV_NUMBER?(int)lv_numeric(height):0);
  }
  printf("]");
  printf(",\"sprites\":["); comma=false;
  for(unsigned i=1;i<DG_SPRITES;i++) { dg_sprite_t *s=&director.sprites[i]; if(!s->value.type)continue;
    int l,t,r,b; dg_bounds(&director,i,&l,&t,&r,&b); if(comma)putchar(',');comma=true;
    printf("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],\"blend\":%u",i,s->value.member,s->value.script,l,t,r,b,dg_opacity(s));
    printf(",\"visible\":%s,\"behaviors\":[",s->visible?"true":"false");
    for(unsigned j=0;j<s->value.behavior_count;j++) {if(j)putchar(',');printf("%u",s->value.behaviors[j].script);}
    printf("]");
    const dg_member_t *m=dg_member(&director,s->value.member);
    if (m && m->source_xtra == 1)
      printf(",\"flash_frame\":%d", lv_integer(&values,lv_get(&values,NULL,"frame",lv_make(LV_SPRITE,i))));
    if(m && (m->type==3 || m->type==7)) {
      printf(",\"editable\":%s,\"text\":",dg_text_editable(&director,i)?"true":"false");
      quoted(lv_cstr(&values,lv_get(&values,NULL,"text",lv_make(LV_MEMBER, (int32_t)m->id))));
    }
    putchar('}');
  }
  puts("]}");fflush(stdout);
}
