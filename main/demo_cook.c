// main/demo_cook.c —— 「小厨宝」：完全离线的菜谱参考应用。
//
// 页面结构：
//   首页    两个按钮（炒个啥呢 / 进入菜谱）；长按 OK 进设置页
//   随机页  随机推荐一道菜，上下键换菜，短按 OK 看做法，长按 OK 回首页
//   列表页  340 道菜分页浏览，短按上下逐项移动、长按上下翻页，短按 OK 进详情
//   详情页  三个子页：封面 + 菜名 / 配料 / 步骤；配料与步骤各自还能整页翻页
//   设置页  亮度、休眠时间、关于
//   关于页  项目与来源说明
//   息屏    无操作满设定时长直接关背光；按键唤醒后回到息屏前那一页
//
// 全局布局规则：每页顶部有一条状态栏，电量固定显示在右上角，正文一律从状态栏下方
// 开始排——这样电量永远不会压住标题或菜名。底部提示条只出现在首页、列表页与设置页，
// 随机页、详情页、关于页把这块高度让给内容。
//
// 与 BSP 的边界：按键事件由 main.c 的输入任务派发，LVGL 对象只在持锁时创建/删除；
// 页面状态机在 cook_model.c，设置与休眠在 cook_settings.c，两者都不依赖 LVGL。
#include "demo.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "cook_covers.h"
#include "cook_data.h"
#include "cook_model.h"
#include "cook_settings.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cook";

LV_FONT_DECLARE(cook_font_14);
LV_FONT_DECLARE(cook_font_18);
LV_FONT_DECLARE(cook_font_26);

// ---------------------------------------------------------------------------
// 视觉常量
// ---------------------------------------------------------------------------
#define COOK_SCREEN_W 240
#define COOK_SCREEN_H 320
#define COOK_MARGIN 12

// 顶部状态栏。所有页面的正文都从它下面开始排，电量不再和任何内容抢位置。
#define COOK_STATUS_H 26
#define COOK_CONTENT_TOP 30

// 底部提示条高度：两行 14px 文字 + 上下留白。只有部分页面用。
#define COOK_HINT_H 50
#define COOK_BODY_BOTTOM (COOK_SCREEN_H - COOK_HINT_H)

// 列表页：页眉一行 + 分隔线，下面 9 行正文。
#define COOK_LIST_TOP 52
#define COOK_ROW_H 24
#define COOK_LIST_ROWS COOK_LIST_VISIBLE_ROWS
#define COOK_LIST_BODY_H (COOK_LIST_ROWS * COOK_ROW_H)

// 详情页正文：整页翻页用"滚一屏"实现，底部留出这条高度放页码。
#define COOK_DETAIL_FOOTER 22
#define COOK_DETAIL_VIEW_X 8
#define COOK_DETAIL_VIEW_W (COOK_SCREEN_W - 2 * COOK_DETAIL_VIEW_X)
#define COOK_DETAIL_VIEW_H (COOK_SCREEN_H - COOK_CONTENT_TOP - COOK_DETAIL_FOOTER)
// 正文行宽：视图内边距之后再收一点，右侧不至于贴死。
#define COOK_DETAIL_TEXT_W (COOK_DETAIL_VIEW_W - 2 * COOK_DETAIL_VIEW_X)

#define COOK_C_BG 0xFFF8EF
#define COOK_C_PANEL 0xFFFFFF
#define COOK_C_PRIMARY 0xE8552F
#define COOK_C_PRIMARY_SOFT 0xF7CDBE
#define COOK_C_INK 0x3A2A1E
#define COOK_C_MUTED 0x9A8674
#define COOK_C_BAR 0xF4E7D8
#define COOK_C_LINE 0xE7D8C7
#define COOK_C_DANGER 0xC0392B

// 静态文案。改动这里必须同步更新 tools/build_cook_assets.py 的 UI_TEXT 并重新生成
// 字体子集，否则新字会显示成缺字方块（编译和 UTF-8 都不会报错）。
static const char *const TEXT_HOME_TITLE = "小厨宝";
static const char *const TEXT_HOME_ITEM[COOK_HOME_ITEM_COUNT] = { "炒个啥呢", "进入菜谱" };
static const char *const TEXT_COUNT = "共 %d 道菜";
static const char *const TEXT_CATEGORY_TITLE = "菜谱分类";
static const char *const TEXT_SECTION_INGREDIENT = "配料";
static const char *const TEXT_SECTION_STEP = "步骤";
static const char *const TEXT_NO_INGREDIENT = "暂无配料";
static const char *const TEXT_NO_STEP = "暂无步骤";
static const char *const TEXT_REROLL = "上下键换一道菜";
static const char *const TEXT_SEE_STEPS = "短按OK看做法";
static const char *const TEXT_NO_COVER = "暂无封面";

static const char *const HINT_HOME = "长按OK进入设置 短按OK确认\n短按上下切换选项";
static const char *const HINT_CATEGORY = "长按OK返回首页 短按OK确认\n短按上下切换选项";
static const char *const HINT_LIST = "长按OK返回分类 短按OK确认\n短按上下切换选项";
static const char *const HINT_SETTINGS = "长按OK返回首页 短按OK调整";
static const char *const HINT_ABOUT = "短按OK返回设置页";

static const char *const TEXT_SETTINGS_TITLE = "设置";
static const char *const TEXT_SET_ITEM[COOK_SETTINGS_ITEM_COUNT] = {
    "亮度", "休眠时间", "关于"
};
static const char *const TEXT_SET_ABOUT_VALUE = ">";

static const char *const TEXT_ABOUT_TITLE = "关于";
static const char *const TEXT_ABOUT_LINES[] = {
    "小厨宝",
    "离线菜谱参考",
    "",
    "版本 1.0",
    "作者 codeniu",
    "",
    "菜谱数据 CookLikeHOC",
    "界面与息屏参考 leo-radio",
    "字体 Noto Sans SC",
};

// ---------------------------------------------------------------------------
// 界面状态
// ---------------------------------------------------------------------------
typedef struct {
    cook_model_t model;
    lv_obj_t *screen;
    lv_obj_t *battery_label;
    lv_obj_t *battery_fill;
    lv_obj_t *battery_frame;

    lv_obj_t *home_item[COOK_HOME_ITEM_COUNT];
    lv_obj_t *home_label[COOK_HOME_ITEM_COUNT];

    lv_obj_t *random_cover_frame;
    lv_obj_t *random_name;
    lv_obj_t *random_detail;

    // 分类页与菜谱列表页共用这一组控件：两页不会同时存在，所以没必要各存一份。
    // 页面标题、每行的文本来源、以及行尾是否显示数量由当前页面决定。
    lv_obj_t *list_counter;
    lv_obj_t *list_body;
    lv_obj_t *list_cursor;
    lv_obj_t *list_label[COOK_LIST_ROWS];
    lv_obj_t *list_num[COOK_LIST_ROWS];  // 行尾数量，只有分类页会用到
    int list_first;            // 当前已渲染的窗口首行，用于跳过无谓的重排

    lv_obj_t *detail_view;
    lv_obj_t *detail_page_tag;
    int detail_text_page;   // 配料 / 步骤正文当前在第几页（0 起）
    int detail_text_pages;  // 该子页一共几页，1 表示一屏装得下
    lv_obj_t *about_view;

    lv_obj_t *set_item[COOK_SETTINGS_ITEM_COUNT];
    lv_obj_t *set_name[COOK_SETTINGS_ITEM_COUNT];
    lv_obj_t *set_value[COOK_SETTINGS_ITEM_COUNT];
} cook_ui_t;

static cook_ui_t s_ui;
static lv_timer_t *s_battery_timer;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static lv_obj_t *cook_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

// 无边框、透明、不可滚动的容器。所有布局容器都从这里派生，避免 LVGL 默认的
// 边框、内边距和滚动条干扰像素级排布。
static lv_obj_t *cook_plain(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    return obj;
}

static lv_obj_t *cook_screen_create(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COOK_C_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    return screen;
}

// ---------------------------------------------------------------------------
// 状态栏：电量固定右上角
// ---------------------------------------------------------------------------
// 电池图形：外框 + 按电量填充的内条 + 正极帽。读值 -1 表示电量计不可用，此时
// 显示 -- 且不画填充，而不是编一个数字。
#define COOK_BATT_W 18
#define COOK_BATT_H 10
// 电量文字的宽度：按最长文案 "100%" 给足，避免折行（见 cook_build_status）。
#define COOK_BATT_LABEL_W 46

static void cook_update_battery(void) {
    if (s_ui.battery_label == NULL) {
        return;
    }
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_ui.battery_label, "--");
        lv_obj_set_style_text_color(s_ui.battery_label, lv_color_hex(COOK_C_MUTED), 0);
        lv_obj_set_width(s_ui.battery_fill, 0);
        return;
    }
    lv_label_set_text_fmt(s_ui.battery_label, "%d%%", soc);
    lv_obj_set_style_text_color(
        s_ui.battery_label,
        lv_color_hex(soc < 20 ? COOK_C_DANGER : COOK_C_MUTED), 0);

    // 内条最多占 (宽 - 边框 - 内壁)，最少留 1 像素，空电也看得出是个电池。
    int fill = (COOK_BATT_W - 4) * soc / 100;
    if (fill < 1) {
        fill = 1;
    }
    if (fill > COOK_BATT_W - 4) {
        fill = COOK_BATT_W - 4;
    }
    lv_obj_set_width(s_ui.battery_fill, fill);
    lv_obj_set_style_bg_color(s_ui.battery_fill,
        lv_color_hex(soc < 20 ? COOK_C_DANGER : COOK_C_INK), 0);
}

static void cook_battery_timer_cb(lv_timer_t *timer) {
    (void)timer;
    cook_update_battery();
}

// 状态栏。left 为 NULL 时左侧留空（只显示电量）。left 目前只用于详情页的页码，
// 让「还有下一页」这件事有个稳定落点，不再依赖底部提示条。
static void cook_build_status(lv_obj_t *screen, const char *left) {
    if (left != NULL) {
        lv_obj_t *tag = cook_label(screen, &cook_font_14, COOK_C_MUTED, left);
        lv_obj_set_pos(tag, COOK_MARGIN, 7);
    }

    // 电量整体靠右：文字在左、电池图形在右，两者一起贴住右边距。
    const int frame_x = COOK_SCREEN_W - COOK_MARGIN - COOK_BATT_W - 2;
    s_ui.battery_label = cook_label(screen, &cook_font_14, COOK_C_MUTED, "");
    // 宽度必须按最长文案 "100%" 给足：14px 下三个数字加百分号约 35px，以前只给
    // 34px，标签默认的 WRAP 会把百分号折到第二行，电量看上去就成了两行。
    // 这里按 46px 留余量，并显式关掉换行，宁可裁掉也不许折行。
    lv_obj_set_pos(s_ui.battery_label, frame_x - 5 - COOK_BATT_LABEL_W, 7);
    lv_obj_set_width(s_ui.battery_label, COOK_BATT_LABEL_W);
    lv_label_set_long_mode(s_ui.battery_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_ui.battery_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_ui.battery_frame = cook_plain(screen);
    lv_obj_set_pos(s_ui.battery_frame, frame_x, 9);
    lv_obj_set_size(s_ui.battery_frame, COOK_BATT_W, COOK_BATT_H);
    lv_obj_set_style_radius(s_ui.battery_frame, 2, 0);
    lv_obj_set_style_border_width(s_ui.battery_frame, 1, 0);
    lv_obj_set_style_border_color(s_ui.battery_frame, lv_color_hex(COOK_C_MUTED), 0);

    s_ui.battery_fill = cook_plain(s_ui.battery_frame);
    lv_obj_set_pos(s_ui.battery_fill, 2, 2);
    lv_obj_set_size(s_ui.battery_fill, 0, COOK_BATT_H - 4);
    lv_obj_set_style_bg_color(s_ui.battery_fill, lv_color_hex(COOK_C_INK), 0);
    lv_obj_set_style_bg_opa(s_ui.battery_fill, LV_OPA_COVER, 0);

    // 正极帽：外框右侧一个 2x4 的小方块。
    lv_obj_t *cap = cook_plain(screen);
    lv_obj_set_pos(cap, frame_x + COOK_BATT_W, 12);
    lv_obj_set_size(cap, 2, 4);
    lv_obj_set_style_bg_color(cap, lv_color_hex(COOK_C_MUTED), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);

    cook_update_battery();
}

// 底部提示条。只有首页、列表页、设置页调用，其余页面把高度让给内容。
static void cook_build_hint(lv_obj_t *screen, const char *text) {
    lv_obj_t *bar = cook_plain(screen);
    lv_obj_set_size(bar, COOK_SCREEN_W, COOK_HINT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COOK_C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(COOK_C_LINE), 0);

    lv_obj_t *hint = cook_label(bar, &cook_font_14, COOK_C_MUTED, text);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(hint, 6, 0);
    lv_obj_set_width(hint, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_obj_center(hint);
}

// ---------------------------------------------------------------------------
// 封面
// ---------------------------------------------------------------------------
// 两档尺寸在打包阶段烘焙好，这里只把 blob 里的字节包成 lv_image_dsc_t。
// LVGL 的 tjpgd 解码器直接读描述符的 header.w/h 与 data_size，这两项必须与实际
// 图片尺寸一致，否则会画错尺寸或读越界。
//
// 描述符在几个槽位间轮转：切页/换菜时会重新指向不同的图片，若正好复用同一个描述符，
// 尚未完成的绘制任务可能读到已经改写的指针。多留几个槽位即可彻底避开这个窗口。
#define COOK_DSC_SLOTS 4
static lv_image_dsc_t s_cover_dsc[COOK_DSC_SLOTS];
static int s_cover_dsc_next;

static lv_obj_t *cook_cover_create(lv_obj_t *parent, int recipe_index, bool large,
                                   int x, int y, int w, int h) {
    lv_obj_t *frame = cook_plain(parent);
    lv_obj_set_pos(frame, x, y);
    lv_obj_set_size(frame, w, h);
    lv_obj_set_style_radius(frame, 8, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(COOK_C_PANEL), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(COOK_C_LINE), 0);

    const cook_recipe_t *recipe =
        (recipe_index >= 0 && recipe_index < COOK_RECIPE_COUNT)
            ? &COOK_RECIPES[recipe_index] : NULL;
    uint32_t offset = 0;
    uint32_t size = 0;
    if (recipe != NULL && recipe->cover >= 0) {
        if (large) {
            cook_cover_large_locate(recipe->cover, &offset, &size);
        } else {
            cook_cover_small_locate(recipe->cover, &offset, &size);
        }
    }
    if (size == 0) {
        // 有 160 多道菜没有实拍图，这里给一个明确占位，而不是留一块空白。
        lv_obj_t *placeholder = cook_label(frame, &cook_font_14, COOK_C_MUTED,
                                           TEXT_NO_COVER);
        lv_obj_center(placeholder);
        return frame;
    }

    lv_image_dsc_t *dsc = &s_cover_dsc[s_cover_dsc_next];
    s_cover_dsc_next = (s_cover_dsc_next + 1) % COOK_DSC_SLOTS;
    lv_memzero(dsc, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    // tjpgd 的 decoder_info 会把 cf 覆写为 LV_COLOR_FORMAT_RAW，这里只为可读性填上。
    dsc->header.cf = LV_COLOR_FORMAT_RAW;
    dsc->header.flags = 0;
    dsc->header.w = large ? COOK_COVER_LARGE_W : COOK_COVER_SMALL_W;
    dsc->header.h = large ? COOK_COVER_LARGE_H : COOK_COVER_SMALL_H;
    dsc->header.stride = dsc->header.w * 3;
    dsc->data_size = size;
    dsc->data = (large ? cook_covers_large_blob_start : cook_covers_small_blob_start)
                + offset;

    lv_obj_t *image = lv_image_create(frame);
    lv_image_set_src(image, dsc);
    lv_obj_center(image);
    return frame;
}

// ---------------------------------------------------------------------------
// 首页
// ---------------------------------------------------------------------------
static void cook_refresh_home(void) {
    for (int i = 0; i < COOK_HOME_ITEM_COUNT; i++) {
        const bool selected = (i == s_ui.model.home_selected);
        lv_obj_set_style_bg_color(s_ui.home_item[i],
            lv_color_hex(selected ? COOK_C_PRIMARY : COOK_C_PANEL), 0);
        // cook_plain() 把背景设成了全透明（容器不该挡住页面底色），按钮却是靠
        // 底色区分选中态的。这里必须显式把不透明度补回来，否则橙色底色根本不
        // 画，选中项会变成「白字压在米白页面底色上」，看上去就是一团糊。
        lv_obj_set_style_bg_opa(s_ui.home_item[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_ui.home_item[i],
            lv_color_hex(selected ? COOK_C_PRIMARY : COOK_C_PRIMARY_SOFT), 0);
        lv_obj_set_style_text_color(s_ui.home_label[i],
            lv_color_hex(selected ? COOK_C_PANEL : COOK_C_PRIMARY), 0);
    }
}

static void cook_build_home(void) {
    lv_obj_t *screen = cook_screen_create();
    s_ui.screen = screen;
    cook_build_status(screen, NULL);

    lv_obj_t *title = cook_label(screen, &cook_font_26, COOK_C_PRIMARY,
                                 TEXT_HOME_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *subtitle = cook_label(screen, &cook_font_14, COOK_C_MUTED, "");
    lv_label_set_text_fmt(subtitle, TEXT_COUNT, COOK_RECIPE_COUNT);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 76);

    for (int i = 0; i < COOK_HOME_ITEM_COUNT; i++) {
        lv_obj_t *item = cook_plain(screen);
        lv_obj_set_size(item, COOK_SCREEN_W - 40, 58);
        lv_obj_align(item, LV_ALIGN_TOP_MID, 0, 112 + i * 74);
        lv_obj_set_style_radius(item, 12, 0);
        lv_obj_set_style_border_width(item, 2, 0);

        lv_obj_t *label = cook_label(item, &cook_font_26, COOK_C_PRIMARY,
                                     TEXT_HOME_ITEM[i]);
        lv_obj_center(label);

        s_ui.home_item[i] = item;
        s_ui.home_label[i] = label;
    }

    cook_build_hint(screen, HINT_HOME);
    cook_refresh_home();
}

// ---------------------------------------------------------------------------
// 随机推荐页（无底部提示条，操作提示放在内容区里）
// ---------------------------------------------------------------------------
static void cook_refresh_random(void) {
    const int index = s_ui.model.random_index;
    const cook_recipe_t *recipe =
        (index >= 0 && index < COOK_RECIPE_COUNT) ? &COOK_RECIPES[index] : NULL;

    if (s_ui.random_cover_frame != NULL) {
        lv_obj_delete(s_ui.random_cover_frame);
    }
    s_ui.random_cover_frame = cook_cover_create(
        s_ui.screen, index, true,
        (COOK_SCREEN_W - COOK_COVER_LARGE_W) / 2, 62,
        COOK_COVER_LARGE_W, COOK_COVER_LARGE_H);

    lv_label_set_text(s_ui.random_name, recipe != NULL ? recipe->name : "");
    if (recipe != NULL) {
        char detail[64];
        snprintf(detail, sizeof(detail), "%s \u00b7 %s", recipe->category,
                 TEXT_SEE_STEPS);
        lv_label_set_text(s_ui.random_detail, detail);
    } else {
        lv_label_set_text(s_ui.random_detail, TEXT_SEE_STEPS);
    }
}

static void cook_build_random(void) {
    lv_obj_t *screen = cook_screen_create();
    s_ui.screen = screen;
    cook_build_status(screen, NULL);

    lv_obj_t *title = cook_label(screen, &cook_font_26, COOK_C_PRIMARY,
                                 TEXT_HOME_ITEM[0]);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, COOK_CONTENT_TOP);

    // 封面在 refresh 里创建，这里只登记文字控件。
    s_ui.random_cover_frame = NULL;

    // 菜名最多两行（18px 在 216px 宽下每行约 12 字），因此预留两行高度。
    s_ui.random_name = cook_label(screen, &cook_font_18, COOK_C_INK, "");
    lv_obj_set_width(s_ui.random_name, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_obj_set_style_text_align(s_ui.random_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui.random_name, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ui.random_name, COOK_MARGIN, 222);

    s_ui.random_detail = cook_label(screen, &cook_font_14, COOK_C_PRIMARY, "");
    lv_obj_set_width(s_ui.random_detail, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_obj_set_style_text_align(s_ui.random_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.random_detail, COOK_MARGIN, 272);

    lv_obj_t *hint = cook_label(screen, &cook_font_14, COOK_C_MUTED, TEXT_REROLL);
    lv_obj_set_width(hint, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, COOK_MARGIN, 294);

    cook_refresh_random();
}

// ---------------------------------------------------------------------------
// 菜谱列表页
// ---------------------------------------------------------------------------
// 选中态用一条独立的橙色条表示，而不是给 9 个行容器逐个改底色：整页重排代价高，
// 移动一个小矩形只要重画它自己。文本只在真正换页时才重设。
static void cook_set_y_anim_cb(void *obj, int32_t value) {
    lv_obj_set_y(obj, value);
}

// 翻页时让整个列表区滑一帧：从偏移位置滑回原位，方向跟着翻页方向走。
static void cook_list_animate_page(int dir) {
    if (s_ui.list_body == NULL) {
        return;
    }
    // 上一次动画可能还没跑完，先归位再起新的，避免中途跳变。
    lv_anim_delete(s_ui.list_body, cook_set_y_anim_cb);
    lv_obj_set_y(s_ui.list_body, COOK_LIST_TOP);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_ui.list_body);
    lv_anim_set_values(&anim, COOK_LIST_TOP + dir * 28, COOK_LIST_TOP);
    lv_anim_set_time(&anim, 220);
    lv_anim_set_exec_cb(&anim, cook_set_y_anim_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

// 分类页与列表页共用同一套刷新：取哪一页的选中项与条目数，是两页唯一的差别。
static void cook_refresh_paged(void) {
    const bool category = (s_ui.model.page == COOK_PAGE_CATEGORY);
    const int selected = category ? s_ui.model.cat_selected
                                  : s_ui.model.list_selected;
    const int count = category ? s_ui.model.cat_count
                               : cook_model_list_count(&s_ui.model);
    const int first = cook_list_window(selected, COOK_LIST_ROWS, count);
    const bool page_changed = (first != s_ui.list_first);
    const int previous_first = s_ui.list_first;
    s_ui.list_first = first;

    if (page_changed) {
        // 只有换页才重排文本。同一页内只是选中条在动，文字一个字都没变。
        for (int row = 0; row < COOK_LIST_ROWS; row++) {
            const int index = first + row;
            const bool active = (index < count);
            if (!active) {
                lv_label_set_text(s_ui.list_label[row], "");
                if (s_ui.list_num[row] != NULL) {
                    lv_label_set_text(s_ui.list_num[row], "");
                }
                continue;
            }
            if (category) {
                lv_label_set_text(s_ui.list_label[row],
                                  COOK_CATEGORIES[index].name);
                lv_label_set_text_fmt(s_ui.list_num[row], "%d",
                                      COOK_CATEGORIES[index].count);
            } else {
                lv_label_set_text(s_ui.list_label[row],
                    COOK_RECIPES[cook_model_list_recipe(&s_ui.model, index)].name);
            }
        }
        cook_list_animate_page(first > previous_first ? 1 : -1);
    }

    for (int row = 0; row < COOK_LIST_ROWS; row++) {
        const int index = first + row;
        const bool active = (index < count);
        const bool selected_row = active && (index == selected);
        lv_obj_set_style_text_color(s_ui.list_label[row],
            lv_color_hex(selected_row ? COOK_C_PANEL : COOK_C_INK), 0);
        lv_obj_set_style_text_opa(s_ui.list_label[row],
                                  active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (s_ui.list_num[row] != NULL) {
            // 数量跟着整行一起反色，否则选中时那行会变成橙底压着灰字，看不清。
            lv_obj_set_style_text_color(s_ui.list_num[row],
                lv_color_hex(selected_row ? COOK_C_PANEL : COOK_C_MUTED), 0);
            lv_obj_set_style_text_opa(s_ui.list_num[row],
                                      active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }

    // 选中条：同页内平滑滑过去；换页时直接落位，配合整页滑入动画。
    const int target_y = (selected - first) * COOK_ROW_H + 1;
    if (page_changed || s_ui.list_cursor == NULL) {
        lv_anim_delete(s_ui.list_cursor, cook_set_y_anim_cb);
        lv_obj_set_y(s_ui.list_cursor, target_y);
    } else {
        const int current_y = lv_obj_get_y(s_ui.list_cursor);
        if (current_y != target_y) {
            lv_anim_delete(s_ui.list_cursor, cook_set_y_anim_cb);
            lv_anim_t anim;
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, s_ui.list_cursor);
            lv_anim_set_values(&anim, current_y, target_y);
            lv_anim_set_time(&anim, 130);
            lv_anim_set_exec_cb(&anim, cook_set_y_anim_cb);
            lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
            lv_anim_start(&anim);
        }
    }

    if (count > 0) {
        lv_label_set_text_fmt(s_ui.list_counter, "%d/%d", selected + 1, count);
    } else {
        lv_label_set_text(s_ui.list_counter, "");
    }
}

// 分类页与菜谱列表页的公共骨架：页眉（标题 + 右上计数）、分隔线、九行正文、
// 底部提示条。两页只有标题、行尾数量和提示条文案不同，因此共用同一份布局——
// 共用布局也就自然共用了翻页窗口与选中条动画，两页手感必然一致。
static void cook_build_paged(const char *title, const char *hint, bool with_count) {
    lv_obj_t *screen = cook_screen_create();
    s_ui.screen = screen;
    cook_build_status(screen, NULL);

    // 页眉压得很扁：标题与计数同一行，把纵向空间全留给列表正文。
    lv_obj_t *heading = cook_label(screen, &cook_font_18, COOK_C_PRIMARY, title);
    lv_obj_set_pos(heading, COOK_MARGIN, 26);

    s_ui.list_counter = cook_label(screen, &cook_font_14, COOK_C_MUTED, "");
    lv_obj_set_pos(s_ui.list_counter, COOK_MARGIN, 28);
    lv_obj_set_width(s_ui.list_counter, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_obj_set_style_text_align(s_ui.list_counter, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *header_rule = cook_plain(screen);
    lv_obj_set_size(header_rule, COOK_SCREEN_W - 2 * COOK_MARGIN, 1);
    lv_obj_set_pos(header_rule, COOK_MARGIN, COOK_LIST_TOP - 4);
    lv_obj_set_style_bg_color(header_rule, lv_color_hex(COOK_C_LINE), 0);
    lv_obj_set_style_bg_opa(header_rule, LV_OPA_COVER, 0);

    // 列表区是一个整体，翻页时整块滑动。
    lv_obj_t *body = cook_plain(screen);
    lv_obj_set_pos(body, 8, COOK_LIST_TOP);
    lv_obj_set_size(body, COOK_SCREEN_W - 2 * 8, COOK_LIST_BODY_H);
    s_ui.list_body = body;

    // 选中条先建，才会落在文字下层。
    lv_obj_t *cursor = cook_plain(body);
    lv_obj_set_size(cursor, COOK_SCREEN_W - 2 * 8, COOK_ROW_H - 2);
    lv_obj_set_style_radius(cursor, 6, 0);
    lv_obj_set_style_bg_color(cursor, lv_color_hex(COOK_C_PRIMARY), 0);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    s_ui.list_cursor = cursor;
    s_ui.list_first = -1;

    for (int row = 0; row < COOK_LIST_ROWS; row++) {
        // 每行先建一个和行等高的容器，再把文字在容器里垂直居中。
        // 直接把标签摆在 row * COOK_ROW_H 上是不够的：18px 字体的行高只有 21px，
        // 比 24px 的行矮 3px，文字会整体偏上，配着选中条看尤其明显。
        lv_obj_t *slot = cook_plain(body);
        lv_obj_set_pos(slot, 0, row * COOK_ROW_H);
        lv_obj_set_size(slot, COOK_SCREEN_W - 2 * 8, COOK_ROW_H);

        // 名字过长时用省略号收尾，避免折行把 24px 的行高撑破。
        // 行尾要放数量时把宽度让出来，免得长品名压到数字上。
        lv_obj_t *label = cook_label(slot, &cook_font_18, COOK_C_INK, "");
        lv_obj_set_width(label, with_count ? COOK_SCREEN_W - 2 * 20 - 44
                                           : COOK_SCREEN_W - 2 * 20);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
        s_ui.list_label[row] = label;

        if (with_count) {
            lv_obj_t *num = cook_label(slot, &cook_font_14, COOK_C_MUTED, "");
            lv_obj_align(num, LV_ALIGN_RIGHT_MID, -8, 0);
            s_ui.list_num[row] = num;
        }
    }

    cook_build_hint(screen, hint);
}

static void cook_build_category(void) {
    cook_build_paged(TEXT_CATEGORY_TITLE, HINT_CATEGORY, true);
    cook_refresh_paged();
}

static void cook_build_list(void) {
    // 标题就是当前品类名，右上角只留 n/总数——品类已经在标题里了，不必再写一遍。
    cook_build_paged(COOK_CATEGORIES[s_ui.model.list_category].name, HINT_LIST,
                     false);
    cook_refresh_paged();
}

// ---------------------------------------------------------------------------
// 菜谱详情页（三个子页，无底部提示条）
// ---------------------------------------------------------------------------
static void cook_detail_add_line(lv_obj_t *view, const lv_font_t *font,
                                 uint32_t color, const char *text) {
    lv_obj_t *label = cook_label(view, font, color, text);
    lv_obj_set_width(label, COOK_SCREEN_W - 2 * COOK_MARGIN - 8);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

// 小节标题：橙色小字 + 一条细分隔线。配料与步骤现在是各自独立的子页，标题成了唯一
// 能说明本页主题的元素，因此给它比正文更醒目的颜色。
static void cook_detail_section_title(lv_obj_t *view, const char *text) {
    cook_detail_add_line(view, &cook_font_14, COOK_C_PRIMARY, text);

    lv_obj_t *rule = cook_plain(view);
    lv_obj_set_size(rule, COOK_DETAIL_TEXT_W, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COOK_C_PRIMARY_SOFT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
}

// 一行正文：前缀（圆点 / 序号）橙色、正文墨色。
// 同一行里混色最省的办法是 LVGL 的重着色命令（#RRGGBB 后接文字，再用 # 收尾）——
// 它不增加任何控件，而给每行再套一个前缀 + 正文的双标签容器，在 48 KB 的 LVGL
// 堆上压力太大。代价是正文里不能出现 #，这条约束由 tests/test_cook_assets.py 守住。
static void cook_detail_add_item(lv_obj_t *view, const char *marker,
                                 const char *text, int length) {
    lv_obj_t *label = cook_label(view, &cook_font_18, COOK_C_INK, "");
    lv_obj_set_width(label, COOK_DETAIL_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(label, true);
    // 行距放宽一点：18px 中文挤在 21px 行高里，长步骤会糊成一片。
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_label_set_text_fmt(label, "%s%.*s", marker, length, text);
}

static void cook_detail_add_note(lv_obj_t *view, const char *text) {
    lv_obj_t *label = cook_label(view, &cook_font_18, COOK_C_MUTED, text);
    lv_obj_set_width(label, COOK_DETAIL_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

// 把 "a\nb\nc" 逐行建成独立标签：每行各自换行，长步骤不会把整段挤成一行。
// 这里用 "%.*s" 直接引用只读数据里的行，不需要额外拷贝缓冲区——构建页面发生在
// 输入任务里，栈空间只有几 KB。
static void cook_detail_add_block(lv_obj_t *view, const char *block,
                                  bool numbered) {
    if (block == NULL || *block == '\0') {
        return;
    }
    const char *start = block;
    int index = 0;
    char marker[24];
    for (const char *cursor = block;; cursor++) {
        if (*cursor != '\n' && *cursor != '\0') {
            continue;
        }
        const int length = (int)(cursor - start);
        if (length > 0) {
            if (numbered) {
                // 序号右对齐到两位，多行步骤的正文左边界才能对齐。
                snprintf(marker, sizeof(marker), "#%06X %2d. #",
                         (unsigned)COOK_C_PRIMARY, index + 1);
            } else {
                snprintf(marker, sizeof(marker), "#%06X \u00b7 #",
                         (unsigned)COOK_C_PRIMARY);
            }
            cook_detail_add_item(view, marker, start, length);
            index++;
        }
        start = cursor + 1;
        if (*cursor == '\0') {
            break;
        }
    }
}

// 子页 0：封面 + 菜名。
static void cook_build_detail_cover(lv_obj_t *screen, const cook_recipe_t *recipe,
                                    int index) {
    int cursor_y = COOK_CONTENT_TOP + 14;
    // 没有实拍图时同样占住这块位置，菜名才不会忽上忽下。
    cook_cover_create(screen, (recipe != NULL && recipe->cover >= 0) ? index : -1,
                      true,
                      (COOK_SCREEN_W - COOK_COVER_LARGE_W) / 2, cursor_y,
                      COOK_COVER_LARGE_W, COOK_COVER_LARGE_H);
    cursor_y += COOK_COVER_LARGE_H + 14;

    lv_obj_t *name = cook_label(screen, &cook_font_18, COOK_C_INK,
                                recipe != NULL ? recipe->name : "");
    lv_obj_set_width(name, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(name, COOK_MARGIN, cursor_y);

    if (recipe != NULL) {
        lv_obj_t *cat = cook_label(screen, &cook_font_14, COOK_C_MUTED,
                                   recipe->category);
        lv_obj_set_width(cat, COOK_SCREEN_W - 2 * COOK_MARGIN);
        lv_obj_set_style_text_align(cat, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(cat, LV_ALIGN_TOP_MID, 0, COOK_SCREEN_H - 42);
    }
}

// 正文容器：一屏就是一页，翻页用"滚一整屏"实现，比逐行滚更接近翻书的手感。
static lv_obj_t *cook_detail_text_view(lv_obj_t *screen) {
    lv_obj_t *view = cook_plain(screen);
    lv_obj_set_pos(view, COOK_DETAIL_VIEW_X, COOK_CONTENT_TOP);
    lv_obj_set_size(view, COOK_DETAIL_VIEW_W, COOK_DETAIL_VIEW_H);
    lv_obj_add_flag(view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
    // 惯性滚动会让人失去位置感，整页翻页要的是页码明确。
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(view, 8, 0);
    lv_obj_set_style_pad_all(view, 2, 0);
    s_ui.detail_view = view;
    return view;
}

// 量一次正文总高，换算成本子页一共几页。
// 建页时先量一次是为了把页码画对；翻页前再量一次是因为建页那次发生在屏幕挂上之前，
// 排版有可能还没跑完，量出来会是 1 页。量不出内容时按 1 页处理，最坏结果是这一子页
// 不分页——短按于是直接翻子页，不会卡住。
static void cook_detail_measure_pages(void) {
    s_ui.detail_text_pages = 1;
    if (s_ui.detail_view == NULL) {
        return;
    }
    // 量之前必须先把排版跑一遍，否则子控件坐标还是旧的。
    lv_obj_update_layout(s_ui.detail_view);
    // scroll_top + scroll_bottom 是可滚动的最大距离；滚动位置为 0 时 scroll_top
    // 就是 0，留着完整式子是为了不依赖这个前提。
    const int32_t scrollable = lv_obj_get_scroll_top(s_ui.detail_view)
                               + lv_obj_get_scroll_bottom(s_ui.detail_view);
    const int32_t page_h = lv_obj_get_height(s_ui.detail_view);
    if (scrollable > 0 && page_h > 0) {
        s_ui.detail_text_pages = (int)((scrollable + page_h - 1) / page_h) + 1;
    }
}

static void cook_detail_update_page_tag(void) {
    if (s_ui.detail_page_tag == NULL) {
        return;
    }
    if (s_ui.detail_text_pages <= 1) {
        lv_label_set_text(s_ui.detail_page_tag, "");
        return;
    }
    // 缓冲按 int 的最坏取整（两个 %d 各 11 字节 + 斜杠 + 结束符），别按"两位数
    // 够用"来估——编译器会对可能截断的 snprintf 报错。
    char tag[24];
    snprintf(tag, sizeof(tag), "%d/%d", s_ui.detail_text_page + 1,
             s_ui.detail_text_pages);
    lv_label_set_text(s_ui.detail_page_tag, tag);
}

// 子页 1 / 2：配料 / 步骤，各自整页翻页。
static void cook_build_detail_text(lv_obj_t *screen, const cook_recipe_t *recipe,
                                   bool steps) {
    lv_obj_t *view = cook_detail_text_view(screen);
    cook_detail_section_title(view, steps ? TEXT_SECTION_STEP
                                          : TEXT_SECTION_INGREDIENT);

    const char *block = recipe != NULL
                            ? (steps ? recipe->steps : recipe->ingredients) : NULL;
    if (block != NULL && *block != '\0') {
        cook_detail_add_block(view, block, steps);
    } else {
        cook_detail_add_note(view, steps ? TEXT_NO_STEP : TEXT_NO_INGREDIENT);
    }

    // 页码落在正文底部右侧：状态栏左边那个是子页序号（n/3），这里是正文页码
    // （n/m），两者分工不同，都只由数字与斜杠组成，不新增字形。
    s_ui.detail_page_tag = cook_label(screen, &cook_font_14, COOK_C_MUTED, "");
    lv_obj_set_pos(s_ui.detail_page_tag, COOK_MARGIN,
                   COOK_SCREEN_H - COOK_DETAIL_FOOTER + 2);
    lv_obj_set_width(s_ui.detail_page_tag, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_label_set_long_mode(s_ui.detail_page_tag, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_ui.detail_page_tag, LV_TEXT_ALIGN_RIGHT, 0);

    s_ui.detail_text_page = 0;
    cook_detail_measure_pages();
    cook_detail_update_page_tag();
}

static void cook_build_detail(void) {
    lv_obj_t *screen = cook_screen_create();
    s_ui.screen = screen;

    const int index = s_ui.model.detail_index;
    const cook_recipe_t *recipe =
        (index >= 0 && index < COOK_RECIPE_COUNT) ? &COOK_RECIPES[index] : NULL;

    // 页码放在状态栏左侧：详情页没有底部提示条，「还有下一页」就靠它说明。
    // 缓冲按 int 的最坏取整（两个 %d 加斜杠与结束符最多 24 字节），别按"两位数
    // 够用"来估——编译器会对可能截断的 snprintf 报错。
    char page_tag[24];
    snprintf(page_tag, sizeof(page_tag), "%d/%d", s_ui.model.detail_sub + 1,
             COOK_DETAIL_SUB_COUNT);
    cook_build_status(screen, page_tag);

    if (s_ui.model.detail_sub == COOK_DETAIL_SUB_COVER) {
        cook_build_detail_cover(screen, recipe, index);
        return;
    }
    cook_build_detail_text(screen, recipe,
                           s_ui.model.detail_sub == COOK_DETAIL_SUB_STEP);
}

// 正文整页翻一屏。返回 false 表示这一子页没有更多页可翻，调用方据此改翻子页。
static bool cook_detail_turn_page(bool up) {
    if (s_ui.detail_view == NULL) {
        return false;
    }
    cook_detail_measure_pages();
    if (s_ui.detail_text_pages <= 1) {
        return false;
    }
    if (s_ui.detail_text_page >= s_ui.detail_text_pages) {
        s_ui.detail_text_page = s_ui.detail_text_pages - 1;
    }
    const int next = s_ui.detail_text_page + (up ? -1 : 1);
    if (next < 0 || next >= s_ui.detail_text_pages) {
        return false;
    }
    s_ui.detail_text_page = next;
    lv_obj_scroll_to_y(s_ui.detail_view,
                       next * lv_obj_get_height(s_ui.detail_view), LV_ANIM_ON);
    cook_detail_update_page_tag();
    return true;
}

// ---------------------------------------------------------------------------
// 设置页
// ---------------------------------------------------------------------------
static void cook_refresh_settings(void) {
    for (int i = 0; i < COOK_SETTINGS_ITEM_COUNT; i++) {
        const bool selected = (i == s_ui.model.set_selected);
        lv_obj_set_style_bg_color(s_ui.set_item[i],
            lv_color_hex(selected ? COOK_C_PRIMARY : COOK_C_PANEL), 0);
        lv_obj_set_style_bg_opa(s_ui.set_item[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_ui.set_name[i],
            lv_color_hex(selected ? COOK_C_PANEL : COOK_C_INK), 0);
        lv_obj_set_style_text_color(s_ui.set_value[i],
            lv_color_hex(selected ? COOK_C_PANEL : COOK_C_MUTED), 0);
    }
    lv_label_set_text(s_ui.set_value[COOK_SET_BRIGHTNESS],
                      cook_settings_brightness_text());
    lv_label_set_text(s_ui.set_value[COOK_SET_TIMEOUT],
                      cook_settings_timeout_text());
    lv_label_set_text(s_ui.set_value[COOK_SET_ABOUT], TEXT_SET_ABOUT_VALUE);
}

static void cook_build_settings(void) {
    lv_obj_t *screen = cook_screen_create();
    s_ui.screen = screen;
    cook_build_status(screen, NULL);

    lv_obj_t *title = cook_label(screen, &cook_font_26, COOK_C_PRIMARY,
                                 TEXT_SETTINGS_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, COOK_CONTENT_TOP);

    for (int i = 0; i < COOK_SETTINGS_ITEM_COUNT; i++) {
        lv_obj_t *item = cook_plain(screen);
        lv_obj_set_size(item, COOK_SCREEN_W - 2 * COOK_MARGIN, 44);
        lv_obj_align(item, LV_ALIGN_TOP_MID, 0, 76 + i * 52);
        lv_obj_set_style_radius(item, 10, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(COOK_C_LINE), 0);
        lv_obj_set_style_pad_left(item, 12, 0);
        lv_obj_set_style_pad_right(item, 12, 0);

        lv_obj_t *name = cook_label(item, &cook_font_18, COOK_C_INK,
                                    TEXT_SET_ITEM[i]);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *value = cook_label(item, &cook_font_14, COOK_C_MUTED, "");
        lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);

        s_ui.set_item[i] = item;
        s_ui.set_name[i] = name;
        s_ui.set_value[i] = value;
    }

    cook_build_hint(screen, HINT_SETTINGS);
    cook_refresh_settings();
}

// 设置页短按 OK：按选中项执行。返回 true 表示需要切页面（进入关于页）。
static bool cook_settings_confirm(void) {
    switch (s_ui.model.set_selected) {
        case COOK_SET_BRIGHTNESS:
            cook_settings_cycle_brightness();
            return false;
        case COOK_SET_TIMEOUT:
            cook_settings_cycle_timeout();
            return false;
        case COOK_SET_ABOUT:
        default:
            s_ui.model.page = COOK_PAGE_ABOUT;
            return true;
    }
}

// ---------------------------------------------------------------------------
// 关于页
// ---------------------------------------------------------------------------
static void cook_build_about(void) {
    lv_obj_t *screen = cook_screen_create();
    s_ui.screen = screen;
    cook_build_status(screen, NULL);

    lv_obj_t *title = cook_label(screen, &cook_font_26, COOK_C_PRIMARY,
                                 TEXT_ABOUT_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, COOK_CONTENT_TOP);

    lv_obj_t *view = cook_plain(screen);
    lv_obj_set_pos(view, COOK_MARGIN, 70);
    lv_obj_set_size(view, COOK_SCREEN_W - 2 * COOK_MARGIN, 218);
    lv_obj_add_flag(view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(view, 6, 0);
    s_ui.about_view = view;

    const size_t lines = sizeof(TEXT_ABOUT_LINES) / sizeof(TEXT_ABOUT_LINES[0]);
    for (size_t i = 0; i < lines; i++) {
        cook_detail_add_line(view, &cook_font_14,
                             i == 0 ? COOK_C_PRIMARY : COOK_C_INK,
                             TEXT_ABOUT_LINES[i]);
    }

    lv_obj_t *hint = cook_label(screen, &cook_font_14, COOK_C_MUTED, HINT_ABOUT);
    lv_obj_set_width(hint, COOK_SCREEN_W - 2 * COOK_MARGIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ---------------------------------------------------------------------------
// 页面切换
// ---------------------------------------------------------------------------
static void cook_clear_page_refs(void) {
    s_ui.battery_label = NULL;
    s_ui.battery_fill = NULL;
    s_ui.battery_frame = NULL;
    s_ui.detail_view = NULL;
    s_ui.detail_page_tag = NULL;
    s_ui.detail_text_page = 0;
    s_ui.detail_text_pages = 1;
    s_ui.about_view = NULL;
    s_ui.random_cover_frame = NULL;
    s_ui.random_name = NULL;
    s_ui.random_detail = NULL;
    s_ui.list_counter = NULL;
    s_ui.list_body = NULL;
    s_ui.list_cursor = NULL;
    s_ui.list_first = -1;
    for (int i = 0; i < COOK_LIST_ROWS; i++) {
        s_ui.list_label[i] = NULL;
        s_ui.list_num[i] = NULL;
    }
    for (int i = 0; i < COOK_HOME_ITEM_COUNT; i++) {
        s_ui.home_item[i] = NULL;
        s_ui.home_label[i] = NULL;
    }
    for (int i = 0; i < COOK_SETTINGS_ITEM_COUNT; i++) {
        s_ui.set_item[i] = NULL;
        s_ui.set_name[i] = NULL;
        s_ui.set_value[i] = NULL;
    }
}

static void cook_build_page(cook_page_t page) {
    // 顺序很关键：必须先把新页挂上显示，再销毁旧页。
    // lv_obj_delete() 删掉正在显示的屏幕时会把 disp->act_scr 置为 NULL 并打印
    // "the active screen was deleted"。在这之后的任何一次 invalidate / 刷新都会
    // 解引用那个空指针，表现为按下 OK 的瞬间整机重启、回到首页。
    lv_obj_t *old_screen = s_ui.screen;
    s_ui.screen = NULL;
    cook_clear_page_refs();
    switch (page) {
        case COOK_PAGE_HOME:
            cook_build_home();
            break;
        case COOK_PAGE_RANDOM:
            cook_build_random();
            break;
        case COOK_PAGE_CATEGORY:
            cook_build_category();
            break;
        case COOK_PAGE_LIST:
            cook_build_list();
            break;
        case COOK_PAGE_DETAIL:
            cook_build_detail();
            break;
        case COOK_PAGE_SETTINGS:
            cook_build_settings();
            break;
        case COOK_PAGE_ABOUT:
            cook_build_about();
            break;
        default:
            // 未知页面退回首页，总好过把空指针挂上屏幕。
            cook_build_home();
            break;
    }
    lv_screen_load(s_ui.screen);
    if (old_screen != NULL) {
        lv_obj_delete(old_screen);
    }
}

static void cook_refresh_page(cook_page_t page) {
    switch (page) {
        case COOK_PAGE_HOME:
            cook_refresh_home();
            break;
        case COOK_PAGE_RANDOM:
            cook_refresh_random();
            break;
        case COOK_PAGE_CATEGORY:
        case COOK_PAGE_LIST:
            cook_refresh_paged();
            break;
        case COOK_PAGE_SETTINGS:
            cook_refresh_settings();
            break;
        default:
            break;
    }
}

static cook_key_t cook_map_key(bsp_btn_t btn) {
    switch (btn) {
        case BSP_BTN_UP:
            return COOK_KEY_UP;
        case BSP_BTN_DOWN:
            return COOK_KEY_DOWN;
        default:
            return COOK_KEY_OK;
    }
}

// ---------------------------------------------------------------------------
// 休眠
// ---------------------------------------------------------------------------
// 阶段判定与关屏 / 低功耗的进出都不在这里——它们在 cook_power 的独立任务里按秒跑。
// 界面这一侧只知道一件事：按键来了要先唤醒，唤醒前不执行任何动作。
static void cook_wake_up(void) {
    cook_settings_note_activity();
}

// ---------------------------------------------------------------------------
// demo_entry 接口
// ---------------------------------------------------------------------------
void demo_cook_enter(void) {
    cook_model_init(&s_ui.model, COOK_RECIPE_COUNT, 0x5EED5EEDu);
    // 「炒个啥呢」只从真正的菜里抽：生成表已经把配料与饮品排除掉了。
    // 菜谱列表页不受影响，仍然遍历全部 COOK_RECIPE_COUNT 条。
    cook_model_set_random_pool(&s_ui.model, COOK_RANDOM_POOL, COOK_RANDOM_POOL_COUNT);
    cook_model_set_categories(&s_ui.model, COOK_CATEGORY_OFFSETS,
                              COOK_CATEGORY_SIZES, COOK_CATEGORY_COUNT);
    s_ui.screen = NULL;
    cook_clear_page_refs();
    cook_build_page(COOK_PAGE_HOME);
    if (s_battery_timer == NULL) {
        // 电量变化慢，5 秒刷新一次足够，也避免频繁占用与音频共享的 I2C 总线。
        s_battery_timer = lv_timer_create(cook_battery_timer_cb, 5000, NULL);
    }
    ESP_LOGI(TAG, "小厨宝就绪: %d 道菜(随机候选 %d), %d 个品类, 封面 %d 张",
             COOK_RECIPE_COUNT, COOK_RANDOM_POOL_COUNT, COOK_CATEGORY_COUNT,
             COOK_COVER_LARGE_COUNT);
}

void demo_cook_exit(void) {
    if (s_battery_timer != NULL) {
        lv_timer_delete(s_battery_timer);
        s_battery_timer = NULL;
    }
    if (s_ui.screen != NULL) {
        lv_obj_delete(s_ui.screen);
        s_ui.screen = NULL;
    }
    cook_clear_page_refs();
}

void demo_cook_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    cook_ev_t cook_ev;
    if (ev == BSP_BTN_CLICK) {
        cook_ev = COOK_EV_CLICK;
    } else if (ev == BSP_BTN_LONG) {
        cook_ev = COOK_EV_LONG;
    } else {
        // PRESS 面向需要极低延迟的场景，双击在本应用里没有语义。
        return;
    }

    // 本回调运行在输入任务里，页面状态机自己不会碰 LVGL；LVGL 是非线程安全的，
    // 所有对象操作都必须在 LVGL 锁内完成。
    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "获取 LVGL 锁超时，丢弃本次按键");
        return;
    }

    // 息屏中：第一下按键只用来唤醒，不执行任何动作，免得「唤醒那一下」顺带把
    // 菜谱翻了页。
    if (cook_settings_phase() != COOK_SLEEP_AWAKE) {
        cook_wake_up();
        bsp_lvgl_unlock();
        return;
    }
    cook_settings_note_activity();

    const cook_page_t page = s_ui.model.page;

    // 详情页与关于页的上下键不改变页面状态，只滚正文；滚到尽头才翻页。
    if (btn != BSP_BTN_OK) {
        const bool up = (btn == BSP_BTN_UP);
        if (page == COOK_PAGE_DETAIL) {
            bool turned;
            if (ev == BSP_BTN_LONG) {
                // 长按直接翻子页：封面 / 配料 / 步骤。
                turned = cook_model_detail_page(&s_ui.model, up)
                         == COOK_ACTION_REBUILD;
            } else {
                // 短按先把当前子页的正文整页翻一屏；翻到头了才接着翻子页，
                // 于是读完这一屏再按一下就自然进入下一个小节。
                turned = !cook_detail_turn_page(up)
                         && cook_model_detail_page(&s_ui.model, up)
                                == COOK_ACTION_REBUILD;
            }
            if (turned) {
                cook_build_page(s_ui.model.page);
            }
            bsp_lvgl_unlock();
            return;
        }
        if (page == COOK_PAGE_ABOUT && s_ui.about_view != NULL) {
            const int32_t step = lv_obj_get_height(s_ui.about_view) / 2;
            lv_obj_scroll_to_y(s_ui.about_view,
                               lv_obj_get_scroll_y(s_ui.about_view)
                                   + (up ? -step : step),
                               LV_ANIM_OFF);
            bsp_lvgl_unlock();
            return;
        }
    }

    const cook_action_t action = cook_model_handle(&s_ui.model, cook_map_key(btn),
                                                   cook_ev);
    if (action == COOK_ACTION_REBUILD) {
        cook_build_page(s_ui.model.page);
    } else if (action == COOK_ACTION_REFRESH) {
        cook_refresh_page(page);
    } else if (action == COOK_ACTION_CONFIRM) {
        // 设置页短按 OK：亮度与休眠只是切档位并就地刷新，关于则换页。
        if (cook_settings_confirm()) {
            cook_build_page(s_ui.model.page);
        } else {
            cook_refresh_settings();
        }
    }
    bsp_lvgl_unlock();
}
