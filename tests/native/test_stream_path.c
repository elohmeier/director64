#include "stream_path.h"
#include <assert.h>
int main(void) {
  char out[192];
  assert(dg_stream_path("DATA","C:\\DATA\\BK\\","@:sounds:INTRO_1.AIF",out,sizeof(out)));
  assert(!strcmp(out,"bk/sounds/intro_1"));
  assert(dg_stream_path("DATA","C:\\DATA\\BAUWAGEN\\","@::sounds:BWBR01",out,sizeof(out)));
  assert(!strcmp(out,"sounds/bwbr01"));
  assert(dg_stream_path("DATA","C:\\DATA\\BK\\","D:\\DATA\\EVO\\SOUNDS\\INTRO_1.AIF",out,sizeof(out)));
  assert(!strcmp(out,"evo/sounds/intro_1"));
  assert(dg_stream_path("DATA","C:\\DATA\\SLIDE\\BRUECK\\","@:SOUND:wind.aif",out,sizeof(out)));
  assert(!strcmp(out,"slide/brueck/sound/wind"));
  assert(!dg_stream_path("DATA","C:\\DATA\\BK\\","@:::outside",out,sizeof(out)));
  assert(!dg_stream_path("DATA","C:\\DATA\\BK\\","@:sounds:voice.aif",out,3));
  puts("Virtual streamed audio resolves exact movie-relative, parent and absolute disc paths");
}
