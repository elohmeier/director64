#!/usr/bin/env python3
"""Run a synthetic Lingo probe in an already-open, isolated Wine projector."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("container")
parser.add_argument("script", type=Path)
parser.add_argument("--engine", choices=("docker", "podman"), default="docker")
parser.add_argument("--output", type=Path, default=Path("build/reference/ink-probes"))
args = parser.parse_args()


def execute(*command, data=None):
    return subprocess.run(
        [args.engine, "exec", "-i", args.container, *command], input=data,
        capture_output=True, check=True,
    ).stdout


source = args.script.read_text()
match = re.search(r'out\.createFile\("(C:\\[a-z-]+\.txt)"\)', source)
if not match:
    raise SystemExit("probe must write a named C:\\<probe>.txt through out.createFile")
result_path = execute("winepath", "-u", match[1]).decode().strip()
marker = "DIRECTOR64_PROBE_COMPLETE"
close = source.rfind("out.closeFile()")
if close < 0:
    raise SystemExit("probe must close its output file")
source = source[:close] + f'out.writeString("{marker}" & RETURN)\n' + source[close:]
# Director's do() parser requires CR line endings, even on a Unix host.
execute("python3", "-c", "import pathlib,sys;pathlib.Path(sys.argv[1]).write_bytes(sys.stdin.buffer.read())",
        "/tmp/director64-ink-probe.ls", data=source.replace("\n", "\r").encode("ascii"))
execute("python3", "-c", "import pathlib,sys;pathlib.Path(sys.argv[1]).unlink(missing_ok=True)", result_path)
window = execute("xdotool", "search", "--name", "^Message$").decode().splitlines()
if len(window) != 1:
    raise SystemExit("expected exactly one original-projector Message window")
for command in (
    "global d64ProbeInput,d64ProbeCode",
    'd64ProbeInput=new(xtra("fileio"))',
    'd64ProbeInput.openFile("Z:\\tmp\\director64-ink-probe.ls",1)',
    "d64ProbeCode=d64ProbeInput.readFile()",
    "d64ProbeInput.closeFile()",
    "do(d64ProbeCode)",
):
    execute("xdotool", "windowfocus", window[0], "key", "ctrl+End")
    execute("xdotool", "type", "--clearmodifiers", "--delay", "1", "--file", "-", data=command.encode())
    execute("xdotool", "key", "KP_Enter")
    time.sleep(0.15)

for _ in range(60):
    result = execute("python3", "-c",
                     "import pathlib,sys;p=pathlib.Path(sys.argv[1]);sys.stdout.buffer.write(p.read_bytes() if p.exists() else b'')",
                     result_path).decode("ascii")
    if marker in result:
        break
    time.sleep(0.5)
else:
    raise SystemExit("projector did not finish the probe; inspect its Message window")

# Record what Wine actually loaded, including thin-projector runtime DLLs.
provenance = execute("python3", "-c", r'''
import hashlib,json,re,subprocess
from pathlib import Path
modules={}
for process in Path('/proc').glob('[0-9]*'):
    try: maps=(process/'maps').read_text()
    except (FileNotFoundError,PermissionError,ProcessLookupError): continue
    if not any(name in maps.lower() for name in ('dirapi.dll','iml32.dll')): continue
    for row in maps.splitlines():
        fields=row.split(None,5)
        if len(fields)!=6: continue
        path=Path(fields[5])
        if path.suffix.lower() not in ('.dll','.exe'): continue
        if path.name.lower() not in ('dirapi.dll','iml32.dll','proj.dll') and path.suffix.lower()!='.exe': continue
        if str(path) in modules: continue
        data=path.read_bytes()
        strings=[s.decode('utf-16le') for s in re.findall(rb'(?:[\x20-\x7e]\x00){5,}',data)]
        versions=[strings[i+1] for i,s in enumerate(strings[:-1]) if s=='FileVersion']
        modules[str(path)]={'sha256':hashlib.sha256(data).hexdigest(),'file_versions':sorted(set(versions))}
print(json.dumps({'wine':subprocess.check_output(['wine','--version'],text=True).strip(),'loaded_modules':modules},indent=2))
''')
args.output.mkdir(parents=True, exist_ok=True)
output = args.output / (args.script.stem + ".txt")
output.write_text(result.replace(marker + "\r", "").replace("\r", "\n"))
metadata = json.loads(provenance)
metadata["probe"] = args.script.name
metadata["observation_count"] = len(output.read_text().splitlines())
output.with_suffix(".json").write_text(json.dumps(metadata, indent=2) + "\n")
print(f"Recorded {metadata['observation_count']} observations in {output}")
