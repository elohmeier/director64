#include "willy_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code={.name="05.DXR"};
static const dg_movie_t movie={.code=&code};
int main(void) {
 dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);d.movie=&movie;
 input_sample_t pad={.connected=true,.buttons=INPUT_B};
 assert(willy_input(&d,&pad));
 assert(lv_numeric(lv_get(&r,NULL,"rightmousedown",(lv_t){0}))==1);
 assert(lv_numeric(lv_get(&r,NULL,"rightmouseup",(lv_t){0}))==0);
 assert(willy_input(&d,&pad) && d.right_mouse_down);
 pad.connected=false;assert(willy_input(&d,&pad) && !d.right_mouse_down);
 pad.connected=true;pad.buttons=0;assert(willy_input(&d,&pad) && !d.right_mouse_down);
 lv_t out={0};bool yield=false;
 assert(r.services.call(&r,"certificate_print_notice",0,NULL,&out,&yield));
 assert(*d.notice && !r.failed);
 pad.buttons=INPUT_A;assert(!willy_input(&d,&pad) && *d.notice);
 pad.buttons=0;assert(!willy_input(&d,&pad) && *d.notice);
 pad.buttons=INPUT_B;assert(!willy_input(&d,&pad) && !*d.notice && d.await_release);
 assert(willy_input(&d,&pad) && !d.right_mouse_down);
 puts("Willy brake/reverse, disconnect and certificate notice input passed");
}
