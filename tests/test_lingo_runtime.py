"""Lingo run end to end on the runtime: control flow, yields, objects and
collection, from source through a converted package (lingo_harness.py)."""

import pytest
from lingo_harness import run_lingo

D8_HARNESS = r"""
static lv_runtime_t runtime;
static unsigned yields;
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
    *movie=fixture;
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
int main(int argc, char **argv) {
    (void)argc;
    load_fixture(argv[1]);
    lv_init(&runtime,(lv_services_t){.call=call,.script=script,.reference=reference,
                                    .resolve=resolve},NULL,global_names,global_count,1);
    runtime.current=fixture;
    assert(lv_start(&runtime,fixture,"start",1,0,NULL));
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


def run_d8(tmp_path, source, assertions):
    run_lingo(tmp_path, source, D8_HARNESS + assertions + "\n}\n", version=800)


def test_control_flow_yields_and_results(tmp_path):
    source = """-- cast: Internal; member: 1; type: MovieScript; name: fixture
on start
  global answer, aliased, nested, stringResult, retained
  set a to [1, 2]
  set aliased to a
  set answer to 0
  repeat with i = 1 to 5
    if i = 2 then
      next repeat
    end if
    set answer to answer + twice(i)
    if i = 4 then
      exit repeat
    end if
  end repeat
  setAt(a, 2, answer)
  repeat while answer < 20
    set answer to answer + 1
    updateStage()
  end repeat
  set nested to twice(waitFor(2)) + waitFor(twice(3))
  set stringResult to "kept alive" & waitFor(8)
  repeat while waitFor(nested) < 12
    set nested to nested + 1
  end repeat
  twice(7)
  set ignored to count([1, 2, 3])
  set ignored to twice(5)
  updateStage()
  noResult()
  set retained to the result
end
on twice n
  return n * 2
end
on waitFor n
  updateStage()
  return n
end
on noResult
  updateStage()
end
"""
    run_lingo(tmp_path, source, r"""
static lv_runtime_t runtime;
static unsigned yields;
static bool call(lv_runtime_t *r,const char *name,unsigned n,const lv_t *a,lv_t *v,bool *y) {
    (void)r; (void)n; (void)a; (void)v;
    if (strcmp(name,"updatestage")) return false;
    *y=true; yields++; return true;
}
int main(int argc, char **argv) {
    (void)argc;
    load_fixture(argv[1]);
    lv_init(&runtime,(lv_services_t){.call=call},0,global_names,global_count,1);
    assert(lv_start(&runtime,fixture,"start",0,0,0));
    unsigned ticks=0;
    while(runtime.depth && ticks++<1000) assert(lv_run(&runtime,7));
    assert(!runtime.depth && !runtime.failed && yields==12);
    assert(lv_number(&runtime,runtime.globals[lv_global_id(&runtime,"retained")])==14);
    assert(lv_number(&runtime,runtime.globals[lv_global_id(&runtime,"answer")])==20);
    assert(lv_number(&runtime,lv_at(&runtime,runtime.globals[lv_global_id(&runtime,"aliased")],2))==16);
    assert(lv_number(&runtime,runtime.globals[lv_global_id(&runtime,"nested")])==12);
    lv_t text=runtime.globals[lv_global_id(&runtime,"stringresult")];
    assert(!strcmp(lv_cstr(&runtime,text),"kept alive8"));
}
""")


@pytest.mark.parametrize("condition", ["the timer < 3", "the mouseDown = 0", "soundBusy(1)"])
def test_stage_updates_in_polling_loop(tmp_path, condition):
    source = f"""-- cast: Internal; member: 1; type: MovieScript; name: fixture
on start
  global iterations, finished
  set iterations to 0
  repeat while {condition}
    set iterations to iterations + 1
    updateStage()
  end repeat
  set finished to 1
end
"""
    run_lingo(tmp_path, source, r"""
#include "director.h"
static lv_runtime_t values;
static dg_runtime_t director;
static bool busy(void *context, unsigned channel) {
    (void)context; (void)channel;
    return director.ticks < 3;
}
int main(int argc, char **argv) {
    (void)argc;
    load_fixture(argv[1]);
    const dg_frame_t frame={0};
    const dg_movie_t movie={.code=fixture,.id=1,.tempo=1,
                           .frame_count=1,.frames=&frame};
    dg_init(&director,&values,(dg_platform_t){.sound_busy=busy},NULL,
            global_names,global_count,1);
    assert(dg_enter(&director,&movie,1,NULL,0));
    assert(lv_start(&values,fixture,"start",0,0,NULL));
    for (int tick=1;tick<=2;tick++) {
        assert(dg_tick(&director,0,0,false,2048));
        assert(values.yielded && values.depth);
        assert(lv_integer(&values,values.globals[lv_global_id(&values,"iterations")])==tick);
        assert(values.steps < 100);
    }
    assert(dg_tick(&director,0,0,true,2048));
    assert(!values.depth);
    assert(lv_integer(&values,values.globals[lv_global_id(&values,"finished")])==1);
}
""", director=True)


def test_decompiled_pi_call_initializes_local_before_heading_math(tmp_path):
    run_d8(tmp_path, """-- cast: Internal; member: 1; type: MovieScript; name: Test
global east, north
on start
  set pi to PI
  set east to sin(pi / 2)
  set north to cos(PI)
end
""", 'assert(lv_numeric(global("east")) > 0.999); assert(lv_numeric(global("north")) < -0.999);')


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
    run_d8(
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
    run_d8(
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
    run_d8(
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
    run_d8(
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
    run_d8(
        tmp_path,
        source,
        r"""
    assert(lv_numeric(global("result"))==1);
    assert(yields==2 && runtime.aborted);
    assert(lv_start(&runtime,fixture,"nextevent",1,0,NULL));
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
    run_d8(
        tmp_path,
        source,
        'assert(lv_numeric(global("result")) == 42); assert(lv_numeric(global("junk")) == 99);',
    )


def test_long_literal_list_builds_in_segments(tmp_path):
    """A 120-item literal exceeds one expression-stack segment (48 slots)."""
    items = ",".join(str(i * 3) for i in range(1, 121))
    run_d8(
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


@pytest.mark.parametrize('version', [600, 850])
def test_field_values_are_text_but_property_and_assignment_owners_are_references(tmp_path, version):
    source = '''-- cast: Internal; member: 1; type: MovieScript; name: fixture
on start
  global profile, fontSeen, afterWrite
  field("input") = "REX"
  profile = [#name: field("input")]
  fontSeen = field("input").font
  the text of field "input" = "ANNA"
  afterWrite = field("input")
end
'''
    body = r'''
static lv_runtime_t r;
static char text[32];
static lv_t ref(lv_runtime_t *v,const char *kind,lv_t name,lv_t lib) {
  (void)lib;assert(!strcmp(kind,"field")&&!strcmp(lv_cstr(v,name),"input"));
  return lv_make(LV_FIELD,42);
}
static lv_t get(lv_runtime_t *v,const char *name,lv_t owner) {
  assert(lv_type(owner)==LV_FIELD&&lv_id(owner)==42);
  return lv_text(v,!strcmp(name,"font")?"Face":text,false);
}
static void set(lv_runtime_t *v,const char *name,lv_t owner,lv_t value) {
  assert(lv_type(owner)==LV_FIELD&&lv_id(owner)==42&&!strcmp(name,"text"));
  snprintf(text,sizeof(text),"%s",lv_cstr(v,value));
}
int main(int argc,char **argv) {
  (void)argc;load_fixture(argv[1]);
  lv_init(&r,(lv_services_t){.reference=ref,.get=get,.set=set},NULL,global_names,global_count,1);
  r.current=fixture;assert(lv_start(&r,fixture,"start",1,0,NULL));
  while(r.depth&&!r.failed)lv_run(&r,1000);
  if(r.failed)fprintf(stderr,"%s\n",r.error);
  assert(!r.failed);
  lv_t profile=r.globals[lv_global_id(&r,"profile")];
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"name",profile)),"REX"));
  assert(!strcmp(lv_cstr(&r,r.globals[lv_global_id(&r,"fontseen")]),"Face"));
  assert(!strcmp(lv_cstr(&r,r.globals[lv_global_id(&r,"afterwrite")]),"ANNA"));
}
'''
    run_lingo(tmp_path, source, body, version=version, extended=True)
