<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Little Kitchen Pal

An entirely offline recipe reference for the AI Passport. The firmware carries 340
community-collected home-cooking recipes — names, ingredient lines, numbered
steps, and 177 real photographs — with no Wi-Fi, no HTTP, and no filesystem
access. Boot it and you either get a random dish suggested or a browsable recipe
list. The on-screen title is the Chinese name; this archive refers to it as Little
Kitchen Pal.

This page is the design and implementation archive. For button actions, settings,
and known limits, see the [user guide](USER_GUIDE.md). The day-by-day development
log is in the [changelog](CHANGELOG.md).

## What it does

- **Boots straight into the app**: no demo menu is registered.
- **Random suggestion**: draws one dish at a time and shows its cover, name, and
  category. Short-press OK for another one.
- **Category page**: the 340 recipes are grouped into 15 categories in the source
  data's own directory order, each row showing the category name and how many
  recipes it holds. "Browse recipes" lands here first.
- **Recipe list**: the recipes of the selected category, short press moves one row,
  long press pages by one screen. It shares its paging and highlighting code with
  the category page, so both feel identical.
- **Recipe detail**: three sub-pages — cover and name, ingredients, steps. Long
  press UP/DOWN moves between sub-pages; a short press pages the current sub-page
  by one screen, and at its last page it advances to the next sub-page.
- **Covers decoded on demand**: the 177 photographs ship as two pre-baked
  baseline-JPEG blobs. LVGL's built-in tjpgd decodes them row by row, so no
  full-resolution frame ever lands in RAM.
- **Battery readout**: top-right corner, refreshed every 5 seconds, red below 20%.
  Shows `--` when the fuel gauge is unavailable instead of inventing a number.
- **Settings**: brightness (25 / 50 / 72 / 100%) and a screen-off timeout (off,
  30 s, 1 min, 5 min, 10 min), both persisted in NVS.
- **About**: project name, version, author, and the origins of the data, fonts,
  and screen-off logic.
- **Screen-off on idle**: at the timeout the backlight goes off, the panel enters
  Sleep In, and LVGL stops refreshing; 5 seconds later the ESP32-C3 starts
  cycling through light sleep and only wakes every 120 ms to poll the buttons.
  Any key restores the display and you land back on the page you were reading.
- **Key-hint bar** at the bottom of the home, category, list, and settings pages
  only; the random, detail, and about pages give that height to content.

## Interaction

- **UP/DOWN (click)**: home = switch between the two buttons; categories and list =
  move one row; detail = page the current sub-page by one screen; settings = move
  one row; about = scroll.
- **UP/DOWN (long-press)**: categories and list = page by one screen; detail = move
  between the three sub-pages.
- **OK (click)**: home = enter the selected item; random = open the steps for this
  dish; categories = open that category's list; list = open the recipe; settings =
  adjust the selected row.
- **OK (long-press)**: home = open settings; random, categories, and settings =
  back to home; list = back to the category page; detail = back to the page it was
  opened from; about = back to settings.
- **While the screen is off**: the first key only wakes it; it never also acts.

## Data and sizing

| Item | Size |
| --- | --- |
| Recipes | 340 across 15 categories |
| Recipes in the random draw | 279 |
| Recipe text in `.rodata` | 117,795 B (~115 KiB) |
| Photographs with a cover | 177 of 340 |
| `main/assets/covers_large.bin` | 1,125,031 B (200 x 150) |
| `main/assets/covers_small.bin` | 698,243 B (144 x 108) |
| Three Chinese font subsets | 748,352 + 1,097,787 + 122,589 B |

The character-list step is what makes this fit: the three CJK subsets cover 987 Han
characters plus printable ASCII, where shipping a full Noto Sans SC face at three
sizes would not.

## Design notes that mattered

- **JPEG instead of raw bitmaps.** A raw 200 x 150 RGB565 cover is 60 KB; 177 of
  them would be 10 MB. Baseline JPEG brings the same set down to 1.1 MB, and tjpgd
  streams one MCU row at a time so decoding needs roughly 4 KB of work buffer
  rather than a full frame.
- **Two sizes baked at build time.** Runtime scaling needs a decoded full frame in
  RAM, which does not fit on a part without PSRAM. The 200 x 150 and 144 x 108
  variants are prepared by the asset script instead.
- **The random pool is its own table.** 40 pantry entries (sauces, soup bases, oils)
  and 21 bottled drinks are recipes in the source data but are not dishes. They stay
  visible in the list and are excluded from the random draw by a generated index
  table, so the draw cannot land on "mineral water".
- **The page and key state machine has no LVGL or ESP-IDF dependency.** Page
  transitions, list windowing, and random selection compile and run on the host, so
  they are covered by tests that need no board.
- **The cover descriptor is rotated through slots.** LVGL's render task can still be
  reading an `lv_image_dsc_t` when the next page rewrites it; four rotating slots
  remove that window.
- **No resting page, but real low power.** An earlier version showed a "resting"
  screen for 30 seconds before turning the backlight off. That page wanted to show
  a clock, and there is no trustworthy clock on this board, so the page was
  dropped. Turning the backlight off alone does not really save power though: the
  CPU keeps running at full speed and the panel still draws current. So after
  blanking, the ESP32-C3 cycles through light sleep, and `iot_button_stop()`
  disables the button component's 5 ms polling timer first — leave it running and
  light sleep gets chopped into 5 ms slices, saving nothing. Light sleep rather
  than deep sleep because waking has to land back on the same page (deep sleep is
  a reboot), and because all three buttons share one ADC resistor ladder: the UP
  key still sits above the digital low threshold when pressed, so there is no
  dependable GPIO wakeup source and polling the voltage is the only option.
- **Ingredient and step markers are recoloured, not separate widgets.** Each line is
  one label whose text opens with an inline `#RRGGBB` command, so the bullet or
  number is orange and the text is ink-coloured without adding a second object per
  line — LVGL's heap here is 48 KB. The price is that recipe text must not contain
  `#`; `tests/test_cook_assets.py` enforces that.

## Source

- Repository: `codeniu/ai-passport`, branch `feature/cook-like-hoc`.
- Recipe text and photographs: the community
  [CookLikeHOC](https://github.com/Gar-b-age/CookLikeHOC) repository. **That
  repository declares no license**, so redistributing this firmware publicly is a
  legal review item rather than a settled matter; the reasoning is recorded in
  `main/assets/README.md`.
- Fonts: Noto Sans SC (SIL OFL 1.1), subset by `tools/build_cook_fonts.py`; see
  `assets/fonts/README.md`.
- Not yet submitted to the community project list. When it is, record the cover
  image by file name, dimensions, and format only — the archive stays text-only and
  does not store the firmware `.bin`.
