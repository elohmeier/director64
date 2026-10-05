"""Source-free compiler and native continuation contracts."""

import json
import shutil
import subprocess

import pytest

from director64.aot import generate
from director64.lingo import LingoError, expression, parse_movie


def test_literals_and_precedence():
    assert expression('"C:\\data\\"') == ["string", "C:\\data\\"]
    assert expression("1 + 2 * 3")[3][1] == "*"
    assert expression('member 3 of castLib "External"')[3] == ["string", "External"]
    assert expression('member (3 + i) of castLib "External"') == expression(
        'member(3 + i, "External")'
    )
    assert expression("the castNum of sprite 3")[0] == "property"
    assert expression("char 3 to 4 of text")[0] == "chunk"
    assert expression("cast + 1")[2] == ["variable", "cast"]
    with pytest.raises(LingoError):
        expression("a ? b")


def test_reject_partial_handler():
    with pytest.raises(LingoError, match="truncated"):
        parse_movie(
            "-- cast: Internal; member: 1; type: MovieScript; name: test\non test\n", "fixture"
        )


def test_picture_name_symbols_keep_their_extension():
    # D5 HAMMERSPIEL uses these symbols as member names, including the period.
    assert expression("[#picture1.pct, #picture2.pct]") == [
        "list",
        [["symbol", "picture1.pct"], ["symbol", "picture2.pct"]],
    ]
    assert expression("string(#Picture.pct)") == ["call", "string", [["symbol", "Picture.pct"]]]
    assert expression("picture.name") == ["get", "name", ["variable", "picture"]]


def test_native_control_flow(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("native C compiler unavailable")
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
    result = generate({"handlers": parse_movie(source, "FIXTURE.DXR")}, tmp_path)
    assert result["movies"][0]["handlers"] == 4
    (tmp_path / "harness.c").write_text("""
#include "lingo_runtime.h"
#include <assert.h>
#include <string.h>
extern const lv_movie_t aot_fixture_dxr;
static lv_runtime_t runtime;
static unsigned yields;
static const char *const names[]={
#include "globals.inc"
};
static bool call(lv_runtime_t *r,const char *name,unsigned n,const lv_t *a,lv_t *v,bool *y) {
    (void)r; (void)n; (void)a; (void)v;
    if (strcmp(name,"updatestage")) return false;
    *y=true; yields++; return true;
}
int main(void) {
    lv_init(&runtime,(lv_services_t){.call=call},0,names,sizeof(names)/sizeof(*names),1);
    assert(lv_start(&runtime,&aot_fixture_dxr,"start",0,0,0));
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
    executable = tmp_path / "test"
    subprocess.run(
        [
            cc,
            "-std=c17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-Iruntime/director",
            "-Iruntime/lingo",
            "-Iruntime/interaction",
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
    assert (
        json.loads((tmp_path / "manifest.json").read_text())["native_execution_verified"] is False
    )


def lowered_loop(tmp_path, condition):
    source = f"""-- cast: Internal; member: 1; type: MovieScript; name: fixture
on start
  repeat while {condition}
    updateStage()
  end repeat
end
"""
    generate({"handlers": parse_movie(source, "FIXTURE.DXR")}, tmp_path)
    return (tmp_path / "fixture_dxr.lst").read_text()


@pytest.mark.parametrize(
    "condition",
    [
        "the movieRate of sprite 2",
        "the movieTime of sprite 2 < 100",
        "the stillDown",
        "(the movieRate of sprite kanal) and not (the mouseDown)",
    ],
)
def test_media_and_drag_polls_yield_the_tick(tmp_path, condition):
    # A linked movie decodes and the pad is sampled once per service tick, so
    # these loops observe one value however fast they spin. Löwenzahn's intro
    # spun its QT-Loop through the whole step budget every tick.
    assert "YIELD" in lowered_loop(tmp_path, condition)


@pytest.mark.parametrize("condition", ["counter < 3", "the castNum of sprite 2 = 4"])
def test_script_driven_loop_keeps_running(tmp_path, condition):
    # Only the body can end these, so yielding would stretch an authored
    # animation over one tick per pass.
    assert "YIELD" not in lowered_loop(tmp_path, condition)


@pytest.mark.parametrize("condition", ["the timer < 3", "the mouseDown = 0", "soundBusy(1)"])
def test_stage_updates_in_polling_loop(tmp_path, condition):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("native C compiler unavailable")
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
    generate({"handlers": parse_movie(source, "FIXTURE.DXR")}, tmp_path)
    (tmp_path / "harness.c").write_text("""
#include "director.h"
#include <assert.h>
extern const lv_movie_t aot_fixture_dxr;
static lv_runtime_t values;
static dg_runtime_t director;
static const char *const names[]={
#include "globals.inc"
};
static bool busy(void *context, unsigned channel) {
    (void)context; (void)channel;
    return director.ticks < 3;
}
int main(void) {
    const dg_frame_t frame={0};
    const dg_movie_t movie={.code=&aot_fixture_dxr,.id=1,.tempo=1,
                           .frame_count=1,.frames=&frame};
    dg_init(&director,&values,(dg_platform_t){.sound_busy=busy},NULL,
            names,sizeof(names)/sizeof(*names),1);
    assert(dg_enter(&director,&movie,1,NULL,0));
    assert(lv_start(&values,&aot_fixture_dxr,"start",0,0,NULL));
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
""")
    executable = tmp_path / "test"
    subprocess.run(
        [
            cc,
            "-std=c17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-Iruntime/director",
            "-Iruntime/lingo",
            "-Iruntime/interaction",
            str(tmp_path / "harness.c"),
            str(tmp_path / "fixture_dxr.c"),
            str(tmp_path / "symbols.c"),
            "runtime/director/director.c",
            "runtime/lingo/lingo_runtime.c",
            "-lm",
            "-o",
            str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
