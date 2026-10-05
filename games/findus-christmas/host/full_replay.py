"""Generate the controller replay after the complete sanitized native journey."""

import hashlib
import json

from director64.aot import cstring
from director64.project import selected_game


def main():
    game = selected_game()
    root = game.work / "native/journey"
    report = json.loads((root / "validation.json").read_text())
    if (
        report.get("status") != "passing"
        or not report.get("sanitizers")
        or report["source_sha256"] != game.source["sha256"]
        or "all-24-start-and-return" not in report["scenarios"]
    ):
        raise ValueError("probe requires the passing sanitized 24-activity journey")
    for name, digest in report["evidence"].items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
            raise ValueError("native journey evidence changed")
    identity = hashlib.sha256((root / "validation.json").read_bytes()).hexdigest()
    steps = []

    def step(ticks, x=320, y=240, buttons="0", movie="", frame=0, objects=-1):
        steps.append(f"{{{cstring(movie)},{frame},{ticks},{x},{y},{buttons},{objects}}},")

    def click(x, y, ticks=120):
        step(60, x, y)
        step(10, x, y, "INPUT_A")
        step(ticks, x, y)

    step(60, movie="KALENDER.DXR", frame=5)
    click(607, 379, 180)
    click(607, 379)
    step(120, movie="DAG16.DXR", frame=4)
    step(60, 560, 200)
    step(60, 560, 200, "INPUT_A")
    step(60, 320, 240, "INPUT_A")
    step(120, 320, 240)
    step(60, objects=1)
    click(400, 450, 240)
    step(60, objects=0)
    click(220, 450)
    step(120, movie="KALENDER.DXR", frame=5)
    click(559, 32, 180)
    click(559, 32)
    step(120, movie="DAG24.DXR", frame=2)
    click(400, 450)
    step(60, movie="DAG24.DXR", frame=29)
    (game.work / "director/c/replay.inc").write_text(
        f"static const char replay_source_sha256[]={cstring(identity)};\n"
        "static const replay_step_t replay_steps[]={\n" + "\n".join(steps) + "\n};\n"
    )
