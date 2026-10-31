// Compare freshly measured synthetic original-engine observations with our
// implementation. Inputs/expected values come from the original, never from us.
#include "ink.h"
#include <stdio.h>
int main(void) {
 unsigned ink,s,f,b,r,g,blue,rows=0,errors=0;
 while(scanf("%u %u %u %u %u %u %u",&ink,&s,&f,&b,&r,&g,&blue)==7){
  unsigned actual=ink==40?dg_ink_lighten_channel(s,f,b):dg_ink_tint_channel(s,f,b);
  if(actual!=r||actual!=g||actual!=blue){
   fprintf(stderr,"ink%u src%u fore%u back%u: port%u original(%u,%u,%u)\n",ink,s,f,b,actual,r,g,blue);errors++;
  }rows++;
 }
 printf("%u original observations, %u mismatches\n",rows,errors);
 return errors||rows!=448;
}
