#!/usr/bin/env python3
"""Generate source-free Flash timeline-mask probes; output stays in build/.

The embedded glyph is a square drawn here, not a recovered font. Sparse pixel,
nonempty-bounds, and target hitTest observations distinguish geometric masks
from ordinary display opacity, strokes, bitmap alpha, and device fonts.
"""
import argparse
from pathlib import Path
import struct
import zlib
from generate_filter_probe import Bits, rect, tag
from generate_flash_geometry_probe import push, variable, call


def matrix():
    return Bits().put(0, 1).put(0, 1).put(1, 5).put(0, 1).put(0, 1).bytes()


def shape(number, alpha, stroke=False, whole=False, bitmap=False):
    start, size = (0, 640) if whole else (160, 320)
    records = Bits().put(0 if stroke else 1, 4).put(1 if stroke else 0, 4)
    records.put(0, 1).put(9 if stroke else 5, 5).put(11, 5)
    records.put(start, 11).put(start, 11).put(1, 1)
    for dx, dy in ((size, 0), (0, size), (-size, 0), (0, -size)):
        records.put(1, 1).put(1, 1).put(9, 4).put(1, 1).put(dx, 11).put(dy, 11)
    records.put(0, 6)
    if stroke:
        styles = bytes((0, 1)) + struct.pack('<H', 80) + bytes((255, 0, 0, alpha))
    elif bitmap:
        styles = bytes((1, 0x40)) + struct.pack('<H', 5) + matrix() + bytes((0,))
    else:
        styles = bytes((1, 0, 0, 255, 0, alpha, 0))
    return tag(32, struct.pack('<H', number) + rect(0, 0, 640, 640) + styles + records.bytes())


def placement(number, depth, clip=0, alpha=256, name=''):
    flags = 6 | (64 if clip else 0) | (8 if alpha != 256 else 0) | (32 if name else 0)
    result = bytes((flags,)) + struct.pack('<HH', depth, number) + matrix()
    if alpha != 256:
        bits = Bits().put(0, 1).put(1, 1).put(10, 4)
        for value in (256, 256, 256, alpha):
            bits.put(value, 10)
        result += bits.bytes()
    if name:
        result += name.encode('ascii') + b'\0'
    if clip:
        result += struct.pack('<H', clip)
    return tag(26, result)


def sprite(number, content):
    return tag(39, struct.pack('<HH', number, 1) + content + tag(1) + tag(0))


def assign(receiver, key, value):
    return variable(receiver) + push(key) + push(value) + b'\x4f'


def invoke(receiver, method, arguments):
    # Arguments here are already bytecode expressions, in reverse stack order.
    return b''.join(arguments) + push(len(arguments)) + variable(receiver) + push(method) + b'\x52'


def capture(hits=True):
    code = push('bmp') + push(0) + b'\x96\x02\x00\x05\x01' + push(32) + push(32) + push(4)
    code += variable('flash') + push('display') + b'\x4e' + push('BitmapData') + b'\x53\x1d'
    code += invoke('bmp', 'draw', [variable('_root')]) + b'\x17'
    for x, y in ((4, 4), (8, 8), (16, 16), (23, 16), (24, 16), (16, 24)):
        code += push(f'PIX {x} {y} ') + invoke('bmp', 'getPixel32', [push(y), push(x)]) + b'\x47\x26'
    code += push('BOUNDS ') + invoke('bmp', 'getColorBoundsRect',
                                    [push(0), push(0), push(-16777216)]) + b'\x47\x26'
    if hits:
        for x, y in ((4, 4), (8, 8), (16, 16), (24, 16)):
            code += push(f'HIT target {x} {y} ') + invoke('target', 'hitTest',
                         [push(1), push(y), push(x)]) + b'\x47\x26'
    return code + push('MASK_DONE') + b'\x26\x07\0'


def embedded_text(alpha):
    glyph = Bits().put(1, 4).put(0, 4)
    glyph.put(0, 1).put(5, 5).put(12, 5).put(0, 12).put(-1024, 12).put(1, 1)
    for dx, dy in ((1024, 0), (0, 1024), (-1024, 0), (0, -1024)):
        glyph.put(1, 1).put(1, 1).put(10, 4).put(1, 1).put(dx, 12).put(dy, 12)
    glyph.put(0, 6)
    glyph_bytes = glyph.bytes()
    font = struct.pack('<HBBB', 5, 0, 0, 5) + b'Probe'
    font += struct.pack('<HHH', 1, 4, 4 + len(glyph_bytes)) + glyph_bytes + b'A'
    record = bytes((0x8f,)) + struct.pack('<H', 5) + bytes((255, 0, 0, alpha))
    record += struct.pack('<hhH', 160, 480, 320) + bytes((1,))
    record += Bits().put(0, 1).put(320, 10).bytes() + bytes((0,))
    text = struct.pack('<H', 6) + rect(0, 0, 640, 640) + matrix() + bytes((1, 10)) + record
    return tag(48, font) + tag(33, text)


def movie(kind, alpha=255, transform=256):
    bitmap = kind == 'bitmap-transparent'
    definitions = shape(1, alpha, stroke=kind == 'stroke-only', bitmap=bitmap)
    definitions += shape(3, 255, whole=True)
    content = placement(1, 1)
    if bitmap:
        definitions += tag(36, struct.pack('<HBHH', 5, 5, 1, 1) + zlib.compress(bytes(4)))
    if kind.startswith('child-'):
        definitions += sprite(7, placement(1, 1))
        content = placement(7, 1, name='child')
    if kind.startswith('device-text'):
        content = b''
    if kind == 'embedded-text':
        definitions += embedded_text(alpha)
        content = placement(6, 1)
    definitions += sprite(2, content) + sprite(4, placement(3, 1))
    actions = b''
    if kind == 'invisible':
        actions += assign('mask', '_visible', 0)
    if kind == 'child-invisible':
        actions += assign('mask.child', '_visible', 0)
    if kind == 'child-alpha0':
        actions += assign('mask.child', '_alpha', 0)
    if kind.startswith('device-text'):
        actions += call('mask', 'createTextField', ('txt', 1, 8, 8, 16, 16))
        actions += assign('mask.txt', 'text', 'A')
    if kind == 'device-text-background':
        actions += assign('mask.txt', 'background', 1)
    control = kind == 'device-text-control'
    actions += capture(hits=not control)
    body = rect(0, 0, 640, 640) + bytes((0, 12)) + struct.pack('<H', 1) + definitions
    body += placement(2, 1, clip=0 if control else 2, alpha=transform, name='mask')
    if not control:
        body += placement(4, 2, name='target')
    body += tag(12, actions) + tag(1) + tag(0)
    return b'FWS\x08' + struct.pack('<I', len(body) + 8) + body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for alpha in (0, 1, 128, 255):
        (args.output / f'fill-{alpha}.swf').write_bytes(movie('fill', alpha))
    for alpha in (0, 128):
        (args.output / f'clip-alpha-{alpha}.swf').write_bytes(movie('fill', transform=alpha))
    for alpha in (0, 255):
        (args.output / f'embedded-text-{alpha}.swf').write_bytes(movie('embedded-text', alpha))
    for kind in ('stroke-only', 'invisible', 'child-invisible', 'child-alpha0',
                 'bitmap-transparent', 'device-text', 'device-text-control', 'device-text-background'):
        (args.output / f'{kind}.swf').write_bytes(movie(kind))


if __name__ == '__main__':
    main()
