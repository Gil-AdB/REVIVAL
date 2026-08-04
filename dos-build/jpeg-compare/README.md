# JPEG decoder comparison (DOS build)

The 1998 `FDS/JPEGLIBD.LIB` is IJG libjpeg **v5**, which compiles only the FLOAT IDCT.
`FDS/JPEG6/` is IJG **jpeg-6b** + `FDS/SOURCE/IMGCODE/JPEG6LD.CPP` (our own `LoadJPEG`:
one `fread`, memory source, decode straight into the destination rows).

## Measured on 86Box (P55C-233), 53 textures, identical tree — only `jpg` moved

| variant                  | decode Mcy | vs v5 | total texture load | @233MHz |
|--------------------------|-----------:|------:|-------------------:|--------:|
| v5 lib (1998)            |     1147.6 |     — |             1284.9 |  5.51 s |
| 6b float                 |     1119.2 | -2.5% |             1257.9 |  5.40 s |
| 6b islow  **(default)**  |      859.1 |  -25% |              994.2 |  4.27 s |
| 6b ifast                 |      755.4 |  -34% |              891.3 |  3.82 s |
| 6b ifast + fastup        |      671.1 |  -42% |              806.3 |  3.46 s |

At matched IDCT (float) the rewrite is worth only 2.5% — the whole win is the integer IDCT
that v5 never shipped, NOT the Targa-writer path.

## Accuracy vs the v5 lib, on the RAW DECODED TEXTURE

| variant | max byte delta |
|---------|---------------:|
| float   | 1  (reproduces v5's algorithm; proves the loader is correct) |
| islow   | 4  |
| ifast   | 10-18 |

`WALL3.JPG` shows scattered deltas to ~30 in ALL variants including float => a v5-vs-6b
chroma UPSAMPLING difference, not the IDCT and not our loader.

Look at `01_difference_x12.png`: float/islow are black, ifast rings along every 8x8 block edge.

## Files

- `00_side_by_side.png` — v5 / float / islow / ifast, four textures
- `01_difference_x12.png` — difference vs v5, amplified 12x
- `<texture>_<variant>.png` — the individual decoded textures

## Reproducing

Build (islow is the default now; `FDS_JPEG_V5=1` gets the 1998 lib back):

    cd dos-build
    docker run --rm --platform linux/amd64 \
      -e FDS_TEX_SWIZZLE=1 -e FDS_FUSED_GOUR=1 -e FDS_CWOB=1 -e FDS_MMXWOB=1 \
      -e FDS_ZBUF=1 -e FDS_ZLINEAR=1 -e FDS_ZFLIP=1 -e FDS_SUBTEX=1 -e FDS_NGON=1 \
      -e FDS_JPEG6_IDCT=ifast \
      -v "$PWD/rev-build:/work" -v "$PWD/ow:/ow" -v "$PWD/build-dos.sh:/build-dos.sh:ro" \
      ubuntu:22.04 bash /build-dos.sh

Regenerate the texture dumps: add `-e FDS_TEXDUMP=1`, run `./run-rebuilt.sh --headless`,
and collect `runtime-rebuilt/TEX0..3.PPM` (written before ANY engine processing).

DO NOT A/B decoders on rendered city frames: two runs of the SAME binary differ by 36-55%
of bytes from the uninitialised-FlareSize reflection coin flip.
