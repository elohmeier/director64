// A Rust-emitted synthetic D8.5 package: verify the linked-movie descriptor
// reaches exactly the C structure the child-player uses.
#include "package.h"
#include "film_track.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 2 || (argc == 3 && !strcmp(argv[2], "reject")));
  FILE *input = fopen(argv[1], "rb");
  assert(input && !fseek(input, 0, SEEK_END));
  long length = ftell(input);
  assert(length > 32 && !fseek(input, 0, SEEK_SET));
  uint8_t *bytes = malloc((size_t)length);
  assert(bytes && fread(bytes, 1, (size_t)length, input) == (size_t)length);
  fclose(input);
  dg_package_t package = {0};
  char error[256];
  bool loaded=dg_package_load(&package, bytes, (size_t)length, bytes + 12, error, sizeof(error));
  if (argc==3) {
    assert(!loaded && error[0]);
    free(bytes);
    return 0;
  }
  if (!loaded) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  assert(package.movie_count == 2 && package.registry[0]->member_count == 6);
  assert(package.registry[0]->casts[0].shape_patterns);
  assert(!strcmp(package.registry[0]->casts[0].shape_patterns[0],"brush.fdi"));
  assert(!package.registry[0]->casts[0].shape_patterns[1]);
  assert(package.registry[1]->members[2].palette_count==256);
  assert(package.registry[1]->members[2].palette[17]==0x123456);
  const dg_member_t *member = &package.registry[0]->members[0];
  assert(member->type == 9 && member->linked_movie == 2 && member->linked_flags == 0x19);
  assert(member->linked_left == 192 && member->linked_top == -12);
  assert(member->width == 200 && member->height == 100);
  const dg_member_t *film = &package.registry[0]->members[1];
  assert(film->film_count == 3 && film->film_sounds);
  assert(film->film_sounds[0] == DG_ID(2, 1, 2) && film->film_sounds[1] == 0);
  assert(film->film_sounds[2] == 0 && film->film_sounds[3] == 0);
  assert(film->film_sounds[4] == 0 && film->film_sounds[5] == DG_ID(2, 1, 2));
  assert(!film->film_frames && film->film_left == -9 && film->film_top == 3);
  assert(film->flash_stop_count == 1 && film->flash_stops[0] == 3);
  assert(film->film_track_count == 2);
  assert(film->film_flags == 14);
  const dg_film_track_t *tracks=film->film_tracks;
  assert(tracks[0].channel==1 && tracks[1].channel==3);
  assert(tracks[0].count==2 && tracks[1].count==2);
  uint32_t offset=0;uint16_t frame=0;dg_spec_t first={0};
  assert(dg_film_track_read(&tracks[0],&offset,&frame,&first) && frame==0);
  assert(first.member==DG_ID(2,1,1));
  assert(first.fore_rgb==0x123456 && first.back_rgb==0xabcdef);
  assert(first.flags==0 && first.stretch==1 && first.blend==80 && first.thickness==18);
  assert(first.x==11 && first.y==7);
  assert(first.rotation==-375 && first.skew==125);
  assert(dg_film_track_read(&tracks[0],&offset,&frame,&first) && frame==2);
  assert(first.member==0 && first.type==0 && offset==tracks[0].size);
  offset=0;memset(&first,0,sizeof(first));
  assert(dg_film_track_read(&tracks[1],&offset,&frame,&first) && frame==0);
  unsigned remaining=tracks[1].size-offset;
  assert(dg_film_track_read(&tracks[1],&offset,&frame,&first) && frame==2);
  assert(first.x==17 && first.y==7 && offset==tracks[1].size);
  assert(remaining==6); // frame+mask+x; unchanged state is never repeated.
  assert(package.registry[0]->members[2].film_sounds == NULL);
  const dg_member_t *flash = &package.registry[0]->members[3];
  assert(flash->film_count == 1 && flash->film_frames[0].count == 2);
  const dg_film_layer_t *sun = flash->film_frames[0].layers;
  assert(sun->value.member == 0 && sun->value.ink == 8 && sun->value.blend == 100);
  assert(sun->channel == 2 && sun->value.x == 9 && sun->value.y == 12);
  assert(sun->value.width == 5 && sun->value.height == 7);
  assert(!strcmp(sun->asset, "sun.fda") && !strcmp(sun->name, "sun"));
  assert(sun->origin_x == 163840 && sun->origin_y == -32768);
  assert(sun[1].asset == NULL && !strcmp(sun[1].name, "my_txt"));
  assert(sun[1].affine && sun[1].matrix[0] == 65536 && sun[1].matrix[1] == 32768);
  assert(sun[1].matrix[2] == -16384 && sun[1].matrix[3] == 98304);
  assert(sun[1].matrix[4] == 655360 && sun[1].matrix[5] == 720896);
  assert(flash->flash_field_count == 1 && flash->flash_fields[0].local_bounds[0] == -16384);
  assert(flash->flash_fields[0].embed_fonts);
  assert(flash->flash_fields[0].local_bounds[1] == 32768);
  assert(flash->flash_fields[0].local_bounds[2] == 1327104);
  assert(flash->flash_fields[0].local_bounds[3] == 557056);
  assert(flash->flash_font_count == 1 && flash->flash_fonts[0]->font_id == 3);
  assert(!strcmp(flash->flash_fonts[0]->font_name,"Synthetic Original"));
  assert(flash->flash_fonts[0]->size == 12 && flash->flash_fonts[0]->ascent == 9);
  assert(flash->flash_fonts[0]->advances && flash->flash_fonts[0]->advances[0] == 6);
  assert(flash->flash_helper_count == 2 && flash->flash_helpers[0].operation == 1);
  assert(!strcmp(flash->flash_helpers[0].name, "advance"));
  assert(!strcmp(flash->flash_helpers[0].target, "sun") && !strcmp(flash->flash_helpers[0].property, "_y"));
  assert(flash->flash_helpers[0].argument == 0);
  assert(!flash->flash_helpers[0].method && flash->flash_helpers[1].method);
  assert(flash->flash_helpers[1].operation == 4 && !strcmp(flash->flash_helpers[1].target, "my_txt"));
  const dg_member_t *variants = &package.registry[0]->members[4];
  assert(variants->film_count == 2 && variants->flash_variant_count == 3);
  assert(!strcmp(variants->film_assets[0], "0a.fdi"));
  assert(!strcmp(variants->film_assets[5], "1a.fdi"));
  assert(variants->source_xtra == 1 && variants->film_loop == 1);
  assert(variants->flash_rate == 24 * 65536 && variants->rate == 44100 && variants->samples == 3356);
  assert(!strcmp(variants->video_audio, "intro.wav64"));
  assert(variants->flash_audio_positions[0] == 0 && variants->flash_audio_positions[1] == 1000);
  assert(variants->flash_playback_mode == 1 && variants->flash_fixed_rate == 15 * 65536);
  assert(variants->flash_direct_to_stage == 1);
  const dg_member_t *interactive=&package.registry[0]->members[5];
  assert(interactive->flash_version==7);
  assert(interactive->flash_initializer_count==1 && interactive->flash_initializers[0].clip_id==2);
  assert(interactive->flash_initializers[0].action.frame==1 && interactive->flash_initializers[0].action.code[0]==7);
  assert(interactive->flash_clip_count==2 && interactive->flash_button_count==1);
  assert(interactive->flash_sound_count==1 && interactive->flash_export_count==1);
  const dg_film_layer_t *root=interactive->film_frames[0].layers;
  assert(root->clip_id==2 && root->placement_generation==3 && root->clip_depth==5);
  assert(root->has_cxform && root->cxform[0]==128 && root->cxform[7]==4);
  assert(root->action_count==1 && root->actions[0].event==0x20000 && root->actions[0].key==65);
  assert(root->actions[0].length==2 && root->actions[0].code[0]==7);
  const dg_flash_clip_t *clip=&interactive->flash_clips[1];
  assert(clip->id==2 && clip->frame_count==1 && clip->local_bounds[0]==-32768);
  assert(clip->frames[0].layers[0].button_id==3);
  assert(clip->label_count==1 && !strcmp(clip->labels[0].name,"start"));
  assert(clip->sound_event_count==1 && clip->sound_events[0].sound==4);
  assert(clip->sound_events[0].loops==2 && clip->sound_events[0].out_point==100);
  assert(clip->sound_events[0].envelope_count==2 && clip->sound_events[0].envelopes[1].position==441);
  assert(interactive->flash_buttons[0].track_as_menu && interactive->flash_buttons[0].states[1].count==0);
  assert(interactive->flash_buttons[0].actions[0].event==4);
  assert(!strcmp(interactive->flash_buttons[0].states[0].layers[0].mask_asset,"button-mask.fda"));
  assert(!strcmp(interactive->flash_sounds[0].asset,"click.wav64") && interactive->flash_sounds[0].rate==22050);
  assert(interactive->flash_exports[0].id==2 && !strcmp(interactive->flash_exports[0].name,"ChildSymbol"));
  assert(package.registry[1]->text_extra_count == 1);
  const dg_text_extra_t *extra = package.registry[1]->text_extras;
  assert(extra->run_count == 2 && extra->link_count == 1);
  assert(extra->runs[0].offset == 0 && extra->runs[0].rgb == 0xff0000);
  assert(extra->runs[1].offset == 1 && extra->runs[1].rgb == 0x0000ff);
  assert(extra->links[0].first == 1 && extra->links[0].last == 2);
  dg_package_free(&package);
  free(bytes);
  puts("Linked movie package metadata round trip passed");
  return 0;
}
