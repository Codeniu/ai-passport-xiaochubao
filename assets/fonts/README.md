<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Recipe font sources

`cook_font_14.c`, `cook_font_18.c`, and `cook_font_26.c` are generated LVGL bitmap
fonts used by the offline recipe application. Each file contains only the glyphs
listed in `cook_chars.txt`, which keeps the CJK payload inside the ESP32-C3 Flash
budget. They are generated sources: edit the generator, not these files.

## Source and license

- Upstream family: Noto Sans SC, weights 400 (Regular), 500 (Medium), 700 (Bold).
- License: SIL Open Font License 1.1.
- Copyright notice carried by the upstream family:
  `Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'`.
- These files are Modified Versions under the OFL. Per clause 3 they do not use
  the Reserved Font Name: the generated symbols are named `cook_font_14`,
  `cook_font_18`, and `cook_font_26`. They remain under the OFL and are
  redistributed with the firmware.

## Character coverage

- `cook_chars.txt` is produced by `tools/build_cook_assets.py`. It contains the
  printable ASCII range plus every character used by the recipe text and by the
  `UI_TEXT` constants in the generator.
- `cook_font_14` and `cook_font_18` cover that full list.
- `cook_font_26` only covers the characters that are actually rendered at 26 px,
  which today means the page titles. That list lives in `cook_chars_titles.txt`,
  produced by `tools/build_cook_assets.py` from its `UI_TEXT_TITLES` constant; add
  new 26 px strings there too or they render as missing-glyph boxes.

## Regenerating

Requires Node.js with `lv_font_conv`, and the `@expo-google-fonts/noto-sans-sc`
package. Both are resolved from the local Node workspace by default; override
with `--lv-font-conv` and `--font-dir`.

```bash
# 1. Rebuild the character list, the recipe tables, and the cover blobs.
python3 tools/build_cook_assets.py

# 2. Regenerate the three font sources.
python3 tools/build_cook_fonts.py

# 3. Check glyph coverage and asset self-consistency.
python3 tests/test_cook_assets.py

# 4. Rebuild so the new sources are compiled into the firmware.
idf.py build
```

Changing any on-screen string means step 1 must run before step 2. UTF-8 text and
a successful build do not prove the glyphs exist; only `tests/test_cook_assets.py`
and an on-device check do.
