"""Export this disc's print documents and precompute their N64 QR matrices.

Run with: uv run --locked python games/loewenzahn-1/host/printing.py
Host requirements: Typst with Liberation Sans, and libqrencode (QR generation).
Recovered text/artwork, PDFs and generated C stay in the ignored build tree.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import struct
import subprocess
import tomllib
import zlib
from pathlib import Path
from urllib.parse import urlparse

from director64.project import GameSpec

SOURCE = "f31970b980f096a343a9aec5f10d52931709fbdda3cf09e5a80023791e402af2"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def config(game):
    if game.source["sha256"] != SOURCE:
        raise ValueError("printing source edition changed")
    result = tomllib.loads((game.directory / "host/printing.toml").read_text())
    local = game.directory / "host/printing.local.toml"
    if local.is_file():
        result |= tomllib.loads(local.read_text())
    url = urlparse(result["base_url"])
    if url.scheme not in {"http", "https"} or not url.netloc or url.query or url.fragment:
        raise ValueError("printing base URL must be an absolute HTTP(S) directory")
    if not result["base_url"].endswith("/"):
        raise ValueError("printing base URL must end in /")
    return result


def documents(game, model):
    result = []
    for book in ("BAS", "REZ"):
        movie = next(m for m in model["movies"] if m["name"] == book + ".DXR")
        if movie["directorVersion"] != 500:
            raise ValueError("expected Director 5 print fields")
        members = {m["name"].lower(): m for m in movie["members"]}
        for chapter in range(1, 9):
            title, body = (members[f"{key}{chapter}"] for key in ("titel", "text"))
            for field in (title, body):
                if field["text"] != bytes.fromhex(field["textHex"]).decode("mac_roman"):
                    raise ValueError("print field decoding differs from original Mac Roman")
            result.append(
                {
                    "book": book,
                    "chapter": chapter,
                    "title": title["text"],
                    "body": body["text"],
                    "filename": f"{book.lower()}-{chapter}.pdf",
                    "logo": f"{book}_LOGO.png",
                    "logo_width": 122 if book == "BAS" else 80,
                    "illustration": f"BAS{chapter}.png"
                    if book == "BAS" and chapter in (2, 3, 6, 7)
                    else None,
                    "illustration_x": 6 if chapter == 6 else 16,
                }
            )
    return result


def decode_pict(data):
    """Strictly decode the observed single-raster 1-bit 200-dpi PICT subset.

    Layout reference: ScummVM image/pict.cpp at 41ac2b31847622d0662d22c03fe6979e3b43cfbc.
    Reject different operations/formats instead of guessing or skipping bytes.
    """

    def require(test):
        if not test:
            raise ValueError("unsupported or truncated print PICT")

    require(len(data) >= 656)
    require(data[522:528] == bytes.fromhex("001102ff0c00"))
    require(data[552:568] == bytes.fromhex("001e0001000a00000000") + data[544:548] + b"\x00\x98")
    q = 568
    stride = int.from_bytes(data[q : q + 2], "big") & 0x7FFF
    top, left, height, width = struct.unpack_from(">hhhh", data, q + 2)
    require((top, left) == (0, 0) and 0 < width <= 1254 and 0 < height <= 990)
    require((width + 7) // 8 <= stride <= (width + 15) // 8)
    require(struct.unpack_from(">H", data, q + 12)[0] == 0)
    require(struct.unpack_from(">II", data, q + 18) == (200 << 16, 200 << 16))
    require(struct.unpack_from(">HHHH", data, q + 26) == (0, 1, 1, 1))
    p = q + 46
    require(struct.unpack_from(">H", data, p + 6)[0] == 1)
    p += 8
    require(
        [struct.unpack_from(">HHHH", data, p + i * 8) for i in range(2)]
        == [(0, 65535, 65535, 65535), (0, 0, 0, 0)]
    )
    p += 16
    require(struct.unpack_from(">hhhh", data, p) == (0, 0, height, width))
    require(struct.unpack_from(">hhhh", data, p + 8) == (0, 0, height, width))
    require(struct.unpack_from(">H", data, p + 16)[0] == 0)
    p += 18
    rows = []
    for _ in range(height):
        require(p < len(data))
        length = data[p]
        p += 1
        packed = data[p : p + length]
        require(len(packed) == length)
        p += length
        i, row = 0, bytearray()
        while i < length:
            code = packed[i]
            i += 1
            if code <= 127:
                require(i + code + 1 <= length)
                row.extend(packed[i : i + code + 1])
                i += code + 1
            elif code > 128:
                require(i < length)
                row.extend(bytes([packed[i]]) * (257 - code))
                i += 1
            require(len(row) <= stride)
        require(len(row) == stride)
        # PNG grayscale 1 is white; this PICT palette index 1 is black.
        rows.append(b"\0" + bytes(v ^ 255 for v in row[: (width + 7) // 8]))
    p += p % 2
    require(data[p:] == b"\x00\xff")

    def chunk(kind, body):
        return (
            struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0))
    png += chunk(b"pHYs", struct.pack(">IIB", 7874, 7874, 1))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    return png, width, height


def qr_matrix(url):
    """Use the installed host libqrencode; only bit matrices enter the ROM."""

    class QRcode(ctypes.Structure):
        _fields_ = [
            ("version", ctypes.c_int),
            ("width", ctypes.c_int),
            ("data", ctypes.POINTER(ctypes.c_ubyte)),
        ]

    library = ctypes.util.find_library("qrencode")
    if not library:
        raise ValueError("install host libqrencode to generate print QR matrices")
    lib = ctypes.CDLL(library)
    lib.QRcode_encodeString8bit.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.QRcode_encodeString8bit.restype = ctypes.POINTER(QRcode)
    lib.QRcode_free.argtypes = [ctypes.POINTER(QRcode)]
    code = lib.QRcode_encodeString8bit(url.encode("ascii"), 0, 1)  # QR_ECLEVEL_M
    if not code:
        raise ValueError("QR encoding failed")
    try:
        size = code.contents.width
        if not 21 <= size <= 49:
            raise ValueError("print URL exceeds the TV QR module budget")
        bits = [code.contents.data[i] & 1 for i in range(size * size)]
        packed = bytearray((len(bits) + 7) // 8)
        for i, bit in enumerate(bits):
            packed[i // 8] |= bit << (7 - i % 8)
        return size, bytes(packed)
    finally:
        lib.QRcode_free(code)


def prepare_qr(game, model):
    settings = config(game)
    records = documents(game, model)
    lines = [
        "/* Generated from the selected disc and host/printing.toml. */",
        "static const print_document_t print_documents[] = {",
    ]
    for doc in records:
        doc["url"] = settings["base_url"] + doc["filename"]
        size, bits = qr_matrix(doc["url"])
        doc["qr_modules"] = size
        doc["qr_bits"] = bits.hex()
        strings = ",".join(json.dumps(doc[k], ensure_ascii=True) for k in ("title", "url"))
        lines.append("{" + strings + f",{size},{{" + ",".join(map(str, bits)) + "}},")
    lines.append("};")
    output = game.work / "director/c/print_documents.inc"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n")
    return records


def export(game):
    model = json.loads((game.work / "director/model.json").read_text())
    records = prepare_qr(game, model)
    output = game.work / "printing"
    output.mkdir(parents=True, exist_ok=True)
    images = []
    for path in sorted((game.work / "extracted/MEDIA/PRINT").glob("*.PCT")):
        png, width, height = decode_pict(path.read_bytes())
        (output / (path.stem + ".png")).write_bytes(png)
        images.append({"source": path.name, "sha256": sha(path), "width": width, "height": height})
    if len(images) != 6:
        raise ValueError("print PICT inventory changed")
    template = game.directory / "host/print.typ"
    (output / "print.typ").write_bytes(template.read_bytes())
    for doc in records:
        path = output / doc["filename"]
        description = path.with_suffix(".json")
        description.write_text(json.dumps(doc, ensure_ascii=False) + "\n")
        subprocess.run(
            [
                "typst",
                "compile",
                "--input",
                f"document={description.name}",
                str(output / "print.typ"),
                str(path),
            ],
            check=True,
        )
        doc["pdf_sha256"] = sha(path)
    receipt = {
        "source_sha256": SOURCE,
        "template_sha256": sha(template),
        "generator_sha256": sha(Path(__file__)),
        "images": images,
        "documents": records,
        "typst": subprocess.check_output(["typst", "--version"], text=True).strip(),
        "layout": "A4, Liberation Sans, source illustration frame size, flow pagination",
    }
    (output / "manifest.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qr-only", action="store_true")
    args = parser.parse_args()
    game = GameSpec.load("loewenzahn-1")
    if args.qr_only:
        prepare_qr(game, json.loads((game.work / "director/model.json").read_text()))
    else:
        receipt = export(game)
        print(f"Generated {len(receipt['documents'])} PDFs in {game.work / 'printing'}")


if __name__ == "__main__":
    main()
