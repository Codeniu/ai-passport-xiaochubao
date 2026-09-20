<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Generated recipe cover blobs

`covers_large.bin` and `covers_small.bin` are generated build inputs for the
offline recipe application. They are concatenations of baseline JPEG covers, one
entry per recipe that has a real photo, and are embedded into Flash by the
`EMBED_FILES` directive in `../CMakeLists.txt`.

The offsets and sizes of every entry live in the generated tables
`../generated/cook_covers.c`, and the per-recipe index lives in
`../generated/cook_data.c`. The binaries alone are not enough to locate a cover.

## Why JPEG

The ESP32-C3 has no PSRAM. LVGL's built-in tjpgd decoder streams one MCU row at a
time, so decoding needs roughly 4 KB of work buffer instead of a full decoded
frame. Decoding a blob entry goes through `LV_USE_FS_MEMFS`, which wraps the
memory range of a single `lv_image_dsc_t` as a readable stream. Both options are
enabled in `../sdkconfig.defaults`.

Two sizes are baked at build time rather than scaled at runtime:

| Blob | Size | Used by |
| --- | --- | --- |
| `covers_large.bin` | 200 x 150 | random recommendation page |
| `covers_small.bin` | 144 x 108 | recipe detail page |

Runtime scaling would need a decoded full frame in RAM, which does not fit.

## Regenerating

```bash
python3 tools/build_cook_assets.py     # rewrites both blobs and the tables
python3 tests/test_cook_assets.py      # checks JFIF headers and table bounds
idf.py build
```

Every entry must start with the JFIF APP0 signature `FF D8 FF E0 00 10 4A 46 49 46`.
LVGL's `is_jpg` check requires exactly that prefix; an entry written by a different
encoder can be rejected even though the file is a valid JPEG. `tests/test_cook_assets.py`
enforces this.

## Source and licensing status

- Source: the `CookLikeHOC` repository (`https://github.com/Gar-b-age/CookLikeHOC`)
  that sits next to this repository in the workspace. The recipe text and the
  photographs come from there; this project only crops and scales them.
- **The upstream repository declares no license.** There is no `LICENSE`,
  `COPYING`, or equivalent file, and its README contains only a disclaimer about
  unrelated websites. The photographs are community-contributed, and the recipes
  describe dishes from a commercial restaurant brand.
- Consequence: redistributing this firmware publicly, or shipping a product built
  from it, republishes third-party text and photographs without a granted license.
  Treat that as a legal review item, not a settled matter. Internal evaluation and
  personal use are the safe scope until the upstream terms are confirmed with the
  upstream maintainers or the content is replaced.

Do not hand-edit these files. They are regenerated from the recipe photographs and
carry no independent content.
