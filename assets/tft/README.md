# TFT flow.io wordmarks

The splash (92 × 26 pixels, FreeSans regular) and overview header (72 × 18
pixels, FreeSans bold) use pre-rendered RGB565 images, stored in flash. Their
bounds match the previous GFX text. The background is white; `flow` is
RGB (7, 59, 102) and `.io` is RGB (0, 174, 239). Change the assets too if the
TFT header background changes.

The PNG files preview the actual RGB565 colours. The generated firmware header
is `src/Modules/TFTModuleS3/FlowIoWordmarkBitmaps.h`. Neither font files nor
Pillow are required to build the firmware.

## Regenerate

Use Python with Pillow 12.3.0 and the official GNU FreeFont 20120503 fonts:
<https://ftp.gnu.org/gnu/freefont/freefont-ttf-20120503.zip>.
Extract the archive, then run from the repository root:

```sh
python3 scripts/generate_tft_wordmarks.py --font-dir /path/to/freefont-20120503
```

The script renders at eight times the nominal resolution, downsamples with
Lanczos, and quantizes to RGB565. It generates both PNG previews and the C++
header. The two variants share the same rendering and colour conversion code.
