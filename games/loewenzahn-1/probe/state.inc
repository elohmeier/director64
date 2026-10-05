// Löwenzahn's probe state line, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, pointer, enter(), find() and the output helpers.
#include "pointer_probe.h"
static void state(void) {
  dg_cursor_t cursor=dg_cursor_current(&director);
  dg_cursor_bitmap_t bitmap={0};
  if(cursor.image) {
    const dg_member_t *m=dg_member(&director,cursor.image), *mask=dg_member(&director,cursor.mask);
    if(director.suspended_stage) {
      if(!m) m=dg_member(director.suspended_stage,cursor.image);
      if(!mask) mask=dg_member(director.suspended_stage,cursor.mask);
    }
    (void)native_cursor_bitmap(&values,m,mask,&bitmap);
  } else (void)dg_cursor_builtin(&bitmap,cursor.resource);
  printf("{\"movie\":"); quoted(director.movie ? director.movie->code->name : "");
  printf(",\"cursor\":{\"resource\":%ld,\"image\":%lu,\"mask\":%lu,\"hotspot\":[%u,%u],\"hash\":%lu}",
         (long)cursor.resource,(unsigned long)cursor.image,(unsigned long)cursor.mask,
         bitmap.hot_x,bitmap.hot_y,(unsigned long)dg_cursor_bitmap_hash(&bitmap));
  printf(",\"print_id\":%u,\"print_paused\":%s", game_print_id(), game_print_paused() ? "true":"false");
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
  printf("},\"channel_volumes\":[");
  for(unsigned i=0;i<DG_SOUND_CHANNELS;i++)printf("%s%u",i?",":"",director.channel_volume[i]);
  printf("],\"channel_sounds\":[");
  for(unsigned i=0;i<DG_SOUND_CHANNELS;i++)printf("%s%u",i?",":"",director.sounds[i]);
  printf("],\"channel_busy\":[");
  for(unsigned i=0;i<DG_SOUND_CHANNELS;i++)printf("%s%s",i?",":"",busy(NULL,i)?"true":"false");
  printf("],\"tempo\":%u,\"sprites\":[",director.clock_tempo); comma=false;
  for(unsigned i=1;i<DG_SPRITES;i++) { dg_sprite_t *s=&director.sprites[i]; if(!s->value.type||!s->visible)continue;
    int l,t,r,b; dg_bounds(&director,i,&l,&t,&r,&b); if(director.native_dialog){l+=112;t+=100;r+=112;b+=100;} if(comma)putchar(',');comma=true;
    printf("{\"id\":%u,\"member\":%u,\"script\":%u,\"bounds\":[%d,%d,%d,%d],\"blend\":%u,\"loc\":[%d,%d],\"video_time\":%.3f,\"video_rate\":%.3f}",i,s->value.member,s->value.script,l,t,r,b,dg_opacity(s),s->value.x,s->value.y,s->video_time,s->video_rate);
  }
  printf("]");native_pointer_state(&pointer);puts("}");fflush(stdout);
}
