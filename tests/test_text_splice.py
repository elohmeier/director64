"""Lingo formatting ranges survive member chunk edits through the real VM."""

import pytest
from lingo_harness import run_lingo


@pytest.mark.parametrize("version", [600, 850, 1000])
def test_member_put_delete_keep_references_and_shift_character_styles(tmp_path, version):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: fixture
on start
  global firstText, firstOwner, bSpacing, iSpacing, cSpacing
  global finalText, finalOwner, finalSize, plain
  global rangeColor, rangeFore, memberFore, nestedFore
  global selectedStart, selectedEnd, collapsedStart, collapsedEnd
  global gapText, gapOwner
  member1 = member(7)
  member1.text = "ABCD"
  member1.color = rgb(17, 34, 51)
  memberFore = member1.foreColor
  member1.char[2].foreColor = 255
  member1.char[2].charSpacing = 4
  put "i" after member1.char[2]
  delete member1.char[1]
  put "Z" before member1.char[1]
  firstText = member1.text
  firstOwner = member1
  bSpacing = member1.char[2].charSpacing
  iSpacing = member1.char[3].charSpacing
  cSpacing = member1.char[4].charSpacing
  rangeColor = member1.char[3].color
  rangeFore = member1.char[3].foreColor
  nestedFore = member1.word[1].char[4].foreColor
  member1.text = "äbc"
  member1.char[2].fontSize = 24
  put "x" before member1.char[1]
  delete member1.char[2]
  finalText = member1.text
  finalOwner = member1
  finalSize = member1.char[2].fontSize
  plain = "äbc".char[2]
  hilite member1.word[1].char[2..3]
  selectedStart = the selStart
  selectedEnd = the selEnd
  hilite char (the maxInteger) of member1
  collapsedStart = the selStart
  collapsedEnd = the selEnd
  member1.text = EMPTY
  repeat with n = 1 to 4
    member1.line[n] = string(n)
  end repeat
  gapText = member1.text
  gapOwner = member1
end
"""
    run_lingo(tmp_path, source, r"""
static lv_runtime_t values;
static dg_runtime_t director;
static uint32_t palette[256];
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t style={.font_name="Arial",.font_id=1,.size=18,
 .ascent=17,.descent=4,.line_height=21};
static const dg_text_format_t formats[]={{.font=1,.size=18,.ascent=17,.line_height=21}};
static const dg_member_t members[]={{.id=DG_ID(1,1,7),.number=7,.cast=1,.type=3,
 .width=100,.height=42,
 .name="Field",.text="",.text_style=&style,.text_formats=formats,.text_format_count=1}};
static const dg_frame_t frames[]={{0,0}};
static lv_t global(const char *name) {
 int id=lv_global_id(&values,name);assert(id>=0);return values.globals[id];
}
int main(int argc,char **argv) {
 (void)argc;load_fixture(argv[1]);
 for(unsigned i=0;i<256;i++)palette[i]=0xffffff;
 palette[239]=0x112233;palette[255]=0;
 dg_movie_t movie={.id=1,.code=fixture,.casts=casts,.cast_count=1,.members=members,
  .member_count=1,.frames=frames,.frame_count=1,.palette=palette};
 dg_init(&director,&values,(dg_platform_t){0},NULL,global_names,global_count,1);
 assert(dg_enter(&director,&movie,1,NULL,0));assert(lv_start(&values,fixture,"start",1,0,NULL));
 for(unsigned i=0;values.depth&&i<2000;i++) {
  if(!lv_run(&values,2)){fprintf(stderr,"%s\n",values.error);return 1;}
  lv_collect(&values);
 }
 assert(!values.failed&&!values.depth);
 assert(lv_type(global("firstowner"))==LV_MEMBER&&(uint32_t)lv_id(global("firstowner"))==members[0].id);
 assert(!strcmp(lv_cstr(&values,global("firsttext")),"ZBiCD"));
 assert(lv_integer(&values,global("bspacing"))==4&&lv_integer(&values,global("ispacing"))==4&&lv_integer(&values,global("cspacing"))==0);
 assert(lv_type(global("rangecolor"))==LV_COLOR&&lv_integer(&values,global("rangecolor"))==0);
 assert(lv_integer(&values,global("rangefore"))==255);
 assert(lv_integer(&values,global("memberfore"))==239);
 assert(lv_integer(&values,global("nestedfore"))==239);
 assert(lv_integer(&values,global("selectedstart"))==1);
 assert(lv_integer(&values,global("selectedend"))==3);
 assert(lv_integer(&values,global("collapsedstart"))==3);
 assert(lv_integer(&values,global("collapsedend"))==3);
 assert(director.hilite_member==members[0].id&&director.hilite_start==3&&director.hilite_end==3);
 assert(lv_type(global("finalowner"))==LV_MEMBER&&(uint32_t)lv_id(global("finalowner"))==members[0].id);
 assert(!strcmp(lv_cstr(&values,global("finaltext")),"xbc"));
 assert(lv_integer(&values,global("finalsize"))==24);
 assert(!strcmp(lv_cstr(&values,global("plain")),"b"));
 assert(!strcmp(lv_cstr(&values,global("gaptext")),"1\r2\r3\r4"));
 assert(lv_type(global("gapowner"))==LV_MEMBER);
}
""", version=version, extended=True, director=True)
