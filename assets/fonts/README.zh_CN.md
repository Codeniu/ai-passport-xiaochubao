<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 菜谱字体源文件

`cook_font_14.c`、`cook_font_18.c`、`cook_font_26.c` 是离线菜谱应用使用的 LVGL
点阵字体，由脚本生成。每个文件只收录 `cook_chars.txt` 里列出的字形，这样中文
字形占用才能压进 ESP32-C3 的 Flash 预算。它们是生成产物：要改内容请改生成脚本，
不要直接改这三个文件。

## 来源与许可

- 上游字体族：Noto Sans SC，字重 400（Regular）、500（Medium）、700（Bold）。
- 许可：SIL Open Font License 1.1。
- 上游字体族携带的版权声明：
  `Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'`。
- 这三个文件属于 OFL 意义上的修改版本。依据第 3 条，它们没有使用保留字体名：
  生成的符号名为 `cook_font_14`、`cook_font_18`、`cook_font_26`。它们仍然
  受 OFL 约束，并随固件一起分发。

## 字形覆盖

- `cook_chars.txt` 由 `tools/build_cook_assets.py` 生成，包含可打印 ASCII 区间，
  以及全部菜谱文案和生成脚本里 `UI_TEXT` 常量用到的字符。
- `cook_font_14`、`cook_font_18` 覆盖上述完整字符集。
- `cook_font_26` 只收录真正以 26px 渲染的字符，目前即各页标题。清单在
  `cook_chars_titles.txt`，由 `tools/build_cook_assets.py` 的 `UI_TEXT_TITLES`
  常量生成；新增 26px 文案时必须同步补充，否则会显示成缺字方块。

## 重新生成

需要 Node.js 与 `lv_font_conv`，以及 `@expo-google-fonts/noto-sans-sc` 包。
两者默认从本机 Node 工作区解析，可用 `--lv-font-conv` 与 `--font-dir` 覆盖。

```bash
# 1. 重新生成字符清单、菜谱数据表与封面 blob。
python3 tools/build_cook_assets.py

# 2. 重新生成三个字体源文件。
python3 tools/build_cook_fonts.py

# 3. 校验字形覆盖与资源自洽性。
python3 tests/test_cook_assets.py

# 4. 重新构建，把新字体源文件编进固件。
idf.py build
```

只要改动了屏幕上出现的文案，第 1 步就必须先于第 2 步执行。UTF-8 编码正确、
编译通过，都不代表字形存在；只有 `tests/test_cook_assets.py` 和真机检查能证明。
