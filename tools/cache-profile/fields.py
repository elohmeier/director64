"""Turn `nm -S` of a compiled fields.c into the JSON attribute.py reads."""

import json
import sys

fields, sizes = {}, {}
for line in sys.stdin:
    parts = line.split()
    if len(parts) != 4:
        continue
    size, name = int(parts[1], 16), parts[3]
    if name.startswith("size__"):
        sizes[name[6:]] = size
    elif name.startswith(("off_", "len_")):
        struct, field = name[4:].split("__")
        key = "offset" if name.startswith("off_") else "size"
        fields.setdefault(struct, {}).setdefault(field, {})[key] = size - 1
json.dump({"sizes": sizes, "fields": fields}, sys.stdout, indent=1)
