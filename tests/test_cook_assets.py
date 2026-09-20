#!/usr/bin/env python3
"""校验生成的菜谱资源自洽，无需硬件即可运行。

覆盖三类容易悄悄坏掉的问题：

1. 封面索引表与封面 blob 不一致（偏移串位、漏图、长度算错）；
2. 封面 JPEG 头部不是 LVGL 内置 tjpgd 能识别的 JFIF APP0，真机上会静默不显示；
3. 菜谱表里的封面下标越界，会导致详情页读到别的菜或读到垃圾数据。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GENERATED = ROOT / "main" / "generated"
ASSETS = ROOT / "main" / "assets"

# LVGL 的 lv_tjpgd 用这 10 个字节判定"这是能解码的 JPEG"。
JFIF_SIGNATURE = bytes([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46])

COVER_TIERS = ("large", "small")


def parse_int_array(text: str, symbol: str) -> list[int]:
    match = re.search(r"%s\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}" % re.escape(symbol), text)
    if match is None:
        raise AssertionError(f"生成文件里找不到数组 {symbol}")
    body = match.group(1).strip()
    if not body:
        return []
    return [int(token) for token in body.replace("\n", " ").split(",") if token.strip()]


def check_covers(errors: list[str]) -> None:
    source = (GENERATED / "cook_covers.c").read_text(encoding="utf-8")
    counts = {}
    for tier in COVER_TIERS:
        upper = tier.upper()
        blob_path = ASSETS / f"covers_{tier}.bin"
        if not blob_path.is_file():
            errors.append(f"缺少封面二进制块 {blob_path.relative_to(ROOT)}")
            continue
        blob = blob_path.read_bytes()
        offsets = parse_int_array(source, f"COVER_{upper}_OFFSETS")
        sizes = parse_int_array(source, f"COVER_{upper}_SIZES")
        counts[tier] = len(offsets)

        if len(offsets) != len(sizes):
            errors.append(f"{tier}: 偏移表 {len(offsets)} 项与长度表 {len(sizes)} 项不一致")
            continue

        cursor = 0
        for index, (offset, size) in enumerate(zip(offsets, sizes)):
            if offset != cursor:
                errors.append(
                    f"{tier}[{index}]: 偏移 {offset} 与顺序排布的 {cursor} 不符（图片串位或漏项）")
            if size <= 0:
                errors.append(f"{tier}[{index}]: 长度必须为正，实际 {size}")
            cursor = offset + size
            head = blob[offset:offset + len(JFIF_SIGNATURE)]
            if head != JFIF_SIGNATURE:
                errors.append(
                    f"{tier}[{index}]: 不是 LVGL tjpgd 可识别的 JFIF JPEG，头部为 {head.hex()}")
        if cursor != len(blob):
            errors.append(f"{tier}: 索引表合计 {cursor} 字节，与 blob 的 {len(blob)} 字节不符")

    for tier, count in counts.items():
        if not count:
            errors.append(f"{tier}: 封面数量为 0，说明打包阶段就已经失败")
    if len(counts) == len(COVER_TIERS) and len(set(counts.values())) != 1:
        errors.append(f"两档封面数量不一致：{counts}")


def generated_array(source: str, symbol: str) -> str:
    """取出某个数组定义的花括号内容。

    cook_data.c 里现在不止一张表（菜谱表之外还有品类表与品类下标表），按行首花括号
    全文匹配会把品类行也数成菜谱，必须按数组名把范围框住。
    """
    match = re.search(re.escape(symbol) + r"\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
                      source, re.DOTALL)
    return match.group(1) if match else ""


def check_recipes(errors: list[str]) -> None:
    header = (GENERATED / "cook_data.h").read_text(encoding="utf-8")
    count_match = re.search(r"#define COOK_RECIPE_COUNT\s+(\d+)", header)
    if count_match is None:
        errors.append("cook_data.h 缺少 COOK_RECIPE_COUNT")
        return
    declared = int(count_match.group(1))

    source = (GENERATED / "cook_data.c").read_text(encoding="utf-8")
    entries = re.findall(r"^\s*\{\s*\"(?:[^\"\\]|\\.)*\",.*?,\s*(-?\d+)\s*\},\s*$",
                         generated_array(source, "COOK_RECIPES"), re.MULTILINE)
    if len(entries) != declared:
        errors.append(f"cook_data.c 有 {len(entries)} 条菜谱，但头文件声明 {declared} 条")

    cover_count = len(parse_int_array(
        (GENERATED / "cook_covers.c").read_text(encoding="utf-8"), "COVER_LARGE_OFFSETS"))
    without_cover = 0
    out_of_range = 0
    for index_text in entries:
        index = int(index_text)
        if index == -1:
            without_cover += 1
        elif index < 0 or index >= cover_count:
            out_of_range += 1
    if out_of_range:
        errors.append(f"{out_of_range} 道菜的封面下标超出封面表范围（共 {cover_count} 张）")
    if without_cover == len(entries) and entries:
        errors.append("所有菜谱都没有封面，封面打包阶段可能整体失效")

    # 文案表里不能出现空字段，否则详情页会是空白页。
    if re.search(r'\{\s*"",', source):
        errors.append("cook_data.c 里存在空菜名")
    print(f"菜谱 {len(entries)} 道，无封面 {without_cover} 道，封面 {cover_count} 张")


def check_recolor_safety(errors: list[str]) -> None:
    """菜谱正文里不能出现 '#'。

    详情页的配料与步骤用 LVGL 的重着色命令给行首的圆点 / 序号上色，格式是
    "#RRGGBB 文字#"。正文里只要有一个 '#'，LVGL 就会把它当成新的颜色命令开头，
    从那里往后的一段文字会被当成命令吞掉——真机上表现为某一行少几个字，编译和
    字体检查都发现不了。数据在打包时定死，这里守住这条不变式。
    """
    source = (GENERATED / "cook_data.c").read_text(encoding="utf-8")
    offenders = []
    for number, line in enumerate(source.splitlines(), start=1):
        if line.lstrip().startswith("#"):   # #include 这类预处理指令不算
            continue
        if "#" in line:
            offenders.append(number)
    if offenders:
        errors.append(
            f"cook_data.c 第 {offenders[:5]} 行等 {len(offenders)} 处菜谱文本含 '#'，"
            "会破坏详情页的重着色命令；请在打包时替换或转义")
    else:
        print("重着色安全: 菜谱文本中不含 '#'")


def check_unique_names(errors: list[str]) -> None:
    """菜名必须两两不同。

    上游 CookLikeHOC 有两对文件的一级标题写成了同一个名字（内容确实是两道不同的菜）。
    照抄标题会让列表出现两行一模一样的条目，而且「炒个啥呢」连按两下会看起来像没换
    菜——状态机避开的是下标，不是名字。生成脚本会用文件名兜底去重，这里确认它生效。
    """
    source = (GENERATED / "cook_data.c").read_text(encoding="utf-8")
    names = [
        re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), literal)
        for literal in re.findall(r'^\s*\{\s*"((?:[^"\\]|\\.)*)"\s*,',
                                  generated_array(source, "COOK_RECIPES"),
                                  re.MULTILINE)
    ]
    if not names:
        errors.append("cook_data.c 里一道菜都没解析出来")
        return
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        for name in duplicates:
            errors.append(
                f"菜名重复: {name!r} 出现 {names.count(name)} 次；"
                "请在 tools/build_cook_assets.py 的 disambiguate_names 里用文件名兜底")
    else:
        print(f"菜名唯一性: {len(names)} 条菜名两两不同")


def check_random_pool(errors: list[str]) -> None:
    """随机候选表必须正好是"真正的菜"，且与菜谱表逐个对得上。

    「炒个啥呢」只从这张表里抽。如果打包阶段漏了一道菜，或者把配料、饮品也放进来，
    真机上的表现是"抽到卤油"或者"某几道菜永远抽不到"——都不会报错。这里把两件事
    都钉住：排除的品类，以及池子与菜谱表的逐项一致性。
    """
    header = (GENERATED / "cook_data.h").read_text(encoding="utf-8")
    count_match = re.search(r"#define COOK_RANDOM_POOL_COUNT\s+(\d+)", header)
    if count_match is None:
        errors.append("cook_data.h 缺少 COOK_RANDOM_POOL_COUNT")
        return
    declared = int(count_match.group(1))
    if "COOK_RANDOM_POOL" not in header:
        errors.append("cook_data.h 缺少 COOK_RANDOM_POOL 声明")

    source = (GENERATED / "cook_data.c").read_text(encoding="utf-8")
    pool = parse_int_array(source, "COOK_RANDOM_POOL")
    if len(pool) != declared:
        errors.append(f"随机候选表有 {len(pool)} 项，头文件声明 {declared} 项")
        return

    # 升序且互不相同：状态机按顺序遍历做"去掉当前项"的均匀抽取，顺序乱掉会引入偏斜。
    if pool != sorted(set(pool)):
        errors.append("随机候选表必须升序且无重复项")

    # 逐行还原每道菜的品类，按下标对齐 COOK_RECIPES。
    categories = re.findall(r'^\s*\{\s*"(?:[^"\\]|\\.)*",\s*"((?:[^"\\]|\\.)*)"\s*,',
                            generated_array(source, "COOK_RECIPES"), re.MULTILINE)
    if len(categories) <= max(pool, default=-1):
        errors.append("随机候选表的下标超出了菜谱表的范围")
        return

    non_dish = ("配料", "饮品")
    expected = [index for index, category in enumerate(categories)
                if category not in non_dish]
    if pool != expected:
        leaked = [categories[index] for index in pool if index < len(categories)
                  and categories[index] in non_dish]
        missing = sorted(set(expected) - set(pool))
        detail = []
        if leaked:
            detail.append(f"混入了非菜品目 {sorted(set(leaked))}")
        if missing:
            detail.append(f"漏掉了 {len(missing)} 道菜")
        errors.append("随机候选表与菜谱表不一致"
                      + ("：" + "；".join(detail) if detail else ""))
    else:
        print(f"随机候选 {len(pool)} 道菜（已排除 {'、'.join(non_dish)}），"
              f"列表页仍展示 {len(categories)} 条")


def check_categories(errors: list[str]) -> None:
    """品类表必须与菜谱表严格对齐。

    状态机拿到的只是「首下标 + 条数」两个数组，它自己不会校验：一旦区间有缝或者
    重叠，列表页就会串味（翻到别的品类里去）或漏菜，而这种错误在真机上很难一眼看出。
    """
    header = (GENERATED / "cook_data.h").read_text(encoding="utf-8")
    recipe_count = int(re.search(r"#define COOK_RECIPE_COUNT\s+(\d+)", header).group(1))
    declared = int(re.search(r"#define COOK_CATEGORY_COUNT\s+(\d+)", header).group(1))

    source = (GENERATED / "cook_data.c").read_text(encoding="utf-8")
    spans = re.findall(r'^\s*\{\s*"((?:[^"\\]|\\.)*)",\s*(\d+),\s*(\d+)\s*\},',
                       generated_array(source, "COOK_CATEGORIES"), re.MULTILINE)
    if len(spans) != declared:
        errors.append(f"品类表有 {len(spans)} 项，头文件声明 {declared} 项")
        return

    offsets = parse_int_array(source, "COOK_CATEGORY_OFFSETS")
    sizes = parse_int_array(source, "COOK_CATEGORY_SIZES")
    if [int(first) for _, first, _ in spans] != offsets:
        errors.append("COOK_CATEGORIES 与 COOK_CATEGORY_OFFSETS 不一致")
    if [int(count) for _, _, count in spans] != sizes:
        errors.append("COOK_CATEGORIES 与 COOK_CATEGORY_SIZES 不一致")

    names = [name for name, _, _ in spans]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        errors.append(f"品类重复: {duplicates}")

    # 区间首尾相接、从 0 起、正好盖满整张菜谱表。
    cursor = 0
    for name, first, count in spans:
        if int(first) != cursor:
            errors.append(f"品类 {name} 的起始下标是 {first}，应为 {cursor}（区间必须连续）")
            return
        cursor += int(count)
    if cursor != recipe_count:
        errors.append(f"品类区间共覆盖 {cursor} 道菜，菜谱表有 {recipe_count} 道")
        return

    print(f"分类 {declared} 个，连续覆盖 {cursor} 道菜")


def check_charset(errors: list[str]) -> None:
    path = ROOT / "assets" / "fonts" / "cook_chars.txt"
    if not path.is_file():
        errors.append("缺少字符清单 assets/fonts/cook_chars.txt")
        return
    charset = path.read_text(encoding="utf-8")
    if not charset:
        errors.append("字符清单为空，字体子集将不含任何字形")
        return
    if "\n" in charset or "\t" in charset:
        errors.append("字符清单里不应包含换行或制表符")
    cjk = sum(1 for c in charset if "\u4e00" <= c <= "\u9fff")
    if cjk < 100:
        errors.append(f"字符清单只覆盖 {cjk} 个汉字，明显不完整")
    print(f"字符清单 {len(charset)} 个字符，其中汉字 {cjk} 个")


def parse_font_codepoints(path: Path) -> set[int]:
    """读出生成字体实际收录的码点。

    只认 lv_font_conv 会写的两种 cmap：FORMAT0_TINY 是连续区间，SPARSE_TINY 是
    "区间起点 + 偏移表"。读实际表而不是文件头那行 --range 参数，是因为源字体缺字时
    请求的码点不一定真的进了字体——那种情况只有这里看得出。
    """
    text = path.read_text(encoding="utf-8")
    offsets: dict[str, list[int]] = {}
    for match in re.finditer(r"static const uint16_t (\w+)\[\] = \{(.*?)\};", text, re.S):
        offsets[match.group(1)] = [int(token, 16)
                                   for token in re.findall(r"0x[0-9A-Fa-f]+", match.group(2))]

    codepoints: set[int] = set()
    entry = re.compile(r"\.range_start\s*=\s*(\d+).*?\.range_length\s*=\s*(\d+)"
                       r".*?\.unicode_list\s*=\s*(\w+).*?\.type\s*=\s*(\w+)", re.S)
    for match in entry.finditer(text):
        start, length, list_name, kind = (int(match.group(1)), int(match.group(2)),
                                          match.group(3), match.group(4))
        if kind.endswith("FORMAT0_TINY"):
            codepoints |= set(range(start, start + length))
        elif list_name in offsets:
            codepoints |= {start + offset for offset in offsets[list_name]}
    return codepoints


def collect_ui_constants(source: str) -> dict[str, list[str]]:
    """demo_cook.c 里的 static const char *const 文案表（标量与数组都收）。"""
    constants: dict[str, list[str]] = {}
    for match in re.finditer(
            r'static const char \*const (\w+)\s*=\s*"((?:[^"\\\n]|\\.)*)"\s*;', source):
        constants[match.group(1)] = [match.group(2)]
    for match in re.finditer(
            r'static const char \*const (\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}\s*;', source, re.S):
        constants[match.group(1)] = re.findall(r'"((?:[^"\\\n]|\\.)*)"', match.group(2))
    return constants


def decode_literal(literal: str) -> str:
    text = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), literal)
    return text.replace('\\"', '"').replace("\\\\", "\\")


def check_font_coverage(errors: list[str]) -> None:
    """按字号核对字形：每个用 cook_font_NN 渲染的文案，该字号都必须有对应字形。

    UI_TEXT 只保证字符进了"全量清单"，而 26px 标题字体是单独收的一个小子集。两边
    一旦不同步，缺的那几个字在真机上就是方块或乱码，编译、UTF-8、全量清单检查全都
    发现不了——设置页与关于页的标题就是这样漏掉的。这里直接从 demo_cook.c 里找出
    "哪个字号渲染了哪些文案"，再拿去跟生成的字体文件对。
    """
    ui_path = ROOT / "main" / "demo_cook.c"
    if not ui_path.is_file():
        return
    source = ui_path.read_text(encoding="utf-8")
    constants = collect_ui_constants(source)

    per_font: dict[str, set[str]] = {}
    for match in re.finditer(r"&cook_font_(\d+)", source):
        # 取到本条语句结束为止，避免把后面几行的文案算到这个字号头上。
        statement = source[match.end():match.end() + 400].split(";")[0]
        texts: list[str] = []
        texts += re.findall(r'"((?:[^"\\\n]|\\.)*)"', statement)
        for name_match in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:\[\s*\w+\s*\])?", statement):
            texts += constants.get(name_match.group(1), [])
        per_font.setdefault(match.group(1), set()).update(texts)

    for size, texts in sorted(per_font.items()):
        font_path = ROOT / "assets" / "fonts" / f"cook_font_{size}.c"
        if not font_path.is_file():
            errors.append(f"缺少字体文件 {font_path.relative_to(ROOT)}")
            continue
        codepoints = parse_font_codepoints(font_path)
        if not codepoints:
            errors.append(f"cook_font_{size}.c 里解析不出任何字形，文件格式可能变了")
            continue
        missing: dict[str, str] = {}
        for literal in sorted(texts):
            for char in decode_literal(literal):
                if char in "\n\t" or ord(char) in codepoints:
                    continue
                missing.setdefault(char, literal)
        if missing:
            for char, literal in sorted(missing.items()):
                errors.append(
                    "cook_font_%s 缺少字形 %r (U+%04X)，文案 %r 会显示成方块；"
                    "请更新 tools/build_cook_assets.py 的 UI_TEXT_TITLES 并重新生成字体"
                    % (size, char, ord(char), literal))
        else:
            print(f"cook_font_{size}: {len(texts)} 条文案的字形齐全")


def check_ui_text_coverage(errors: list[str]) -> None:
    """界面里的每个非 ASCII 字符都必须出现在字体子集清单里。

    编译成功、UTF-8 正确都不代表字形存在：缺字只会在真机上显示成方块。这里把
    demo_cook.c 的静态文案与 cook_chars.txt 对一遍，把这类问题挡在构建之前。

    ESP_LOG* 的内容走 USB 串口、由电脑端渲染，不消耗屏幕字形，因此整行跳过。
    """
    charset_path = ROOT / "assets" / "fonts" / "cook_chars.txt"
    ui_path = ROOT / "main" / "demo_cook.c"
    if not charset_path.is_file() or not ui_path.is_file():
        return
    charset = set(charset_path.read_text(encoding="utf-8"))

    log_line = re.compile(r"^\s*ESP_LOG[A-Z]\s*\(")
    stripped = "\n".join(
        "" if log_line.match(line) else line
        for line in ui_path.read_text(encoding="utf-8").splitlines()
    )

    literals = re.findall(r'"((?:[^"\\\n]|\\.)*)"', stripped)
    used: dict[str, list[str]] = {}
    for literal in literals:
        # 还原 \uXXXX 与 \" \\ 两类转义，其余保持原样。
        text = re.sub(r"\\u([0-9a-fA-F]{4})",
                      lambda m: chr(int(m.group(1), 16)), literal)
        text = text.replace('\\"', '"').replace("\\\\", "\\")
        for ch in text:
            if ord(ch) < 0x80 or ch in "\n":
                continue
            used.setdefault(ch, []).append(literal)

    missing = sorted(ch for ch in used if ch not in charset)
    if missing:
        for ch in missing:
            sample = used[ch][0]
            errors.append(
                "界面文案用到了字体子集里没有的字符 %r (U+%04X)，例如 %r；"
                "请更新 tools/build_cook_assets.py 的 UI_TEXT 并重新生成字体"
                % (ch, ord(ch), sample))
    else:
        print(f"界面文案覆盖检查: {len(used)} 个非 ASCII 字符全部在字体子集内")


def main() -> int:
    errors: list[str] = []
    check_covers(errors)
    check_recipes(errors)
    check_recolor_safety(errors)
    check_unique_names(errors)
    check_random_pool(errors)
    check_categories(errors)
    check_charset(errors)
    check_ui_text_coverage(errors)
    check_font_coverage(errors)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("Cook assets: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
