// Christmas's probe state line, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, enter(), find() and the output helpers.
static void state(void) {
  printf("{\"movie\":"); quoted(director.movie ? director.movie->code->name : "");
  printf(",\"frame\":%u,\"tick\":%u,\"depth\":%u,\"quit\":%s,\"text_pending\":%s,\"sounds\":%u,\"save_generation\":%u,\"error\":", director.frame, director.ticks, values.depth, director.quit ? "true":"false", director.text_pending ? "true":"false", sound_count, game_save_generation());
  quoted(values.failed ? values.error : "");
  printf(",\"frame_script\":%u,\"event_sprite\":%u",director.score[0].script,director.current_event_sprite);
  printf(",\"script_errors\":%u,\"last_script_error\":", values.script_error_count);
  quoted(values.last_script_error); printf(",\"stack\":[");
  for (unsigned i=0;i<values.depth;i++) { if(i)putchar(','); printf("{\"handler\":");quoted(values.frames[i].handler->name);printf(",\"line\":%u,\"self\":",values.frames[i].line);{char self_text[64];quoted(lv_format(&values,values.frames[i].self,self_text,sizeof(self_text))?self_text:"?");}printf(",\"locals\":[");
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
  printf(",\"sprites\":["); comma=false;
  for(unsigned i=1;i<DG_SPRITES;i++) { dg_sprite_t *s=&director.sprites[i]; if(!s->value.type)continue;
    int l,t,r,b; dg_bounds(&director,i,&l,&t,&r,&b); if(comma)putchar(',');comma=true;
    printf("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],\"blend\":%u",i,s->value.member,s->value.script,l,t,r,b,dg_opacity(s));
    printf(",\"visible\":%s,\"behaviors\":[",s->visible?"true":"false");
    for(unsigned j=0;j<s->value.behavior_count;j++) {if(j)putchar(',');printf("%u",s->value.behaviors[j].script);}
    printf("]");
    const dg_member_t *m=dg_member(&director,s->value.member);
    if(m && (m->type==3 || m->type==7)) {
      printf(",\"editable\":%s,\"text\":",dg_text_editable(&director,i)?"true":"false");
      quoted(lv_cstr(&values,lv_get(&values,NULL,"text",lv_make(LV_MEMBER, (int32_t)m->id))));
    }
    putchar('}');
  }
  puts("]}");fflush(stdout);
}
