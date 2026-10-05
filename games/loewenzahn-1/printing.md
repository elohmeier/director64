# Löwenzahn 1 phone printing

The original print stamps now open a QR code for the selected craft instruction
or recipe. Scan it with a phone to open the PDF, then use the phone's print or
share menu. B closes the overlay and returns to the same chapter. The game
clock and audio pause while the code is open; there is no timeout.

The PDFs are generated from your own disc and served by any static web server
you control; the phone must be able to reach it, the console needs no network
connection. The PDFs reproduce the game's text and illustrations, so host them
for your own use, not publicly.

The tracked `host/printing.toml` holds a placeholder address
(`https://print.invalid/lz1-f319/`). Put your server's directory in the ignored
`host/printing.local.toml`:

```toml
base_url = "https://print.example.net/lz1-f319/"
```

The address is compiled into the ROM: correcting a PDF at the same address does
not require a new ROM, changing the address requires redirects or a rebuild.
Adding, changing or removing the local file invalidates the generated assets'
receipt. The edition token `lz1-f319` distinguishes this disc.

## Documents

| Chapter | Craft book (BAS) | Separate illustration | Recipe book (REZ) |
| --- | --- | --- | --- |
| 1 | Das Badewannenschiff | — | Brötchen backen |
| 2 | Drehende Scheiben | BAS2.PCT | Bauernfrühstück |
| 3 | Das Holzrennauto | BAS3.PCT | Karamellpudding |
| 4 | Kartoffeldruck | — | Kartoffelsuppe |
| 5 | Der Nageligel | — | Löwenzahnhonig |
| 6 | Der Salzwasserdimmer | BAS6.PCT | Löwenzahnkaffee |
| 7 | Das Solei | BAS7.PCT | Pommes frites |
| 8 | Der Unkrautgarten | — | Wildkräutersalat |

Filenames are `bas-1.pdf` through `bas-8.pdf` and `rez-1.pdf` through `rez-8.pdf`.
Each document is one A4 page, with complete original text, its original logo,
the applicable illustration and the Bavaria Film Multimedia copyright footer.
The export uses Liberation Sans at 12 pt, with an 18 pt bold title. Layout is
an adaptation; it has not been compared with an original-projector printout.

The four illustration rasters are 1254 × 990 pixels at 200 dpi and are placed
at the source PICT frame size of 451 × 356 pt. The craft logo uses a 122 × 78 pt
frame; the recipe logo uses 80 × 92 pt. Use actual-size/100% printing to preserve
template dimensions. All six source PICTs total 91,834 bytes. Source text is
Mac Roman, including this Windows disc's recovered cast fields.

## Generate, publish and build

Host export requirements: Typst, Liberation Sans and libqrencode. PDF review
also uses Poppler (`pdfinfo`, `pdftotext`, `pdfimages`, `pdftoppm`). No QR encoder
or PDF renderer runs on the N64.

```sh
# Recover/convert the selected disc, including the precomputed QR table.
uv run --locked director64 assets --game loewenzahn-1

# Generate the PDFs locally from the recovered model and original PICTs.
uv run --locked python games/loewenzahn-1/host/printing.py

# Copy the inspected PDFs to your server's directory, e.g.:
rsync build/loewenzahn-1/sha256-f31970b980f0/printing/{bas,rez}-?.pdf \
  myhost:/srv/www/print/lz1-f319/

uv run --locked director64 build --game loewenzahn-1 --reuse-assets
```

The export, manifest and review files live under the selected build's ignored
`printing/` directory. Only PDFs are published. Any static file server works;
no custom web app or application backend is involved. Updates should replace
the intended PDFs at those same paths after inspection.

The installed ImageMagick PICT reader rejected the six source files. The
exporter handles their observed single-raster, 1-bit PackBits subset explicitly:
header/opcode sequence, palette, dimensions, DPI, every decompressed row and the
end-of-picture marker are checked. Unsupported formats fail rather than being
silently approximated. The exported PNG pixels agree exactly with the initial
independent local raster extraction.

QR matrices are generated on the host by libqrencode at error correction level M.
The configured URLs produce 37 × 37 matrices. The N64 draws them at six pixels
per module with a four-module white border: 270 × 270 pixels including the
border. The matrices contain 2,752 bytes of data in bounded slots reserving
4,816 bytes; titles and links are also included in the ROM. The overlay displays the title and a readable fallback
address. Requests, repeated input, disconnects, dismissal and the neutral-input
gate are handled by the portable game adapter in `runtime/printing.c`.

## Original behavior and adaptation

The local disc is `sha256-f31970b980f0`, full SHA-256
`f31970b980f096a343a9aec5f10d52931709fbdda3cf09e5a80023791e402af2`.
Its media declares Director 5 (raw 1217), matching `game.toml`.

The BAS stamp calls `drucken gSpiel, 122`; REZ calls `drucken gSpiel, 80`.
Both invoke `BUCHSCRIPT.drucken`, shared cast CURSOR.CXT, member 90. The last
argument is logo width. `mBuch` and `mKapitel` select the complete document;
printing is independent of narration position, quiz results, names and saves.

The original method opens `pmatic.dll` on Windows or `pmatic.xobj` on Mac and
creates a PrintOMatic object. If successful, it resets the document, sets
margins `(60, 50, 50, 30)`, creates a page and queries driver page dimensions.
It adds a logo at the top right, title at `(0, 58)`, body in a text box starting
at `(0, 90)`, an optional BAS illustration below the reported text insertion
point, and the copyright footer. Windows uses Arial and calls `mPrint`; Mac
uses Helvetica and calls `mDoJobSetup` before printing.

The port's `host/compatibility.py` checks the exact disc, movie, cast, member,
handler and recovered script hash before replacing only this method with a
`director64_print(mBuch, mKapitel)` host request. The recovered source AST stays
unchanged. The compatibility change is recorded in the AOT manifest. The
source stamp's press/release behavior is retained, and the game adapter installs
its print service after runtime initialization. It forwards all other calls.
This does not implement the full PrintOMatic extension.

Local source locations beneath the selected build directory:

- `inspection/BAS.DXR.lingo:71` and `inspection/REZ.DXR.lingo:85`: stamps.
- `inspection/CURSOR.CXT.lingo:864`: book state and chapter selection.
- `inspection/CURSOR.CXT.lingo:944`: original print method.
- `inspection/CURSOR.CXT.lingo:1002`: extension initialization and platform paths.
- `director/model.json`: original title/body bytes and decoded fields.
- `extracted/MEDIA/PRINT/`: the six original print images.

## Validation

```sh
uv run --locked pytest tests/test_loewenzahn_printing.py tests/test_build_inputs.py
mise run test-native
uv run --locked director64 native --game loewenzahn-1 --sanitizers --printing
uv run --locked director64 probe --game loewenzahn-1 PRINT
uv run --locked director64 capture --game loewenzahn-1 PRINT
```

The sanitized native printing journey opens both books through source pointer
input, prints every chapter, checks document selection and a frozen game clock,
exercises held A/B input, closes the QR, turns the page and returns to PANO.
The shorter PRINT journey produces an isolated probe ROM that opens BAS8 and
BAS7, closes both overlays and returns to the panorama. Probe code is excluded
from the release ROM. Native evidence lives under `native/printing/`.

PDF validation checked complete text extraction for all 16 documents, all
rendered pages, image scale and every served PDF's HTTP status, content type
and SHA-256. All URLs returned HTTP 200 with `application/pdf` and no forced
attachment. An independent ZXing decoder recovered all 16 intended URLs from
the precomputed QR matrices at their N64 display size. Publication evidence is
in `printing/published.json`; the initial analysis is in `printing-analysis/`.

The Gopher64 PRINT capture in `captures/probe-urh38zgx/` rendered both QR
dialogs, closed them and reached PANO. ZXing decoded the intended BAS8 and BAS7
URLs from those captured frames after extracting the even field. The unprocessed
capture's alternating blank lines prevent QR decoding at its original size;
this is the existing interlaced capture artifact. `qr-validation.json` records
the processing and scope explicitly. This is not a physical camera/TV test.

The local ScummVM checkout was clean at
`41ac2b31847622d0662d22c03fe6979e3b43cfbc` during the source analysis.
`engines/director/lingo/xlibs/p/printomatic.cpp` documents the extension methods
but implements printing as stubs. `image/pict.cpp` provides the PixMap, palette
and PackBits reference. Reference agreement and successful native/emulator
checks do not establish original-projector print fidelity or hardware results.

An actual phone scan from M64/original-N64 TV output and a physical print job
remain to be checked. The phone uses its normal
[AirPrint](https://support.apple.com/en-us/109349) or
[Android print flow](https://support.google.com/chrome/answer/1069693?co=GENIE.Platform%3DAndroid&hl=en).
