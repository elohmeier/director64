// Mucklas's probe state line for the web runtime's probe
// (platforms/web/web_runtime.c), which includes it after defining values, director, pointer, enter(), find() and the output helpers.
#include "pointer_probe.h"
static lv_t match_global(const char *name) {
  int index=lv_global_id(&values,name);
  return index>=0 ? values.globals[index] : (lv_t){0};
}
static void match_counts(unsigned *humans,unsigned *computers) {
  *humans=*computers=0;
  if(!director.movie)return;
  const char *movie=director.movie->code->name;
  if(!strcmp(movie,"KR.DXR")) {
    lv_t refs=match_global("gobjektreferens");
    if(lv_type(refs)!=LV_PROPLIST)return;
    lv_t drivers=lv_get(&values,NULL,"kotte",refs);
    if(lv_type(drivers)==LV_LIST)
      for(unsigned i=1;i<=lv_count(&values,drivers)&&i<=4;i++)
        *humans+=lv_type(lv_at(&values,drivers,i))==LV_INSTANCE;
    *computers=lv_type(lv_get(&values,NULL,"dator",refs))==LV_INSTANCE;
  } else if(!strcmp(movie,"SS.DXR")) {
    lv_t players=match_global("gspelarlista");
    if(lv_type(players)!=LV_LIST)return;
    for(unsigned i=1;i<=lv_count(&values,players)&&i<=4;i++) {
      lv_t player=lv_at(&values,players,i);
      if(lv_type(player)!=LV_INSTANCE)continue;
      lv_t type=lv_get(&values,NULL,"typ",player);
      if(lv_type(type)!=LV_STRING&&lv_type(type)!=LV_SYMBOL)continue;
      const char *name=lv_cstr(&values,type);
      *humans+=!strcmp(name,"manniska");*computers+=!strcmp(name,"dator");
    }
  } else if(!strcmp(movie,"KB.DXR")) {
    const char *names[]={"gundermucklalist","gmellanmucklalist","govermucklalist"};
    for(unsigned i=0;i<3;i++) {
      lv_t player=match_global(names[i]);
      if(lv_type(player)!=LV_PROPLIST)continue;
      lv_t type=lv_get(&values,NULL,"typavspelare",player);
      if(lv_type(type)!=LV_STRING&&lv_type(type)!=LV_SYMBOL)continue;
      const char *name=lv_cstr(&values,type);
      *humans+=!strcmp(name,"musspelare")||!strcmp(name,"medspelare");
      *computers+=!strcmp(name,"dator");
    }
  }
}
static void state(void) {
  printf("{\"movie\":"); quoted(director.movie ? director.movie->code->name : "");
  printf(",\"frame\":%u,\"tick\":%u,\"depth\":%u,\"quit\":%s,\"sounds\":%u,\"save_generation\":%u,\"error\":", director.frame, director.ticks, values.depth, director.quit ? "true":"false", sound_count, game_save_generation());
  quoted(values.failed ? values.error : "");
  printf(",\"script_errors\":%u,\"last_script_error\":", values.script_error_count);
  quoted(values.last_script_error); printf(",\"stack\":[");
  for (unsigned i=0;i<values.depth;i++) { if(i)putchar(','); printf("{\"handler\":");quoted(values.frames[i].handler->name);printf(",\"line\":%u}",values.frames[i].line); }
  unsigned handles = 0; for (unsigned i=0;i<LV_OBJECTS;i++) handles += values.objects[i].type != 0;
  printf("],\"heap_bytes\":%u,\"heap_high_water\":%u,\"object_handles\":%u,\"globals\":{", values.heap_used, values.heap_high_water, handles); bool comma=false;
  for(unsigned i=0;i<values.global_count;i++) if(lv_type(values.globals[i])) {
    char text[16384]; if(!lv_format(&values,values.globals[i],text,sizeof(text))) break;
    if(comma)putchar(',');
    comma=true; quoted(global_names[i]);putchar(':');quoted(text);
  }
  printf("},\"name_text\":");
  const dg_member_t *name_member = director.movie && !strcmp(director.movie->code->name,"LO.DXR") ? dg_member(&director,director.sprites[39].value.member) : NULL;
  quoted(name_member ? lv_cstr(&values,lv_get(&values,NULL,"text",lv_make(LV_MEMBER, (int32_t)name_member->id))) : "");
  printf(",\"objects\":{"); comma=false;
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
  printf("},\"sprites\":["); comma=false;
  for(unsigned i=1;i<DG_SPRITES;i++) { dg_sprite_t *s=&director.sprites[i]; if(!s->value.type||!s->visible)continue;
    int l,t,r,b; dg_bounds(&director,i,&l,&t,&r,&b); if(comma)putchar(',');comma=true;
    printf("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],\"blend\":%u}",i,s->value.member,s->value.script,l,t,r,b,dg_opacity(s));
  }
  unsigned humans,computers;match_counts(&humans,&computers);
  printf("],\"match_humans\":%u,\"match_computers\":%u",humans,computers);
  native_pointer_state(&pointer);puts("}");fflush(stdout);
}
