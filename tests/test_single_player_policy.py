"""Execute manifest policy operations against source-free synthetic handlers."""
import json
import subprocess

import pytest
from lingo_harness import run_lingo

from director64.project import GameSpec, converter


def policies(tmp_path, slug, ids):
    selected = [patch for patch in GameSpec.load(slug).data["convert"]["patch"]
                if patch["id"] in ids]
    assert {patch["id"] for patch in selected} == set(ids)
    assert all(len(patch["source_sha256"]) == 64 for patch in selected)

    def transform(program):
        patches = []
        for patch in selected:
            handler, = [handler for handler in program["handlers"]
                        if all(handler[key] == patch[key]
                               for key in ("movie", "cast", "member"))
                        and handler["name"] == patch["handler"]]
            # The operations come from the shipped manifest. The fixture has
            # its own source pin, rather than any recovered retail script.
            patches.append(patch | {"source_sha256": handler["source_sha256"]})
        for name, data in (("policy-program.json", program), ("policy-source.json", {}),
                           ("policy-port.json", {"slug": slug, "convert": {"patch": patches}})):
            (tmp_path / name).write_text(json.dumps(data))
        result = subprocess.run([
            *converter(), "director", "port-program", tmp_path / "policy-program.json",
            tmp_path / "policy-source.json", tmp_path / "policy-port.json",
        ], capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    return transform


HARNESS = r"""
static lv_runtime_t runtime;
static char key='2';
static bool shift=true,busy;
static char destination[64];
static unsigned stopped;
static bool visible[200];
static lv_t location;
static void set(lv_runtime_t *r,const char *name,lv_t owner,lv_t value){
    assert(lv_type(owner)==LV_SPRITE&&lv_id(owner)<200);
    if(!strcmp(name,"visible"))visible[lv_id(owner)]=lv_integer(r,value)!=0;
    else {assert(!strcmp(name,"loc"));location=value;}
}
static lv_t get(lv_runtime_t *r,const char *name,lv_t owner){
    (void)owner;
    if(!strcmp(name,"shiftdown"))return lv_num(shift);
    if(!strcmp(name,"key")){char text[2]={key,0};return lv_text(r,text,false);}
    assert(0);return (lv_t){0};
}
static bool call(lv_runtime_t *r,const char *name,unsigned argc,const lv_t *args,
                 lv_t *value,bool *yield){
    (void)yield;
    if(!strcmp(name,"go")){
        assert(argc==1);snprintf(destination,sizeof(destination),"%s",lv_cstr(r,args[0]));
        return true;
    }
    if(!strcmp(name,"soundbusy")){assert(argc==1);*value=lv_num(busy);return true;}
    if(!strcmp(name,"point")){
        assert(argc==2);*value=lv_point(r,lv_numeric(args[0]),lv_numeric(args[1]));return true;
    }
    if(!strcmp(name,"sound_stop")){
        assert(argc==1);stopped|=1u<<lv_integer(r,args[0]);return true;
    }
    return false;
}
static lv_t global(const char *name){
    int id=lv_global_id(&runtime,name);assert(id>=0);return runtime.globals[id];
}
static void run(const char *name,unsigned member){
    assert(lv_start(&runtime,fixture,name,member,0,NULL));
    unsigned guard=0;
    while(runtime.depth&&guard++<1000){
        if(!lv_run(&runtime,64)){fprintf(stderr,"%s\n",runtime.error);assert(0);}
    }
    assert(!runtime.depth&&!runtime.failed);
}
int main(int argc,char **argv){
    (void)argc;(void)key;(void)shift;(void)busy;(void)stopped;
    (void)global;load_fixture(argv[1]);
    lv_init(&runtime,(lv_services_t){.call=call,.get=get,.set=set},NULL,global_names,global_count,1);
    runtime.current=fixture;
"""


def test_ball_debug_chord_cannot_activate_second_human(tmp_path):
    source = """-- cast: Script; member: 10; type: MovieScript; name: fixture
global gtypavspel, originalCalls
on keyDown
  originalCalls = originalCalls + 1
  if the shiftDown and (the key = "2") then
    gtypavspel = #tvaspelare
  end if
  if the shiftDown and (the key = "1") then
    gtypavspel = #enSpelare
  end if
end
"""
    run_lingo(tmp_path, source, HARNESS + r"""
    runtime.globals[lv_global_id(&runtime,"gtypavspel")]=lv_text(&runtime,"enSpelare",true);
    run("keydown",10);
    assert(!strcmp(lv_cstr(&runtime,global("gtypavspel")),"enSpelare"));
    assert(lv_integer(&runtime,global("originalcalls"))==0);
    key='1';run("keydown",10);assert(lv_integer(&runtime,global("originalcalls"))==1);
    shift=false;key='2';run("keydown",10);
    assert(lv_integer(&runtime,global("originalcalls"))==2);
    assert(!strcmp(lv_cstr(&runtime,global("gtypavspel")),"enSpelare"));
}
""", version=800, movie="KB.DXR", cast_name="Script", program_transform=policies(
        tmp_path, "findus-mucklas", ["single-player-ball-shortcut"]))


def test_race_hides_human_opponent_and_keeps_computer_hit_target(tmp_path):
    source = """-- cast: script; member: 1; type: MovieScript; name: fixture
global gFastaSprites, originalCalls
on mainloop
  originalCalls = originalCalls + 1
end
-- cast: script; member: 2; type: MovieScript; name: fixture
global gKlickSprite
on kollaKlick
  return gKlickSprite
end
"""
    run_lingo(tmp_path, source, HARNESS + r"""
    lv_t controls=lv_list(&runtime,0,NULL,true);
    runtime.globals[lv_global_id(&runtime,"gfastasprites")]=controls;
    lv_set(&runtime,NULL,"kompis",controls,lv_num(141));
    lv_set(&runtime,NULL,"kompisikon",controls,lv_num(95));
    lv_set(&runtime,NULL,"dator",controls,lv_num(142));
    visible[141]=visible[95]=visible[142]=true;
    lv_t entries[]={lv_text(&runtime,"kompis",true),lv_text(&runtime,"dator",true),
                    lv_text(&runtime,"kompisikon",true),lv_text(&runtime,"utpil",true)};
    runtime.globals[lv_global_id(&runtime,"gklicksprite")]=lv_list(&runtime,4,entries,false);
    run("mainloop",1);
    assert(!visible[141]&&!visible[95]&&visible[142]);
    assert(lv_integer(&runtime,global("originalcalls"))==1);
    run("kollaklick",2);
    lv_t hits=global("gklicksprite");assert(lv_count(&runtime,hits)==2);
    assert(!strcmp(lv_cstr(&runtime,lv_at(&runtime,hits,1)),"dator"));
    assert(!strcmp(lv_cstr(&runtime,lv_at(&runtime,hits,2)),"utpil"));
}
""", version=800, movie="KR.DXR", cast_name="script", program_transform=policies(
        tmp_path, "findus-mucklas", ["single-player-race-controls",
                                  "single-player-race-hit-list"]))


def test_ships_removes_hidden_rectangle_before_both_hit_tests(tmp_path):
    source = """-- cast: script; member: 1; type: MovieScript; name: fixture
global gFastaSprites, originalCalls
on introloop
  originalCalls = originalCalls + 1
end
on mainloop
  originalCalls = originalCalls + 1
end
-- cast: script; member: 2; type: MovieScript; name: fixture
on kollaVariantKlick
  return #untouched
end
on vilkenklick
  return #untouched
end
"""
    run_lingo(tmp_path, source, HARNESS + r"""
    lv_t controls=lv_list(&runtime,0,NULL,true);
    runtime.globals[lv_global_id(&runtime,"gfastasprites")]=controls;
    lv_set(&runtime,NULL,"tvaspelare",controls,lv_num(67));
    lv_set(&runtime,NULL,"enspelare",controls,lv_num(68));
    visible[67]=visible[68]=true;
    run("introloop",1);assert(!visible[67]&&visible[68]);
    visible[67]=true;run("mainloop",1);assert(!visible[67]&&visible[68]);
    run("kollavariantklick",2);
    assert(lv_integer(&runtime,lv_at(&runtime,location,1))==-10000);
    assert(lv_integer(&runtime,lv_at(&runtime,location,2))==-10000);
    location=(lv_t){0};run("vilkenklick",2);
    assert(lv_integer(&runtime,lv_at(&runtime,location,1))==-10000);
    assert(lv_integer(&runtime,global("originalcalls"))==2);
}
""", version=800, movie="SS.DXR", cast_name="script", program_transform=policies(
        tmp_path, "findus-mucklas", ["single-player-ships-intro", "single-player-ships-game",
                                  "single-player-ships-variant-hit",
                                  "single-player-ships-control-hit"]))


@pytest.mark.parametrize(("movie", "policy"), [
    ("QUIZ__QUIZ.DXR", "single-player-quiz-entry"),
    ("FLIPPER__FLIPPER.DXR", "single-player-pinball-entry"),
])
def test_hotseat_chooser_bypasses_to_single_player_without_erasing_names(tmp_path, movie, policy):
    source = """-- cast: Interne; member: 37; type: MovieScript; name: fixture
global originalCalls, historicalNames
on prepareMovie
  originalCalls = originalCalls + 1
end
"""
    run_lingo(tmp_path, source, HARNESS + r"""
    lv_t names=lv_text(&runtime,"retained profile data",false);
    runtime.globals[lv_global_id(&runtime,"historicalnames")]=names;
    run("preparemovie",37);
    assert(!strcmp(destination,"1_wer"));
    assert(lv_integer(&runtime,global("originalcalls"))==1);
    assert(global("historicalnames")==names);
}
""", version=850, extended=True, movie=movie, cast_name="Interne", program_transform=policies(
        tmp_path, "loewenzahn-optik", [policy]))


def test_telephone_keeps_intro_wait_and_restarts_single_player(tmp_path):
    source = """-- cast: game; member: 20; type: BehaviorScript; name: fixture
global originalCalls
on exitFrame
  originalCalls = originalCalls + 1
end
-- cast: game; member: 4; type: MovieScript; name: fixture
on doAgain
  go("unused")
end
"""
    run_lingo(tmp_path, source, HARNESS + r"""
    busy=true;run("exitframe",20);
    assert(lv_integer(&runtime,global("originalcalls"))==1&&!destination[0]);
    busy=false;run("exitframe",20);
    assert(lv_integer(&runtime,global("originalcalls"))==1&&!strcmp(destination,"1"));
    destination[0]=0;run("doagain",4);
    assert(!strcmp(destination,"1")&&stopped==((1u<<3)|(1u<<4)));
}
""", version=850, extended=True, movie="TELEFONM.DXR", cast_name="game", program_transform=policies(
        tmp_path, "loewenzahn-8", ["single-player-telephone-intro",
                                 "single-player-telephone-restart"]))
