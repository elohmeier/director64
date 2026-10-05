"""Pointer regressions for the hammer game, radio and animation speed control."""

import hashlib
import json
import math
import re

from director64.project import selected_game

from .full_boot import validate_intro_stops
from .full_journey import DOOR, RIGHT, ROUTES, Replay

SCENARIOS = ("HAMMER", "RADIO", "FILMSPEED")


def sprite(replay, channel):
    return next(s for s in replay.state["sprites"] if s["id"] == channel)


def member(movie, name):
    value = next(m for m in movie["members"] if m["name"].casefold() == name.casefold())
    return movie["id"] << 20 | value["cast"] << 16 | value["number"]


def mark(replay, **checks):
    # Additional N64 checkpoints accompany the recorded input, never alter it.
    replay.state["checks"] = checks


def validate_reward_stop(log, checkpoint):
    video, duration = checkpoint["video_member"], checkpoint["video_duration"]
    if f"VIDEO_STOP member={video} time={duration} duration={duration}" not in log:
        raise ValueError("hammer reward video did not reach its full duration")


def menu(replay, movies, name):
    replay.step(4000)
    if replay.state["movie"] != "PANO.DXR":
        raise ValueError("launcher did not reach panorama")
    path, point = ROUTES[name]
    for position in path:
        replay.click(*position)
    replay.click(*point)
    target = name + ".DXR"
    frame = next(
        label["frame"] for label in movies[target]["score"]["labels"] if label["name"] == "Menue"
    )
    for _ in range(300):
        if replay.state["movie"] == target and frame <= replay.state["frame"] < frame + 4:
            return
        replay.step(60, *point)
    raise ValueError(f"{name}: source menu not reached")


def drag(replay, start, end):
    replay.step(30, *start)
    replay.step(120, *start, 1)
    replay.step(120, *end, 1)
    replay.step(120, *end)


def hammer(replay, movies):
    menu(replay, movies, "HAM")
    replay.sprite(11)
    movie = movies["HAM.DXR"]
    remaining = re.findall(r"#[^,\]]+", replay.state["objects"]["gspiel"]["mbilderliste"])
    if len(remaining) != 5 or not all(name.endswith(".pct") for name in remaining):
        raise ValueError("hammer picture-name symbols did not initialize")
    picture = int(replay.state["objects"]["gspiel"]["maktbild"])
    picture_name = next(m["name"] for m in movie["members"] if m["number"] == picture)
    video = member(movie, "HAM" + picture_name[9] + ".mov")
    nail = member(movie, "Nagel")
    drag(replay, (102, 365), (250, 160))
    if sprite(replay, 10)["member"] != nail + 2 or sprite(replay, 10)["loc"] != [250, 160]:
        raise ValueError("nail drag did not reach its authored position")
    mark(replay, sprite=[10, nail + 2])
    replay.click(395, 380)
    if sprite(replay, 10)["member"] != nail + 3:
        raise ValueError("hammer did not drive in the nail")
    mark(replay, sprite=[10, nail + 3])
    drag(replay, (240, 380), (250, 200))
    if sprite(replay, 25)["member"] != video or sprite(replay, 25)["video_rate"] != 1:
        raise ValueError("hanging the picture did not leave its reward video playing")
    mark(replay, sprite=[25, video])
    duration = next(m["frames"] for m in movie["members"] if m["number"] == (video & 0xFFFF))
    replay.step(math.ceil(duration / 10) + 120)
    dummy = member(movies["CURSOR.CXT"], "Dummy")
    if sprite(replay, 25)["member"] != dummy:
        raise ValueError("hammer reward video did not return to the game")
    mark(replay, sprite=[25, dummy])
    wallpaper = sprite(replay, 8)["member"]
    replay.click(530, 380)
    if sprite(replay, 8)["member"] != wallpaper + 1 or sprite(replay, 10)["member"] != nail:
        raise ValueError("wallpaper reset did not rebuild the hammer game")
    mark(replay, sprite=[8, wallpaper + 1])
    replay.sprite(46)
    return {"video_member": video, "video_duration": duration, "wallpaper_reset": True}


def radio(replay, movies):
    replay.step(4000)
    for position in (DOOR, RIGHT, RIGHT, RIGHT):
        replay.click(*position)
    replay.click(184, 270)
    sound = member(movies["CURSOR.CXT"], "HOELI.AIF")
    if replay.state["channel_sounds"][2] != sound or not replay.state["channel_busy"][2]:
        raise ValueError("radio did not resolve and start its external audio")
    mark(replay, sound=[3, sound])
    replay.step(300)
    # Hold long enough for the source's polling handler, then leave the kitchen.
    replay.step(30, *RIGHT)
    replay.step(120, *RIGHT, 1)
    replay.step(240, *RIGHT)
    if (
        replay.state["frame"] != 12
        or replay.state["channel_sounds"][2]
        or replay.state["channel_busy"][2]
    ):
        raise ValueError("leaving the kitchen did not stop the radio")
    mark(replay, sound=[3, 0])
    return {"resolved_sound": sound, "stopped_on_navigation": True}


def film_speed(replay, movies):
    menu(replay, movies, "FIL")
    replay.sprite(12)
    samples = []
    for x, speed in ((278, 1), (380, 11), (503, 24)):
        drag(replay, tuple(sprite(replay, 10)["loc"]), (x, 372))
        if (
            int(replay.state["objects"]["gspiel"]["mspeed"]) != speed
            or replay.state["tempo"] != speed
        ):
            raise ValueError("film slider did not control the active score tempo")
        mark(replay, tempo=speed)
        changes = []
        for _ in range(2):
            before = sprite(replay, 13)["member"]
            replay.step(60)
            changes.append((sprite(replay, 13)["member"] - before) % 50)
            mark(replay, tempo=speed)
        # The source retains the last picture during its phase-49 wrap before
        # setting phase 1, so one interval can differ by one visible phase.
        if any(abs(change - speed) > 1 for change in changes) or not sum(changes):
            raise ValueError(f"film animation speed does not follow the slider: {changes}")
        samples.append({"speed": speed, "phase_advances_per_60_ticks": changes})
    replay.sprite(46)
    return {"animation_samples": samples}


def run(executable, output):
    game = selected_game()
    model = json.loads((game.work / "director/model.json").read_text())
    movies = {m["name"]: m for m in model["movies"]}
    checkpoints, hashes = {}, {}
    for name, scenario in zip(SCENARIOS, (hammer, radio, film_speed), strict=True):
        replay = Replay(executable, output / name)
        try:
            checkpoints[name] = scenario(replay, movies)
            replay.step(300)
            if replay.state["movie"] != "PANO.DXR":
                raise ValueError(f"{name}: did not return to panorama")
        finally:
            replay.close()
        log = (output / name / "native.log").read_text()
        validate_intro_stops(log, model)
        if name == "HAMMER":
            validate_reward_stop(log, checkpoints[name])
        hashes[name] = hashlib.sha256((output / name / "replay.json").read_bytes()).hexdigest()
    return {
        "status": "passing",
        "platform": "host-native",
        "scenario": "hammer-radio-film-speed",
        "checkpoints": checkpoints,
        "replay_sha256": hashes,
        "source_sha256": game.source["sha256"],
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "hardware_qualified": False,
        "original_projector_compared": False,
    }
