#!/usr/bin/env python3
"""把小厨宝的菜谱素材转换为固件可直接使用的 C 资源。

输入（CookLikeHOC 菜谱仓库的本地 checkout）：

    <source>/
        炒菜/*.md            每个 .md 是一道菜，标题为一级标题
        images/*.png|jpg     封面图，文件名与菜名一致

输出（全部生成到仓库内，可被 git 追踪）：

    main/generated/cook_data.h     菜谱表结构与外部声明
    main/generated/cook_data.c     340 道菜的文案表（名称/品类/配料/步骤）
    main/generated/cook_covers.h   封面表结构与外部声明
    main/generated/cook_covers.c   封面偏移表（图片字节在 blob 中的位置）
    main/assets/covers.bin         全部封面拼接成的 JPEG 二进制块
    assets/fonts/cook_chars.txt    生成中文子集字体所需的字符清单

设计取舍：

* 文案以内联字符串字面量写入 C，交给编译器合并到 .rodata 并直接 XIP 执行，
  不额外占用 RAM。
* 封面统一裁剪缩放到 COVER_SIZE，编码为基线 JPEG（非渐进式，带 JFIF APP0
  头），以便 LVGL 内置 tjpgd 解码器按 MCU 行流式解码，RAM 占用只有一个
  4KB 工作缓冲。
* 封面拼接成单个 blob，避免上百个 EMBED_FILES 目标拖慢构建。
"""

import argparse
import io
import json
import re
from collections import Counter
from pathlib import Path

from PIL import Image

# 与 CookLikeHOC 仓库目录一致的品类顺序，决定菜谱在列表中的排列。
CATEGORIES = [
    "主食", "凉拌", "卤菜", "早餐", "汤", "炒菜", "炖菜",
    "炸品", "烤类", "烫菜", "煮锅", "砂锅菜", "蒸菜", "配料", "饮品",
]

# 「炒个啥呢」的随机候选只取真正的菜。配料是酱料与锅底，饮品是瓶装饮料与冲调品，
# 抽到「卤油」「农夫山泉矿泉水」只会让人莫名其妙。这两个品类仍然出现在菜谱列表页里，
# 只是不参与随机推荐。
NON_DISH_CATEGORIES = ("配料", "饮品")

COVER_SIZES = {
    # 键名 -> (宽, 高)：同时用于 C 资源符号后缀与 blob 文件名。
    # large 用于「炒个啥呢」随机推荐页，small 用于详情页缩略图。
    # 两档都在打包阶段烘焙好，避免真机上再做缩放（既能保证页面加载时间可控，
    # 也避免依赖 LVGL 解码 + 缩放的组合行为）。
    "large": (200, 150),
    "small": (144, 108),
}
JPEG_QUALITY = 82

# 章节标题归类规则：命中即把该章节下的列表项收进对应字段。
INGREDIENT_KEYS = ("配料", "原料", "成分", "调料", "配菜")
STEP_KEYS = ("步骤", "制作", "做法")
# 明确忽略的章节（营养数据属于报告内容，不是做菜步骤）。
IGNORE_KEYS = ("营养", "热量")


def parse_sections(text: str):
    """把 Markdown 拆成 {章节标题: [列表项]}。"""
    sections = {}
    current = None
    for raw in text.splitlines():
        line = raw.rstrip()
        heading = re.match(r"^#{2,6}\s*(.+?)\s*$", line)
        if heading:
            current = heading.group(1).strip().rstrip("：:").strip()
            sections.setdefault(current, [])
            continue
        if current is None:
            continue
        item = re.match(r"^\s*[-*+]\s+(.+?)\s*$", line)
        if item:
            sections[current].append(item.group(1).strip())
    return sections


def clean_item(text: str) -> str:
    """去掉 Markdown 强调/链接，压平空白。"""
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.replace("**", "").replace("__", "").replace("`", "")
    text = text.replace("\\", "")
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def split_steps(items):
    """去掉步骤里重复的序号前缀，保留可读的原文顺序。"""
    steps = []
    for item in items:
        text = clean_item(item)
        if not text:
            continue
        # "1. xxx" / "1、xxx" / "1）xxx" 这类前缀在列表里已由序号体现，去掉后更省屏宽。
        text = re.sub(r"^\d+\s*[.、)）]\s*", "", text)
        steps.append(text)
    return steps


def parse_recipe(path: Path):
    text = path.read_text(encoding="utf-8")
    title_match = re.search(r"^#\s+(.+?)\s*$", text, re.MULTILINE)
    if not title_match:
        return None
    name = clean_item(title_match.group(1))

    image_match = re.search(r"!\[[^\]]*\]\(([^)]+)\)", text)
    image_name = None
    if image_match:
        from urllib.parse import unquote
        image_name = Path(unquote(image_match.group(1))).name

    sections = parse_sections(text)
    ingredients, steps = [], []
    for heading, items in sections.items():
        if any(k in heading for k in IGNORE_KEYS):
            continue
        if any(k in heading for k in INGREDIENT_KEYS):
            ingredients.extend(clean_item(i) for i in items)
        elif any(k in heading for k in STEP_KEYS):
            steps.extend(split_steps(items))

    ingredients = [i for i in ingredients if i]
    steps = [s for s in steps if s]
    return {
        "name": name,
        "stem": path.stem,  # 名称重复时用它兜底，见 disambiguate_names
        "image": image_name,
        "ingredients": ingredients,
        "steps": steps,
        "source": str(path),
    }


def disambiguate_names(recipes):
    """上游有少数文件把一级标题写成了同一个名字，这里用文件名把它们区分开。

    CookLikeHOC 里存在标题撞车但内容确实不同的菜：例如
    ``炒菜/农家小炒肉（玉耳版本）.md`` 与 ``炒菜/农家小炒肉（玉耳）.md`` 的一级标题
    都写作「农家小炒肉（玉耳版本）」，但一个用「小炒肉调料」且没有实拍图，另一个用
    「小炒肉调味汁」且带图。照抄标题会让列表出现两行完全一样的条目，而且
    「炒个啥呢」连按两下会看起来像没换菜——状态机避开的是下标，不是名字。

    同一目录内的文件名必然唯一，用它兜底既能区分开，又不需要编造内容。
    """
    counts = Counter(recipe["name"] for recipe in recipes)

    used: set[str] = set()
    renamed = 0
    for recipe in recipes:
        name = recipe["name"]
        stem = recipe["stem"]

        if counts[name] == 1 and name not in used:
            used.add(name)
            continue

        # 标题撞车了。文件名与标题一致的那个是"正主"，保留原标题；
        # 其余的改用文件名——同一目录内文件名唯一，且它本身就是上游的区分依据。
        if stem == name:
            if name in used:
                # 文件名也撞了（同目录内不会发生），交给测试暴露出来。
                print(f"  [警告] 名称重复且无法区分: 「{name}」({recipe['source']})")
                continue
            used.add(name)
            continue

        if stem in used:
            print(f"  [警告] 文件名已被占用，无法区分: 「{name}」({recipe['source']})")
            continue

        print(f"  名称去重: 「{name}」-> 「{stem}」  ({recipe['source']})")
        recipe["name"] = stem
        used.add(stem)
        renamed += 1
    return renamed


def load_recipes(source: Path):
    recipes = []
    for index, category in enumerate(CATEGORIES):
        directory = source / category
        if not directory.is_dir():
            raise SystemExit(f"缺少品类目录: {directory}")
        for path in sorted(directory.glob("*.md")):
            if path.name.upper() == "README.MD":
                continue
            recipe = parse_recipe(path)
            if recipe is None:
                print(f"  跳过（无一级标题）: {path}")
                continue
            recipe["category"] = category
            recipe["category_index"] = index
            recipes.append(recipe)
    disambiguate_names(recipes)
    return recipes


def encode_cover(path: Path, size):
    """按 4:3 居中裁剪并缩放，输出基线 JPEG 字节。"""
    with Image.open(path) as im:
        cover = im.convert("RGB")
        ratio = size[0] / size[1]
        if cover.width / cover.height > ratio:
            width = round(cover.height * ratio)
            left = (cover.width - width) // 2
            cover = cover.crop((left, 0, left + width, cover.height))
        else:
            height = round(cover.width / ratio)
            top = (cover.height - height) // 2
            cover = cover.crop((0, top, cover.width, top + height))
        cover = cover.resize(size, Image.Resampling.LANCZOS)
        buffer = io.BytesIO()
        # 基线 JPEG + JFIF APP0：LVGL 内置 tjpgd 只识别这一种头部。
        cover.save(buffer, format="JPEG", quality=JPEG_QUALITY,
                   optimize=True, progressive=False)
    return buffer.getvalue()


def build_covers(source: Path, recipes, assets_dir: Path):
    """按菜谱顺序生成两档封面，返回 {档位名: (blob 路径, [(偏移, 长度)])}。"""
    images_dir = source / "images"
    available = {p.name: p for p in images_dir.iterdir() if p.is_file()}

    blobs = {name: bytearray() for name in COVER_SIZES}
    entries = {name: [] for name in COVER_SIZES}
    missing = []

    for recipe in recipes:
        image_name = recipe.get("image")
        path = available.get(image_name) if image_name else None
        if path is None:
            missing.append(recipe["name"])
            for name in COVER_SIZES:
                entries[name].append(None)
            continue
        for name, size in COVER_SIZES.items():
            data = encode_cover(path, size)
            entries[name].append((len(blobs[name]), len(data)))
            blobs[name].extend(data)

    assets_dir.mkdir(parents=True, exist_ok=True)
    for name, blob in blobs.items():
        target = assets_dir / f"covers_{name}.bin"
        target.write_bytes(bytes(blob))
        print("封面[%s %dx%d]: %d 张, %d 字节 -> %s"
              % (name, COVER_SIZES[name][0], COVER_SIZES[name][1],
                 sum(1 for e in entries[name] if e), len(blob), target.name))
    if missing:
        print("  无封面菜谱 %d 道，前 5 个: %s" % (len(missing), missing[:5]))
    return entries


def c_string(value: str) -> str:
    """把 UTF-8 文本写成 C 字符串字面量，保留可读的中文。"""
    out = value.replace("\\", "\\\\").replace('"', '\\"')
    out = out.replace("\r", "").replace("\n", "\\n")
    return f'"{out}"'


def build_category_index(recipes):
    """把菜谱表切成若干连续的品类段，顺序沿用菜谱原来的排列（原始目录顺序）。

    之所以能用「首下标 + 条数」而不是一张下标表，是因为 COOK_RECIPES 本身就是按品类
    分组存放的，同类的菜一定挨在一起。这里顺手把这个前提断言掉：一旦数据源未来打乱
    了顺序，生成阶段就会失败，而不是等到真机上列表串味。
    """
    spans = []
    for index, recipe in enumerate(recipes):
        category = recipe["category"]
        if spans and spans[-1]["name"] == category:
            spans[-1]["count"] += 1
            continue
        if any(span["name"] == category for span in spans):
            raise SystemExit(
                f"品类 {category!r} 在菜谱表中不连续，无法用首下标+条数索引；"
                "请改为生成下标表"
            )
        spans.append({"name": category, "first": index, "count": 1})
    return spans


def write_data_c(out_dir: Path, recipes):
    header = out_dir / "cook_data.h"
    source = out_dir / "cook_data.c"

    random_pool = [
        index for index, recipe in enumerate(recipes)
        if recipe["category"] not in NON_DISH_CATEGORIES
    ]
    categories = build_category_index(recipes)

    header.write_text(
        """// 由 tools/build_cook_assets.py 生成，请勿手工修改。
#pragma once

#include <stdint.h>

#define COOK_RECIPE_COUNT %d

// 「炒个啥呢」的随机候选数量。它小于 COOK_RECIPE_COUNT，因为配料与饮品不参与随机。
#define COOK_RANDOM_POOL_COUNT %d

// 一道菜的全部展示文案。字符串指向 .rodata 中的字面量，生命周期覆盖整个应用。
typedef struct {
    const char *name;         // 菜名
    const char *category;     // 品类
    const char *ingredients;  // 配料，逐行以 \\n 分隔
    const char *steps;        // 步骤，逐行以 \\n 分隔
    int16_t cover;            // 封面在 cook_covers.c 中的下标，-1 表示无封面
} cook_recipe_t;

extern const cook_recipe_t COOK_RECIPES[];

// 「炒个啥呢」的随机候选下标，指向 COOK_RECIPES，升序排列。
// 只包含真正的菜，已排除 %s。菜谱列表页仍然展示全部 COOK_RECIPE_COUNT 条。
extern const int16_t COOK_RANDOM_POOL[];

// 一个品类在菜谱表中的连续区间。COOK_RECIPES 按品类分组存放，同类的菜一定挨在一起，
// 所以只要记下首下标与条数就能遍历整个品类，不必再建一张下标表。
// 顺序沿用菜谱原本的排列（也就是 CookLikeHOC 的目录顺序）。
typedef struct {
    const char *name;   // 品类名
    int16_t first;      // 该品类第一道菜在 COOK_RECIPES 中的下标
    int16_t count;      // 该品类的菜谱数
} cook_category_t;

#define COOK_CATEGORY_COUNT %d

extern const cook_category_t COOK_CATEGORIES[];

// 上表的纯下标版本。状态机不认识 cook_category_t，只吃两张 int16 表。
extern const int16_t COOK_CATEGORY_OFFSETS[COOK_CATEGORY_COUNT];
extern const int16_t COOK_CATEGORY_SIZES[COOK_CATEGORY_COUNT];
""" % (len(recipes), len(random_pool), "、".join(NON_DISH_CATEGORIES),
       len(categories)),
        encoding="utf-8",
    )

    lines = [
        "// 由 tools/build_cook_assets.py 生成，请勿手工修改。",
        f"// 菜谱来源: CookLikeHOC ({len(recipes)} 道菜)",
        '#include "cook_data.h"',
        "",
        "const cook_recipe_t COOK_RECIPES[] = {",
    ]
    for recipe in recipes:
        ingredients = "\n".join(recipe["ingredients"])
        steps = "\n".join(recipe["steps"])
        cover = recipe["cover"]
        lines.append(
            "    { %s, %s, %s, %s, %d },"
            % (
                c_string(recipe["name"]),
                c_string(recipe["category"]),
                c_string(ingredients),
                c_string(steps),
                cover,
            )
        )
    lines.append("};")
    lines.append("")

    # 随机候选表：每行 16 个下标，避免出现几千字符的长行。
    lines.append("const int16_t COOK_RANDOM_POOL[COOK_RANDOM_POOL_COUNT] = {")
    for start in range(0, len(random_pool), 16):
        chunk = random_pool[start:start + 16]
        lines.append("    " + ", ".join(str(index) for index in chunk) + ",")
    lines.append("};")
    lines.append("")

    lines.append("const cook_category_t COOK_CATEGORIES[COOK_CATEGORY_COUNT] = {")
    for span in categories:
        lines.append(
            "    { %s, %d, %d },"
            % (c_string(span["name"]), span["first"], span["count"])
        )
    lines.append("};")
    lines.append("")

    # 同样的区间再拆成两张纯下标表，给不认识 cook_category_t 的状态机用：
    # cook_model 因此不必包含 cook_data.h，主机测试里可以喂假数据。
    for table, key in (("OFFSETS", "first"), ("SIZES", "count")):
        values = [str(span[key]) for span in categories]
        lines.append(
            f"const int16_t COOK_CATEGORY_{table}[COOK_CATEGORY_COUNT] = {{")
        for start in range(0, len(values), 16):
            lines.append("    " + ", ".join(values[start:start + 16]) + ",")
        lines.append("};")
        lines.append("")
    source.write_text("\n".join(lines), encoding="utf-8")
    print(f"文案表: {len(recipes)} 道菜, 随机候选 {len(random_pool)} 道, "
          f"品类 {len(categories)} 个 -> {source}")


def write_covers_c(out_dir: Path, packed):
    """packed: {档位名: [(偏移, 长度)]}，下标即菜谱的 cover 字段。"""
    header = out_dir / "cook_covers.h"
    source = out_dir / "cook_covers.c"

    declarations = []
    for name, size in COVER_SIZES.items():
        upper = name.upper()
        declarations.append(
            f"""#define COOK_COVER_{upper}_COUNT {len(packed[name])}
#define COOK_COVER_{upper}_W {size[0]}
#define COOK_COVER_{upper}_H {size[1]}

// {name} 档封面 JPEG 二进制块，由 main/CMakeLists.txt 的 EMBED_FILES 提供。
extern const uint8_t cook_covers_{name}_blob_start[] asm("_binary_covers_{name}_bin_start");
extern const uint8_t cook_covers_{name}_blob_end[] asm("_binary_covers_{name}_bin_end");

// 取第 index 张封面在本档 blob 中的偏移与长度；越界时返回全 0，由调用方跳过绘制。
void cook_cover_{name}_locate(int index, uint32_t *offset, uint32_t *size);
""")

    header.write_text(
        f"""// 由 tools/build_cook_assets.py 生成，请勿手工修改。
#pragma once

#include <stdint.h>

// 菜的封面下标在 cook_data.h 的 cover 字段里，两档封面共用同一个下标。
{"".join(declarations)}""",
        encoding="utf-8",
    )

    blocks = []
    for name, size in COVER_SIZES.items():
        upper = name.upper()
        entries = packed[name]
        offsets = ", ".join(str(o) for o, _ in entries)
        sizes = ", ".join(str(s) for _, s in entries)
        blocks.append(
            f"""static const int32_t COVER_{upper}_OFFSETS[COOK_COVER_{upper}_COUNT] = {{ {offsets} }};
static const uint32_t COVER_{upper}_SIZES[COOK_COVER_{upper}_COUNT] = {{ {sizes} }};

void cook_cover_{name}_locate(int index, uint32_t *offset, uint32_t *size) {{
    if (offset) *offset = 0;
    if (size) *size = 0;
    if (index < 0 || index >= COOK_COVER_{upper}_COUNT) return;
    if (COVER_{upper}_OFFSETS[index] < 0) return;
    if (offset) *offset = (uint32_t)COVER_{upper}_OFFSETS[index];
    if (size) *size = COVER_{upper}_SIZES[index];
}}""")

    source.write_text(
        """// 由 tools/build_cook_assets.py 生成，请勿手工修改。
#include "cook_covers.h"

""" + "\n\n".join(blocks) + "\n",
        encoding="utf-8",
    )
    print("封面索引表: %s -> %s"
          % (", ".join("%s %d 项" % (n, len(packed[n])) for n in packed), source.name))


def write_charset(repo_root: Path, recipes):
    """汇总固件里会出现的中文/半角字符，供字体子集工具使用。"""
    chars = set()
    for recipe in recipes:
        chars |= set(recipe["name"])
        chars |= set(recipe["category"])
        chars |= set("".join(recipe["ingredients"]))
        chars |= set("".join(recipe["steps"]))
    chars |= set(UI_TEXT)
    # 字体不收录换行与制表符。
    chars.discard("\n")
    chars.discard("\r")
    chars.discard("\t")
    charset = "".join(sorted(chars))
    out = repo_root / "assets" / "fonts" / "cook_chars.txt"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(charset, encoding="utf-8")
    cjk = [c for c in charset if "\u4e00" <= c <= "\u9fff"]
    print(f"字符清单: 共 {len(charset)} 个，其中汉字 {len(cjk)} 个 -> {out}")

    # 标题字号的单独清单：26px 子集只收这几条文案的字，不必重复打包整套菜谱字形。
    titles_out = out.parent / "cook_chars_titles.txt"
    titles_out.write_text("".join(sorted(set(UI_TEXT_TITLES))), encoding="utf-8")
    print(f"标题字符清单: {len(set(UI_TEXT_TITLES))} 个 -> {titles_out}")
    return charset


# 26px 标题字体的字形来源。必须逐字覆盖 demo_cook.c 里所有以 cook_font_26 渲染的
# 文案：目前是首页标题、首页两个按钮、随机页标题、设置页标题、关于页标题。
# 新增 26px 文案时同步补充，否则真机上是方块——tests/test_cook_assets.py 会按字号
# 逐条核对，这里漏字会在跑测试时直接报出来。
UI_TEXT_TITLES = "小厨宝炒个啥呢进入菜谱设置关于"


# 界面固定文案。这里必须逐字覆盖 demo_cook.c 里出现的每一条静态文案，改动界面时
# 同步更新，否则新字会在屏幕上显示成缺字方块（编译成功也不代表字形存在）。
# 注意包含 U+00B7 间隔号，随机推荐页用它分隔品类与操作提示。
UI_TEXT = (
    "小厨宝"          # 首页标题
    "炒个啥呢"        # 首页按钮一 / 随机页标题
    "进入菜谱"        # 首页按钮二
    "菜谱"            # 列表页标题
    "菜谱分类"        # 分类页标题
    "共道菜"          # 首页副标题 "共 N 道菜"
    "配料步骤"        # 详情页小节标题
    "暂无配料暂无步骤"  # 详情页某一小节为空时的占位（字形已被上面两行覆盖）
    "短按OK换一道菜"  # 随机页操作提示
    "暂无封面"        # 无实拍图菜谱的占位
    "长按OK返回 短按OK确认 长按上下键翻页 短按上下切换"  # 底部提示条
    "\u00b7"          # 间隔号
    # --- 设置 / 关于 / 息屏页 ---
    "长按OK进入设置"      # 首页提示条：长按进设置页
    "短按上下切换选项"    # 首页与列表页提示条
    "短按OK看做法"        # 随机页：短按进这道菜的做法
    "上下键换一道菜"      # 随机页：上下键换推荐
    "长按OK返回首页"      # 列表页与设置页提示条
    "设置亮度休眠时间关于"  # 设置页标题与三个条目
    "短按OK调整"          # 设置页提示条
    "关闭秒分钟"          # 休眠档位的取值文案
    "休息中按任意键唤醒"  # 息屏页
    "离线参考版本作者数据界面息屏字体"  # 关于页正文
    # 关于页要印作者名与参考项目名，档位要印百分比与秒数；设置页的「关于」项用
    # 一个 > 表示可进入。ASCII 也必须进子集，否则真机上会渲染成空白——缺字检查
    # 只覆盖非 ASCII，这部分全靠这里补齐。
    "codeniu CookLikeHOC leo-radio Noto Sans SC 0123456789.%/>-"
    # --- windmill 吹气转风车 demo 的 HUD 文案 ---
    "吹气转风车监听风力转速级转分"
    "对着麦克风吹气，风车转动越快"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default="../CookLikeHOC",
                        help="CookLikeHOC 仓库根目录")
    parser.add_argument("--repo", default=None, help="ai-passport 仓库根目录")
    parser.add_argument("--report", default=None, help="额外输出统计 JSON")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve() if args.repo else Path(__file__).resolve().parents[1]
    source = Path(args.source)
    if not source.is_absolute():
        source = (repo_root / source).resolve()
    if not source.is_dir():
        raise SystemExit(f"找不到菜谱源目录: {source}")

    recipes = load_recipes(source)
    print(f"解析到 {len(recipes)} 道菜")

    out_dir = repo_root / "main" / "generated"
    out_dir.mkdir(parents=True, exist_ok=True)

    entries = build_covers(source, recipes, repo_root / "main" / "assets")

    # 无封面的菜谱不入表：封面表只保留真实存在的图片，避免把空数据塞进固件。
    packed = {name: [] for name in COVER_SIZES}
    for index, recipe in enumerate(recipes):
        if entries["large"][index] is None:
            recipe["cover"] = -1
            continue
        recipe["cover"] = len(packed["large"])
        for name in COVER_SIZES:
            packed[name].append(entries[name][index])

    write_data_c(out_dir, recipes)
    write_covers_c(out_dir, packed)
    write_charset(repo_root, recipes)

    if args.report:
        Path(args.report).write_text(json.dumps({
            "recipes": len(recipes),
            "with_cover": sum(1 for r in recipes if r["cover"] >= 0),
            "random_pool": sum(1 for r in recipes if r["category"] not in NON_DISH_CATEGORIES),
            "categories": {c: sum(1 for r in recipes if r["category"] == c) for c in CATEGORIES},
        }, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
