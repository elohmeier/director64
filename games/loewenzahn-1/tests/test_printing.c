#include "printing.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static unsigned forwarded;
static bool fallback(lv_runtime_t *r, const char *name, unsigned argc,
                      const lv_t *a, lv_t *out, bool *yield) {
  (void)r; (void)name; (void)argc; (void)a; (void)yield;
  forwarded++; *out=lv_num(99); return true;
}
static void init(void) {
  lv_init(&values,(lv_services_t){.call=fallback},NULL,NULL,0,42);
  game_print_init(&values);
}
static void request(const char *book,double chapter) {
  lv_t args[]={lv_text(&values,book,false),lv_num(chapter)},out={0};bool yield=false;
  assert(values.services.call(&values,"director64_print",2,args,&out,&yield));
  assert(!yield);
}
int main(void) {
  init(); game_print_init(&values); // Installation is safe to repeat, without recursion.
  lv_t out={0};bool yield=false;
  assert(values.services.call(&values,"unrelated",0,NULL,&out,&yield));
  assert(forwarded==1 && lv_numeric(out)==99);
  input_sample_t neutral={.connected=true},b={.connected=true,.buttons=INPUT_B};
  for(unsigned id=1;id<=16;id++) {
    request(id<=8?"BAS":"REZ",(id-1)%8+1);
    assert(!values.failed && game_print_id()==id && game_print_paused());
    char expected[64];snprintf(expected,sizeof(expected),"http://example.test/%u.pdf",id);
    assert(!strcmp(game_print_document()->url,expected));
    assert(game_print_module(game_print_document(),0,0));
    assert(!game_print_module(game_print_document(),1,0));
    assert(!game_print_module(game_print_document(),49,0));
    assert(game_print_input(&b) && game_print_id()==id); // Held on opening must not dismiss.
    assert(game_print_input(&neutral));
    input_sample_t a={.connected=true,.buttons=INPUT_A};
    for(unsigned tick=0;tick<1000;tick++)assert(game_print_input(&a));
    assert(game_print_id()==id); // No timeout and A cannot click the suspended page.
    input_sample_t disconnected={0};
    assert(game_print_input(&disconnected) && game_print_id()==id);
    assert(game_print_input(&b) && !game_print_id() && game_print_paused());
    assert(game_print_input(&b) && game_print_paused());
    assert(game_print_input(&a) && game_print_paused());
    assert(game_print_input(&neutral) && !game_print_paused());
    assert(!game_print_input(&neutral));
  }
  double invalid[]={0,9,1.5,NAN,INFINITY};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
    init();request("BAS",invalid[i]);assert(values.failed && !game_print_paused());
  }
  init();request("OTHER",1);assert(values.failed && !game_print_paused());
  puts("printing service: document selection, forwarding and pause/dismissal contracts passed");
}
