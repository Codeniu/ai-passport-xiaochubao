#!/usr/bin/env python3
"""用 lv_font_conv 生成小厨宝的中文子集字体。

字形覆盖由 assets/fonts/cook_chars.txt 决定（由 tools/build_cook_assets.py 依据
全部菜谱文案与界面固定文案生成）。缺字会显示成缺字方块，因此改动文案后必须重新
运行两个脚本。

字符集通过十六进制码点（--range）而不是 --symbols 传入：--symbols 会把原始
CJK 字符放进命令行参数，在 Windows 上会经过代码页转换而损坏；十六进制码点是
纯 ASCII，不依赖终端编码。

依赖：Node.js + lv_font_conv（本机安装在 Node 工作区，可用 --lv-font-conv 覆盖）。
字形来源：Noto Sans SC（SIL Open Font License 1.1）。
"""

import argparse
import subprocess
import sys
from pathlib import Path

# 字号 -> (postscript 字重目录, 文件名前缀, 是否使用全量字符集)
FONTS = [
    ("cook_font_14", 14, "400Regular", "NotoSansSC_400Regular.ttf", "full"),
    ("cook_font_18", 18, "500Medium", "NotoSansSC_500Medium.ttf", "full"),
    ("cook_font_26", 26, "700Bold", "NotoSansSC_700Bold.ttf", "titles"),
]



def build_ranges(charset: str):
    """把字符集转成 lv_font_conv 的 --range 参数（含可打印 ASCII），并合并连续码点。"""
    codepoints = {0x20 + i for i in range(0x7E - 0x20 + 1)}
    codepoints |= {ord(c) for c in charset}
    ordered = sorted(codepoints)

    ranges = []
    start = previous = ordered[0]
    for cp in ordered[1:]:
        if cp == previous + 1:
            previous = cp
            continue
        ranges.append((start, previous))
        start = previous = cp
    ranges.append((start, previous))

    parts = []
    count = 0
    for first, last in ranges:
        count += last - first + 1
        if first == last:
            parts.append("0x%04X" % first)
        else:
            parts.append("0x%04X-0x%04X" % (first, last))
    return ",".join(parts), count


def find_lv_font_conv(explicit):
    if explicit:
        return Path(explicit)
    node_root = Path.home() / ".workbuddy" / "binaries" / "node" / "workspace"
    candidates = [
        node_root / "node_modules" / "lv_font_conv" / "lv_font_conv.js",
        node_root / "node_modules" / ".bin" / "lv_font_conv",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit("找不到 lv_font_conv，请先 npm install lv_font_conv 或用 --lv-font-conv 指定")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=None)
    parser.add_argument("--font-dir", default=None,
                        help="Noto Sans SC TTF 所在目录（@expo-google-fonts/noto-sans-sc）")
    parser.add_argument("--lv-font-conv", default=None)
    parser.add_argument("--node", default="node")
    args = parser.parse_args()

    repo = Path(args.repo).resolve() if args.repo else Path(__file__).resolve().parents[1]
    out_dir = repo / "assets" / "fonts"
    charset_path = out_dir / "cook_chars.txt"
    if not charset_path.is_file():
        raise SystemExit(f"缺少字符清单，请先运行 tools/build_cook_assets.py: {charset_path}")
    charset = charset_path.read_text(encoding="utf-8")

    # 标题字号只收标题文案的字，清单同样由 build_cook_assets.py 产出——把字符集全部
    # 放在一个脚本里维护，才不会出现"改了界面文案却只更新一处"的漏字。
    titles_path = out_dir / "cook_chars_titles.txt"
    if not titles_path.is_file():
        raise SystemExit(f"缺少标题字符清单，请先运行 tools/build_cook_assets.py: {titles_path}")
    title_charset = titles_path.read_text(encoding="utf-8")

    font_dir = Path(args.font_dir) if args.font_dir else (
        Path.home() / ".workbuddy" / "binaries" / "node" / "workspace"
        / "node_modules" / "@expo-google-fonts" / "noto-sans-sc")
    conv = find_lv_font_conv(args.lv_font_conv)

    for name, size, weight_dir, file_name, scope in FONTS:
        symbols = title_charset if scope == "titles" else charset
        source = font_dir / weight_dir / file_name
        if not source.is_file():
            raise SystemExit(f"缺少字体文件: {source}")
        output = out_dir / f"{name}.c"
        ranges, glyph_count = build_ranges(symbols)
        command = [
            args.node, str(conv),
            "--font", str(source),
            "--range", ranges,
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--no-compress",
            "--lv-font-name", name,
            "--lv-include", "lvgl.h",
            "--output", str(output),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise SystemExit(f"生成 {name} 失败")
        # 源字体缺字时 lv_font_conv 只在 stderr 里提醒，必须显式暴露出来。
        for line in (result.stderr or "").splitlines():
            if "warning" in line.lower() or "missing" in line.lower():
                print("  [警告] %s" % line.strip())
        print("%s: %dpx, 请求 %d 个码点, %d 字节 -> %s"
              % (name, size, glyph_count, output.stat().st_size, output.name))


if __name__ == "__main__":
    main()
