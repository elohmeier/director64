"""Execute D8 source lowering against native objects and GC continuations."""

import shutil
import subprocess

import pytest

from director64.aot import generate
from director64.lingo import parse_movie


def test_decompiled_pi_call_initializes_local_before_heading_math(tmp_path):
    run_native(tmp_path, """-- cast: Internal; member: 1; type: MovieScript; name: Test
global east, north
on start
  set pi to PI
  set east to sin(pi / 2)
  set north to cos(PI)
end
""", 'assert(lv_numeric(global("east")) > 0.999); assert(lv_numeric(global("north")) < -0.999);')


def run_native(tmp_path, source, assertions):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("native C compiler unavailable")
    generate({"handlers": parse_movie(source, "FIXTURE.DXR")}, tmp_path)
    (tmp_path / "harness.c").write_text(
        r"""
#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern const lv_movie_t aot_fixture_dxr;
static lv_runtime_t runtime;
static unsigned yields;
static const char *const names[]={
#include "globals.inc"
};
static lv_t script(lv_runtime_t *r,const lv_movie_t *m,const lv_handler_t *h) {
    (void)r; (void)m;
    return lv_make(LV_SCRIPT, h->member);
}
static lv_t reference(lv_runtime_t *r,const char *kind,lv_t name,lv_t library) {
    (void)library;
    assert(!strcmp(kind,"script"));
    assert(!strcmp(lv_cstr(r,name),"actor"));
    return lv_make(LV_SCRIPT, 2);
}
static const lv_handler_t *resolve(lv_runtime_t *r,lv_t receiver,const char *name,
                                   const lv_movie_t **movie) {
    (void)r;
    *movie=&aot_fixture_dxr;
    return lv_find(NULL,*movie,name,(unsigned)lv_id(receiver));
}
static bool call(lv_runtime_t *r,const char *name,unsigned argc,const lv_t *args,
                  lv_t *value,bool *yield) {
    (void)r; (void)argc; (void)args; (void)value;
    if (strcmp(name,"updatestage")) return false;
    *yield=true; yields++; return true;
}
static lv_t global(const char *name) {
    int index=lv_global_id(&runtime,name);
    assert(index>=0);
    return runtime.globals[index];
}
int main(void) {
    lv_init(&runtime,(lv_services_t){.call=call,.script=script,.reference=reference,
                                    .resolve=resolve},NULL,names,sizeof(names)/sizeof(*names),1);
    runtime.current=&aot_fixture_dxr;
    assert(lv_start(&runtime,&aot_fixture_dxr,"start",1,0,NULL));
    unsigned ticks=0;
    while(runtime.depth && ticks++<10000) {
        if (!lv_run(&runtime,3)) {
            fprintf(stderr,"%s\n",runtime.error);
            return 1;
        }
        // Deliberately collect between every small state-machine budget and
        // yield: selectors, receivers and arguments must be explicit roots.
        lv_collect(&runtime);
    }
    assert(!runtime.depth && !runtime.failed);
"""
        + assertions
        + "\n}\n"
    )
    executable = tmp_path / "native-test"
    subprocess.run(
        [
            cc,
            "-std=c17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-DDIRECTOR64_DIRECTOR_VERSION=8",
            "-Iruntime/lingo",
            str(tmp_path / "harness.c"),
            str(tmp_path / "fixture_dxr.c"),
            str(tmp_path / "symbols.c"),
            "runtime/lingo/lingo_runtime.c",
            "-lm",
            "-o",
            str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True)


def test_cached_foreach_case_and_property_loop(tmp_path):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: main
on start
  global result, values, selections, listCalls, propertyResult, propSum, dispatched
  values = [1, 2]
  result = 0
  selections = 0
  listCalls = 0
  repeat with item in getValues()
    case selectItem(item) of
      1:
        result = result + 10
      2, 3:
        result = result + 20
      otherwise:
        result = 999
    end case
    values.add(50)
  end repeat
  actor = script("actor").new()
  propertyResult = actor.tally()
  propSum = 0
  repeat with item in [#first: 4, #second: 8]
    propSum = propSum + item
  end repeat
  do("stamp")
end
on stamp
  global dispatched
  updateStage()
  dispatched = 1
end
on getValues
  global listCalls, values
  listCalls = listCalls + 1
  updateStage()
  return values
end
on selectItem value
  global selections
  selections = selections + 1
  updateStage()
  return value
end
-- cast: Internal; member: 2; type: MovieScript; name: actor
property counter, total
on new me
  total = 0
  return me
end
on tally me
  repeat with counter = 1 to 3
    total = total + counter
    updateStage()
  end repeat
  return [counter, total]
end
"""
    run_native(
        tmp_path,
        source,
        r"""
    assert(lv_numeric(global("result"))==30);
    assert(lv_numeric(global("selections"))==2);
    assert(lv_numeric(global("listcalls"))==1);
    assert(lv_count(&runtime,global("values"))==4);
    assert(lv_numeric(lv_at(&runtime,global("propertyresult"),1))==4);
    assert(lv_numeric(lv_at(&runtime,global("propertyresult"),2))==6);
    assert(lv_numeric(global("propsum"))==12);
    assert(lv_numeric(global("dispatched"))==1);
    assert(yields==7);
""",
    )


def test_yielding_method_receiver_arguments_and_chunk_writeback(tmp_path):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: main
on start
  global result, order, chunks, shadow
  order = EMPTY
  result = getActor().join(waitText("A"), waitText("B"))
  state = [#text: "one two three"]
  delete word 2 of state.text
  delete char 1 to 4 of state.text
  chunks = state.text
  shadow = returnValue()
end
on getActor
  global order
  order = order & "receiver"
  updateStage()
  return script("actor").new()
end
on waitText text
  global order
  order = order & text
  text = text & " retained"
  updateStage()
  return text
end
on returnValue
  return = 8
  updateStage()
  return return + 1
end
-- cast: Internal; member: 2; type: MovieScript; name: actor
property text
on new me
  text = "prefix: "
  return me
end
on join me, first, second
  updateStage()
  return text & first & second
end
"""
    run_native(
        tmp_path,
        source,
        r"""
    assert(!strcmp(lv_cstr(&runtime,global("result")),"prefix: A retainedB retained"));
    assert(!strcmp(lv_cstr(&runtime,global("order")),"receiverAB"));
    assert(!strcmp(lv_cstr(&runtime,global("chunks")),"three"));
    assert(lv_numeric(global("shadow"))==9);
    assert(yields==5);
""",
    )


def test_native_dynamic_selectors_route_declared_arguments_and_methods(tmp_path):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: main
on start
  global actor, result
  actor = script("actor").new()
  do("record(actor, 7, #first)")
  do("record actor, 3, #second")
  do("actor.bump(4)")
end
on record who, amount, tag
  global result
  updateStage()
  who.bump(amount)
  result = tag
end
-- cast: Internal; member: 2; type: MovieScript; name: actor
property counter
on new me
  counter = 0
  return me
end
on bump me, amount
  updateStage()
  counter = counter + amount
end
"""
    run_native(
        tmp_path,
        source,
        r"""
    assert(lv_type(global("actor"))==LV_INSTANCE);
    assert(lv_numeric(lv_get(&runtime,NULL,"counter",global("actor")))==14);
    assert(!strcmp(lv_cstr(&runtime,global("result")),"second"));
    assert(yields==5);
""",
    )


def test_sound_options_type_query_preserves_playlist_shape(tmp_path):
    # GENMODUL.CXT genLjudModul:new/spelaNasta, reached by KP:springljud.
    source = """-- cast: Internal; member: 1; type: MovieScript; name: main
on start
  global firstSound, secondSound, firstLoop, secondLoop
  options = [#sound: "kp_009_spring_1", #Typ: #vanlig, #loopCount: 0]
  player = script("actor").new([options])
  firstSound = player.nextSound()
  firstLoop = player.loopCount
  player = script("actor").new([#sound: "kp_103_ticktack", #loopCount: -1])
  secondSound = player.nextSound()
  secondLoop = player.loopCount
end
-- cast: Internal; member: 2; type: ParentScript; name: actor
property minLjudLista, loopCount
on new me, ljudLista
  if (ljudLista.ilk = #string) or (ljudLista.ilk = #propList) or (ljudLista.ilk = #symbol) then
    minLjudLista = [ljudLista]
  else
    minLjudLista = ljudLista
  end if
  return me
end
on nextSound me
  nastaLjud = minLjudLista[1]
  updateStage()
  if nastaLjud.ilk = #propList then
    ljudMember = nastaLjud[#sound]
    loopCount = nastaLjud[#loopCount]
  else
    ljudMember = nastaLjud
  end if
  return ljudMember
end
"""
    run_native(
        tmp_path,
        source,
        r"""
    assert(!strcmp(lv_cstr(&runtime,global("firstsound")),"kp_009_spring_1"));
    assert(!strcmp(lv_cstr(&runtime,global("secondsound")),"kp_103_ticktack"));
    assert(lv_numeric(global("firstloop"))==0);
    assert(lv_numeric(global("secondloop"))==-1);
    assert(yields==2);
""",
    )


def test_abort_unwinds_yielding_method_chain_and_allows_next_event(tmp_path):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: main
on start
  global result
  result = 0
  actor = script("actor").new()
  result = actor.outer()
  result = 999
end
on nextEvent
  global result
  result = result + 10
end
-- cast: Internal; member: 2; type: ParentScript; name: actor
on new me
  return me
end
on outer me
  updateStage()
  return me.inner()
end
on inner me
  global result
  updateStage()
  result = 1
  abort()
  result = 2
  return 3
end
"""
    run_native(
        tmp_path,
        source,
        r"""
    assert(lv_numeric(global("result"))==1);
    assert(yields==2 && runtime.aborted);
    assert(lv_start(&runtime,&aot_fixture_dxr,"nextevent",1,0,NULL));
    assert(lv_run(&runtime,100));
    assert(!runtime.depth && !runtime.aborted && !runtime.failed);
    assert(lv_numeric(global("result"))==11);
""",
    )


def test_declared_property_shadows_implicit_global(tmp_path):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: main
global junk, result
on start
  set junk to 99
  set result to read(new(script "actor"))
end
-- cast: Internal; member: 2; type: ParentScript; name: actor
property junk
on new me
  set junk to 42
  return me
end
on read me
  return junk
end
"""
    run_native(
        tmp_path,
        source,
        'assert(lv_numeric(global("result")) == 42); assert(lv_numeric(global("junk")) == 99);',
    )


def test_long_literal_list_builds_in_segments(tmp_path):
    """A 120-item literal exceeds one expression-stack segment (48 slots)."""
    items = ",".join(str(i * 3) for i in range(1, 121))
    run_native(
        tmp_path,
        f"""-- cast: Internal; member: 1; type: MovieScript; name: Test
global total, width, head, tail
on start
  set stock to [{items}]
  set width to count(stock)
  set total to 0
  repeat with i = 1 to width
    set total to total + getAt(stock, i)
  end repeat
  set head to getAt(stock, 1)
  set tail to getAt(stock, 120)
end
""",
        'assert(lv_numeric(global("width")) == 120); assert(lv_numeric(global("head")) == 3);'
        ' assert(lv_numeric(global("tail")) == 360); assert(lv_numeric(global("total")) == 21780);',
    )
