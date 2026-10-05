"""Attribute an emulator cache-miss profile to runtime functions and fields.

usage: attribute.py PROFILE NM_TEXT GOPHER64_LOG FIELDS_JSON [--top N]
"""
import bisect, json, re, sys

profile, nm_text, log, fields_json = sys.argv[1:5]
top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 30

symbols = []  # (addr, size, name) of code
statics = []  # (addr, size, name) of data, read-only data and bss
for line in open(nm_text):
    parts = line.split()
    if len(parts) == 4 and parts[2] in "tTwW":
        symbols.append((int(parts[0], 16) & 0x1FFFFFFF, int(parts[1], 16), parts[3]))
    elif len(parts) == 4 and parts[2] in "dDbBrRgGsSvV":
        statics.append((int(parts[0], 16) & 0x1FFFFFFF, int(parts[1], 16), parts[3]))
symbols.sort()
statics.sort()
starts = [s[0] for s in symbols]
static_starts = [s[0] for s in statics]

def symbol(pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and pc < symbols[i][0] + symbols[i][1]:
        return symbols[i][2]
    return "(overlay/other)"

def static(addr):
    """The static object at or before an address: the profile's buckets are
    256 bytes, so this names the first object of a bucket, not every one."""
    i = bisect.bisect_right(static_starts, addr) - 1
    if i >= 0:
        return f"{statics[i][2]}+{addr - statics[i][0]:#x}"
    return "static"

layout = {}
extra = []  # (name, base, size) regions the ROM reported: overlays, the image region
for line in open(log, errors="replace"):
    m = re.search(r"NATIVE_LAYOUT values=(0x[0-9a-f]+) values_size=(\d+) director=(0x[0-9a-f]+) director_size=(\d+)", line)
    if m and not layout:
        layout = {"values": (int(m[1], 16) & 0x1FFFFFFF, int(m[2])), "director": (int(m[3], 16) & 0x1FFFFFFF, int(m[4]))}
        # Since 2026-09-27 the ROM also reports the static sections and the
        # heap start; older captures have neither.
        for key in ("data", "bss", "heap"):
            s = re.search(rf" {key}=(0x[0-9a-f]+)", line)
            if s:
                layout[key] = int(s[1], 16) & 0x1FFFFFFF
    m = re.search(r"NATIVE_OVERLAY_BASE symbol=(\S+) at=(0x[0-9a-f]+) bytes=(-?\d+)", line)
    if m:
        at, size = int(m[2], 16) & 0x1FFFFFFF, max(int(m[3]), 0)
        extra.append((f"overlay {m[1]}", at - size, size + 256))
    m = re.search(r"NATIVE_IMAGE_REGION bytes=(\d+) .* at=(0x[0-9a-f]+)", line)
    if m:
        extra.append(("image region", int(m[2], 16) & 0x1FFFFFFF, int(m[1])))
fields = json.load(open(fields_json))["fields"]
def field(name, key, off):
    best = None
    for f, info in fields[key].items():
        if "offset" in info and info["offset"] <= off:
            if best is None or info["offset"] > fields[key][best]["offset"]:
                best = f
    return f"{name}.{best}"
def region(addr, exact=False):
    """The region an address belongs to; `exact` names the object within a
    region where one is known (a field, a static, a stack depth)."""
    for name, key in (("values", "lv_runtime_t"), ("director", "dg_runtime_t")):
        if name in layout:
            base, size = layout[name]
            if base <= addr < base + size:
                return field(name, key, addr - base)
    for name, base, size in extra:
        if base <= addr < base + size:
            return f"{name} {addr:#x}" if exact else name
    if addr >= 0x7F0000:
        return f"stack -{0x800000 - addr:#x}" if exact else "stack"
    if "data" in layout and "heap" in layout:
        if addr < layout["data"]:
            return static(addr) if exact else "read-only data"
        if addr < layout["bss"]:
            return static(addr) if exact else "static .data"
        if addr < layout["heap"]:
            return static(addr) if exact else "static .bss"
        return f"heap {addr:#x}" if exact else "heap"
    return static(addr) if exact else "other (heap/static)"

totals = {}
dmiss_pc, imiss_pc, dmiss_data = {}, {}, {}
dmiss_ra, dmiss_bucket = {}, {}
for line in open(profile):
    parts = line.split()
    if parts[0] == "T":
        totals = dict(p.split("=") for p in parts[1:])
    elif parts[0] == "P":
        pc, d, i = int(parts[1], 16) & 0x1FFFFFFF, int(parts[2]), int(parts[3])
        name = symbol(pc)
        dmiss_pc[name] = dmiss_pc.get(name, 0) + d
        imiss_pc[name] = imiss_pc.get(name, 0) + i
    elif parts[0] == "D":
        addr, d = int(parts[1], 16), int(parts[2])
        r = region(addr)
        dmiss_data[r] = dmiss_data.get(r, 0) + d
        dmiss_bucket[addr] = dmiss_bucket.get(addr, 0) + d
    elif parts[0] == "R":
        ra, d = int(parts[1], 16) & 0x1FFFFFFF, int(parts[2])
        name = symbol(ra)
        dmiss_ra[name] = dmiss_ra.get(name, 0) + d

dh, dm, ih, im = (int(totals.get(k, 0)) for k in ("dhits", "dmisses", "ihits", "imisses"))
print(f"data: {dm:,} misses / {dh + dm:,} accesses = {100 * dm / max(1, dh + dm):.1f}% miss rate")
print(f"code: {im:,} misses / {ih + im:,} fetches = {100 * im / max(1, ih + im):.2f}% miss rate")
print(f"layout: {layout}")
def show(title, table, total):
    print(f"\n{title}")
    for name, n in sorted(table.items(), key=lambda kv: -kv[1])[:top]:
        print(f"  {100 * n / max(1, total):5.1f}%  {n:>10,}  {name}")
show("data misses by function", dmiss_pc, dm)
if dmiss_ra:
    show("data misses by caller (return address)", dmiss_ra, dm)
show("code misses by function", imiss_pc, im)
show("data misses by region", dmiss_data, dm)

# The data cache is 8 KiB and direct-mapped: addresses 8 KiB apart share a
# line. This folds every bucket into that period, 32 slices of 256 bytes,
# and names the objects in the slices that miss most: two hot objects in
# one slice evict each other whatever their functions do.
slices = [0] * 32
for addr, d in dmiss_bucket.items():
    slices[(addr >> 8) & 31] += d
print("\ndata misses by cache-period slice (256 bytes; the stack's hottest lines are 19..24)")
print("  " + " ".join(f"{s}:{100 * slices[s] / max(1, dm):.1f}" for s in range(32)))
for s in sorted(range(32), key=lambda s: -slices[s])[:4]:
    print(f"  slice {s} ({100 * slices[s] / max(1, dm):.1f}%)")
    hot = [(a, d) for a, d in dmiss_bucket.items() if (a >> 8) & 31 == s]
    for addr, d in sorted(hot, key=lambda kv: -kv[1])[:8]:
        print(f"    {100 * d / max(1, dm):5.2f}%  {d:>10,}  {addr:#010x}  {region(addr, exact=True)}")
