// Willy's probe state line, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, enter(), find() and the output helpers.
static void state(void) {
  printf("{\"movie\":"); quoted(director.movie ? director.movie->code->name : "");
  printf(",\"notice\":");quoted(director.notice);
  printf(",\"frame\":%u,\"tick\":%u,\"depth\":%u,\"quit\":%s,\"text_pending\":%s,\"sounds\":%u,\"save_generation\":%u,\"error\":", director.frame, director.ticks, values.depth, director.quit ? "true":"false", director.text_pending ? "true":"false", sound_count, game_save_generation());
  quoted(values.failed ? values.error : "");
  printf(",\"script_errors\":%u,\"last_script_error\":", values.script_error_count);
  quoted(values.last_script_error); printf(",\"stack\":[");
  for (unsigned i=0;i<values.depth;i++) { if(i)putchar(','); printf("{\"handler\":");quoted(values.frames[i].handler->name);printf(",\"line\":%u,\"self\":%d,\"locals\":[",values.frames[i].line,lv_id(values.frames[i].self));
    if(values.failed)for(unsigned j=0;j<values.frames[i].handler->locals;j++) {
      char text[16384];if(j)putchar(',');lv_format(&values,values.frames[i].locals[j],text,sizeof(text));quoted(text);
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
  printf("},\"player\":{");
  int global = lv_global_id(&values, "gmulleglobals");
  lv_t player = global >= 0 && lv_type(values.globals[global]) == LV_INSTANCE ? lv_get(&values, NULL, "user", values.globals[global]) : (lv_t){0};
  if (lv_type(player) == LV_INSTANCE) {
    unsigned count = values.objects[lv_id(player)].count;
    for (unsigned i = 1; i < count; i += 2) {
      char text[16384];
      if (i > 1) putchar(',');
      quoted_key(lv_cstr(&values, lv_at(&values, player, i))); putchar(':');
      lv_format(&values, lv_at(&values, player, i+1), text, sizeof(text)); quoted(text);
    }
  }
  printf("},\"car_name\":");
  lv_t car = lv_type(player) == LV_INSTANCE ? lv_get(&values, NULL, "car", player) : (lv_t){0};
  lv_t car_name = lv_type(car) == LV_INSTANCE ? lv_get(&values, NULL, "name", car) : (lv_t){0};
  quoted(lv_type(car_name) == LV_STRING ? lv_cstr(&values, car_name) : "");
  printf(",\"car_parts\":");
  lv_t parts = lv_type(car)==LV_INSTANCE ? lv_get(&values,NULL,"parts",car) : (lv_t){0};
  char parts_text[4096];lv_format(&values,parts,parts_text,sizeof(parts_text));quoted(parts_text);
  printf(",\"driving\":{");
  global=lv_global_id(&values,"gdir");
  lv_t dir=global>=0?values.globals[global]:(lv_t){0};
  lv_t driving=lv_type(dir)==LV_INSTANCE?lv_get(&values,NULL,"cardriving",dir):(lv_t){0};
  if(lv_type(driving)==LV_INSTANCE) {
    unsigned count=values.objects[lv_id(driving)].count;
    for(unsigned i=1;i<count;i+=2) {
      char text[16384];if(i>1)putchar(',');
      quoted_key(lv_cstr(&values,lv_at(&values,driving,i)));putchar(':');
      lv_format(&values,lv_at(&values,driving,i+1),text,sizeof(text));quoted(text);
    }
  }
  putchar('}');
  printf(",\"sprites\":["); comma=false;
  for(unsigned i=1;i<DG_SPRITES;i++) { dg_sprite_t *s=&director.sprites[i]; if(!s->value.type||!s->visible)continue;
    int l,t,r,b; dg_bounds(&director,i,&l,&t,&r,&b); if(comma)putchar(',');comma=true;
    printf("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],\"blend\":%u",i,s->value.member,s->value.script,l,t,r,b,dg_opacity(s));
    const dg_member_t *m=dg_member(&director,s->value.member);
    if(m && (m->type==3 || m->type==7)) {
      printf(",\"editable\":%s,\"text\":",dg_text_editable(&director,i)?"true":"false");
      quoted(lv_cstr(&values,lv_get(&values,NULL,"text",lv_make(LV_MEMBER, (int32_t)m->id))));
    }
    putchar('}');
  }
  puts("]}");fflush(stdout);
}
