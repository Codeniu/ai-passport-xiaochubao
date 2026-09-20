<p align="right">
  <a href="USER_GUIDE.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Little Kitchen Pal — User Guide

A short, practical guide to the offline recipe application: what it is, how to
drive it with the three buttons, and what it does when you leave it alone.
Design rationale, sizing numbers, and the asset pipeline are in
[`README.md`](README.md); this page is the one to read before you pick the device
up. What changed and when is in the [changelog](CHANGELOG.md).

## Introduction

Little Kitchen Pal turns the AI Passport into a pocket cookbook that never
touches a network. The firmware itself carries 340 community-collected home
cooking recipes — name, category, ingredient lines, numbered steps — plus 177
real photographs of the finished dishes. There is no Wi-Fi setup, no account, no
server, and no filesystem: the recipe text and the pictures are baked into the
binary at build time, so the device works in a kitchen drawer, on a train, or
anywhere else with no signal.

Everything fits because nothing is decoded up front. The photographs are stored
as baseline JPEG and decoded row by row by LVGL's built-in tjpgd, so a full
frame never lands in the ESP32-C3's RAM. The Chinese text uses three font
subsets built from exactly the characters the app can display, not a full CJK
face.

Two ways in, depending on how decisive you feel:

- **Not sure what to cook?** Let it draw a dish for you.
- **Already know?** Browse the list and open the recipe.

## What you need

- An AI Passport (ESP32-C3, 240 x 320 display, UP / DOWN / OK buttons).
- The firmware from branch `feature/cook-like-hoc` flashed once. After that the
  device boots straight into the app — there is no demo menu to pass through.
- Nothing else. No phone, no network, no SD card.

The three buttons are the whole interface. Every button distinguishes a short
press (**click**) from a long press (**hold**, roughly half a second), so six
gestures cover the app. When the screen is off, the first press only wakes it —
it never also acts, so you never wake the device into an action you did not
intend.

## Pages at a glance

| Page | What you see | Bottom bar |
| --- | --- | --- |
| **Home** | Title, recipe count, two buttons | Yes |
| **Random** | Cover photo, dish name, category | No |
| **Categories** | Nine categories per screen, each with its recipe count | Yes |
| **List** | The recipes of one category, nine rows per screen | Yes |
| **Detail** | Cover and name / ingredients / steps | No |
| **Settings** | Brightness, screen-off timeout, About | Yes |
| **About** | Project, version, author, sources | Hint line only |

A status bar runs across the top of every page. The battery readout is pinned to
the top-right corner and never moves; on the detail page the sub-page counter
(`1/3`, `2/3`, `3/3`) sits on the left of the same bar, and the text page
counter (`n/m`) sits at the bottom right.

## Button reference

### Home

| Gesture | Result |
| --- | --- |
| UP / DOWN click | Switch between the two buttons |
| OK click | Enter the highlighted button |
| OK hold | Open Settings |

### Categories

Choosing "Browse recipes" on Home lands here first. The 340 recipes are split
into 15 categories, kept in the source data's own directory order: staples 32,
cold dishes 4, braised 11, breakfast 35, soups 6, stir-fries 71, stews 14,
fried 18, grilled 1, blanched 12, hot pots 11, clay pots 15, steamed 49, pantry
40, drinks 21.

| Gesture | Result |
| --- | --- |
| UP / DOWN click | Move the highlight one row; crossing the last row of a screen loads the next screen and lands on its first row |
| UP / DOWN hold | Move one whole screen |
| OK click | Open the recipe list for this category |
| OK hold | Back to Home |

### Random

| Gesture | Result |
| --- | --- |
| UP / DOWN click or hold | Draw another dish |
| OK click | Open the steps for this dish |
| OK hold | Back to Home |

Only 279 of the 340 entries take part in the draw. The source data also contains
40 pantry items (sauces, soup bases, oils) and 21 bottled drinks; they stay
browsable in the list but would make a poor suggestion, so a generated index
table excludes them.

### List

| Gesture | Result |
| --- | --- |
| UP / DOWN click | Move the highlight one row; crossing the last row of a screen loads the next screen and lands on its first row |
| UP / DOWN hold | Move one whole screen |
| OK click | Open the highlighted recipe |
| OK hold | Back to Categories |

The header shows the category name and the position within it. Paging and
highlighting behave exactly as on the category page — the two share the same
paging logic. It never scrolls into an empty screen: at the last recipe of the
category the highlight simply stops.

### Detail

Three sub-pages: **cover and name**, **ingredients**, **steps**.

| Gesture | Result |
| --- | --- |
| UP / DOWN click | Page the current sub-page by one screen; at its last page, move to the next sub-page |
| UP / DOWN hold | Move to the previous / next sub-page |
| OK click or hold | Back to the page you came from |

Ingredients and steps are separate sub-pages rather than one long scroll,
because a recipe with eighteen ingredient lines is easier to read as "page 1 of
2" than as an endless scroll. Bullets and step numbers are orange; the text is
ink-coloured.

### Settings

| Gesture | Result |
| --- | --- |
| UP / DOWN click | Move between rows |
| OK click | Brightness / timeout: step to the next value. About: open the About page |
| OK hold | Back to Home |

### About

| Gesture | Result |
| --- | --- |
| UP / DOWN click | Scroll the text |
| OK click | Back to Settings |

## Settings in detail

**Brightness** — 25%, 50%, 72%, 100%. Written to NVS, so it survives a reboot.
Lower it if you are cooking by dim light; the panel is the dominant power draw.

**Screen-off timeout** — off, 30 s, 1 min, 5 min, 10 min. Reaching the timeout
happens in two steps:

1. **Blank** — the backlight goes off, the panel enters Sleep In, and LVGL stops
   refreshing. The interface itself stays untouched in RAM.
2. **Low power** (5 s later) — the ESP32-C3 repeatedly enters light sleep, waking
   every 120 ms only to poll the buttons. All three buttons share one resistor
   ladder on a single ADC pin, so reading the voltage is the only way to tell
   them apart.

Any key at either step brings you straight back to the recipe you were reading.
There is no intermediate "resting" screen: an earlier version had one, it wanted
to show a clock, and there is no trustworthy clock on this board.

Light sleep rather than deep sleep: deep sleep means a reboot, so the page state
would have to be persisted and waking would cost a second or two, while light
sleep keeps RAM and simply resumes. The trade-off is that it drops the board from
tens of milliamps to roughly a milliamp rather than to microamps — for a 500 mAh
cell, a day or two of extra idle time.

**About** — project name, version, author, and where the recipes, fonts, and
screen-off approach came from.

## Battery indicator

Top-right corner, refreshed every five seconds: a percentage plus a small
battery outline whose fill tracks the level. Below 20% both turn red. If the
fuel gauge is not answering, it shows `--` rather than inventing a number.
Accuracy depends on the cell and the battery profile; treat it as an indicator,
not a calibrated measurement.

## Where the content comes from

- Recipe text and photographs: the community
  [CookLikeHOC](https://github.com/Gar-b-age/CookLikeHOC) repository.
  **That repository declares no license.** Embedding the text and photographs in
  firmware and distributing it publicly is therefore a legal review item, not a
  settled matter; the reasoning is recorded in `main/assets/README.md`.
  Internal evaluation and personal use are within a safe range.
- Fonts: Noto Sans SC, SIL Open Font License 1.1, subset by
  `tools/build_cook_fonts.py`; see `assets/fonts/README.md`.
- Screen-off and brightness approach: modelled on
  [leo-radio](https://github.com/leo0183/leo-radio) (NVS persistence plus
  backlight control).

## Building and flashing

Only needed if you are changing the app; a normal user flashes once.

```bash
# 1. Regenerate the recipe table, cover blobs, and character lists.
python3 tools/build_cook_assets.py

# 2. Rebuild the three font subsets from those character lists.
python3 tools/build_cook_fonts.py

# 3. Check glyph coverage and asset consistency.
python3 tests/test_cook_assets.py

# 4. Build and flash.
idf.py build
idf.py -p <PORT> flash
```

Step 1 must run before step 2 whenever on-screen text changes: correct UTF-8 and
a successful build do not prove that a glyph exists. Only step 3 and a look at
the real screen do. The 26 px title font uses its own small character list
(`assets/fonts/cook_chars_titles.txt`); updating the shared list alone leaves
title glyphs missing.

## Known limits

- 163 of the 340 recipes have no photograph and show a placeholder on the cover
  sub-page; the text is complete.
- Recipe text is verbatim from the source repository, including its measure
  wording. There is no search, no favourites, and no editing — the data is
  read-only by design.
- The device has no clock, so nothing in the app is time-aware.
- No audio. The ES8311 is untouched by this application.
