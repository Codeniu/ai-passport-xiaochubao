<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Little Kitchen Pal — Changelog

Development log for the offline recipe application. Entries are grouped by the
day the change landed; the project has no release tags yet. Button actions and
settings are documented in the [user guide](USER_GUIDE.md), and the design and
implementation archive lives in [`README.md`](README.md).

## 2026-09-20 — Recipe categories

- Added a category page between the home screen and the recipe list. Short-press
  OK on the list entry now lands on a category list, and the list page itself
  shows only the recipes of the selected category.
- The category list follows the source repository's directory order, 15 entries,
  with no "all" row:

  | Category | Recipes | Category | Recipes |
  | --- | ---: | --- | ---: |
  | Stir-fry | 71 | Clay pot | 15 |
  | Steamed | 49 | Braised | 14 |
  | Pantry | 40 | Blanched | 12 |
  | Breakfast | 35 | Master stock | 11 |
  | Staples | 32 | Hot pot | 11 |
  | Drinks | 21 | Soup | 6 |
  | Fried | 18 | Cold dishes | 4 |
  |  |  | Roasted | 1 |

- Each row shows the category name at 18 px on the left and its recipe count at
  14 px on the right, using the same orange cursor and row layout as the list
  page.
- The category page reuses the list page's paging and selection code, so the
  cursor, page turning, and row animation behave identically on both pages.
  A long OK press on the list page now returns to the category page instead of
  the home screen.
- The list header shows the active category name and an `n/category-total`
  counter instead of `n/340`, and the hint bar reads "long-press OK for
  categories, short-press OK to confirm".
- Data pipeline: `tools/build_cook_assets.py` now emits `COOK_CATEGORIES[]`
  (`name`, `first`, `count`) plus the `COOK_CATEGORY_OFFSETS[]` and
  `COOK_CATEGORY_SIZES[]` tables the state machine reads. Categories are
  contiguous ranges inside `COOK_RECIPES`, so no extra index table is needed.
- State machine: added `COOK_PAGE_CATEGORY` together with `cat_selected` and
  `list_category`; `list_selected` is now an index inside the active category
  while `detail_index` remains global.
- Tests: host tests cover entering the category page, paging through it,
  selecting a category, and returning from the list page.

  **Not yet verified on hardware** — the image for this change is built but had
  not been flashed to a device at the time of writing.

## 2026-09-20 — Screen-off now means low power

- Turning the screen off used to only drop the backlight. It now blanks the panel
  first and then puts the chip into light sleep.
- Added `main/cook_power.{h,c}`: the sleep phase gained a `BLANK` stage (backlight
  off, panel sent a Sleep In command, refresh stopped, CPU still running so keys
  stay instant) that hands over to a `LIGHT` stage (repeated light sleep) after
  `COOK_SLEEP_LIGHT_DELAY` seconds.
- Light sleep needs the `iot_button` polling stopped (`iot_button_stop()`),
  otherwise its 5 ms timer wakes the chip immediately; buttons are instead wired
  as GPIO wake-up sources so any key still brings the device back.
- Settings keep their existing wake-up behaviour: any key press restores the
  backlight and the panel.

## 2026-09-20 — Missing glyphs in page titles

- Fixed garbled text at the top of the settings and about pages. The 26 px title
  font is a separate, much smaller subset, and its character list used to be
  hard-coded in `tools/build_cook_fonts.py`; it did not contain the four
  characters used by those two titles, and LVGL silently rendered boxes.
- Title characters are now generated: `tools/build_cook_assets.py` owns a
  `UI_TEXT_TITLES` constant and writes `assets/fonts/cook_chars_titles.txt`, and
  the font script reads that file. The hard-coded list was removed.
- Added `check_font_coverage()` to `tests/test_cook_assets.py`: it works out
  which strings each font size renders by scanning `main/demo_cook.c` for
  `&cook_font_NN`, then walks the generated `cook_font_NN.c` cmap (both
  `FORMAT0_TINY` ranges and `SPARSE_TINY` offset tables) and checks every code
  point. All three sizes pass now.

## 2026-09-20 — Second round of hardware feedback

- Removed the standby page. It was supposed to show a clock, but the board has no
  trustworthy time source, so reaching the sleep timeout now turns the screen off
  directly. The sleep phase dropped from three stages to two, and the settings
  entry for the removed stage went with it.
- Fixed the battery indicator wrapping onto a second line: its label was 34 px
  wide while `100%` needs about 35 px at 14 px font size. The label is now 46 px
  (`COOK_BATT_LABEL_W`) and explicitly clips instead of wrapping.
- Fixed list rows sitting too high. The 18 px font has a 21 px line height
  against a 24 px row, so each row now gets an equal-height slot container and
  the label is aligned `LV_ALIGN_LEFT_MID` inside it.
- Reworked the recipe detail view into three sub-pages (cover and title,
  ingredients, steps). Ingredients and steps each flip a whole page at a time; a
  short OK press turns the page and rolls into the next sub-page at the end,
  while a long OK press jumps between sub-pages directly. Leading bullets and
  numbers are tinted orange through LVGL's `#RRGGBB ... #` recoloring, which
  costs no extra widgets in a 48 KB LVGL heap. Recipe text must not contain a
  `#` character anymore; a test guards the invariant.

## 2026-09-20 — Documentation and preview

- Added `USER_GUIDE.md` and `USER_GUIDE.zh_CN.md`: introduction, page overview,
  per-page button tables, settings explained, battery display, content sources
  with a licensing note, build and flash steps, and known limits. Both READMEs
  and the reference index link to it.
- Fixed overlapping battery graphics in the preview generator: the frame was
  positioned with a `right:` offset that shifted it by its own width, so it sat on
  top of the percentage text. It now uses the same absolute `left` coordinates as
  the firmware.

## 2026-09-18 — First round of hardware feedback

- Fixed the invisible home selection: the item containers were created with a
  fully transparent background, so setting only the background colour did
  nothing. The refresh path now also sets `bg_opa` to `LV_OPA_COVER`, and the
  selected row uses the primary orange.
- Fixed a reset that fired whenever a page with a cover photo was opened: the
  task stack was the `esp_lvgl_port` default of 7168 bytes while LVGL's tjpgd
  `decoder_info()` reserves a 4096-byte buffer on the stack, so the render path
  overflowed and the chip raised a stack protection fault. Tasks that decode
  JPEG now get at least 16 KB of stack.
- Fixed page switching to load the new screen before deleting the old one,
  because deleting the active screen leaves LVGL with a NULL current screen and
  crashes on the next refresh.
- Corrected the input task stack comment: `xTaskCreate` counts stack depth in
  4-byte words, so 8192 was already 32 KB.

## 2026-09-18 — First usable build

- Added the application itself: `main/cook_model.{h,c}` holds a four-page state
  machine (home, random, list, detail) with no dependency on LVGL or ESP-IDF so
  it can be tested on the host, and `main/demo_cook.c` holds the LVGL screens.
  The device now boots straight into the app with no demo menu.
- Added the asset pipeline: `tools/build_cook_assets.py` parses the 340 recipe
  Markdown files into C tables, two sizes of cover JPEG blobs, and the character
  lists; `tools/build_cook_fonts.py` builds 14/18/26 px Noto Sans SC subsets
  through `lv_font_conv`.
- Random draws skip pantry and drink entries, leaving 279 real dishes as
  candidates, while the list still offers all 340. The draw walks the table twice
  instead of allocating a 4096-entry array on an 8 KB task stack.
- Deduplicated four recipe names that collided with each other upstream, using
  the file name to disambiguate; all 340 names are unique now, with a test to
  keep them that way.
- Added `tests/test_cook_model.c` and `tests/test_cook_assets.py` to the host
  test run.

## Known upstream baseline issues

These are unrelated to this application and remain open:

- `tests/test_check_repo.py` fails five cases on Windows because
  `tools/check_repo.py` formats paths with `Path.relative_to`, which yields
  backslashes there while the test expects forward slashes.
- `actionlint` ships an installer for Linux and macOS only, so the validation
  script cannot run its step on Windows.
