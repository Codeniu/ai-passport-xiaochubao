// main/cook_model.h —— 「小厨宝」的页面与按键状态机。
//
// 这一层刻意不依赖 LVGL 与 ESP-IDF：按键语义、列表分页窗口和随机抽取都能在主机上
// 直接跑测试（见 tests/test_cook_model.c）。界面层只负责把状态画出来。
//
// 全局按键约定（各页一致，长按与短按不会抢同一个动作）：
//   短按 OK    = 确认 / 进入下一级
//   长按 OK    = 返回上一级（首页上是进入设置页）
//   短按 上下  = 在当前层级内移动一项（详情页是滚动正文）
//   长按 上下  = 翻一页（详情页是切换子页）
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 与 bsp_button.h 的三键定义保持一致，但这里用独立枚举，避免逻辑层反向依赖 BSP。
typedef enum {
    COOK_KEY_UP = 0,
    COOK_KEY_DOWN,
    COOK_KEY_OK,
} cook_key_t;

typedef enum {
    COOK_EV_CLICK = 0,  // 短按
    COOK_EV_LONG,       // 长按
} cook_ev_t;

typedef enum {
    COOK_PAGE_HOME = 0,  // 首页：炒个啥呢 / 进入菜谱
    COOK_PAGE_RANDOM,    // 随机推荐页
    COOK_PAGE_CATEGORY,  // 分类页：先选品类，再进该品类的菜谱列表
    COOK_PAGE_LIST,      // 菜谱列表页（某一分类内）
    COOK_PAGE_DETAIL,    // 菜谱详情页（三个子页）
    COOK_PAGE_SETTINGS,  // 设置页
    COOK_PAGE_ABOUT,     // 关于页
} cook_page_t;

typedef enum {
    COOK_ACTION_NONE = 0,   // 无需重绘
    COOK_ACTION_REFRESH,    // 页面不变，但内容变化（选中项、滚动位置等）
    COOK_ACTION_REBUILD,    // 切换页面，需要重新构建界面
    COOK_ACTION_CONFIRM,    // 设置页短按 OK：由界面按选中项执行具体动作
} cook_action_t;

// 首页两个按钮的顺序：0 = 炒个啥呢，1 = 进入菜谱。
#define COOK_HOME_ITEM_COUNT 2

// 列表页一屏可见行数，也是分页时的页大小。
#define COOK_LIST_VISIBLE_ROWS 9

// 设置页条目顺序。
#define COOK_SETTINGS_ITEM_COUNT 3
typedef enum {
    COOK_SET_BRIGHTNESS = 0,  // 亮度
    COOK_SET_TIMEOUT,         // 休眠时间
    COOK_SET_ABOUT,           // 关于
} cook_set_item_t;

// 详情页子页：0 = 封面 + 菜名，1 = 配料，2 = 步骤。
// 配料与步骤各占一页，两者都能在页内再整页翻页——合在一页里时，翻到步骤往往要
// 按十几次，分开后一眼就能定位。
#define COOK_DETAIL_SUB_COUNT 3
#define COOK_DETAIL_SUB_COVER 0
#define COOK_DETAIL_SUB_INGREDIENT 1
#define COOK_DETAIL_SUB_STEP 2

typedef struct {
    cook_page_t page;
    cook_page_t detail_from;  // 进入详情页前所在的页面，长按返回时回到这里
    int home_selected;    // 首页选中的按钮
    int cat_selected;     // 分类页选中的品类下标
    int list_category;    // 列表页当前展示的品类下标
    int list_selected;    // 列表页选中项在**当前分类内**的位置，不是全局下标
    int random_index;     // 随机推荐页当前菜谱下标
    int detail_index;     // 详情页菜谱下标（始终是全局下标）
    int detail_sub;       // 详情页子页下标
    int set_selected;     // 设置页选中项
    int recipe_count;     // 菜谱总数（来自生成的资源表）
    int cat_count;        // 品类数
    uint32_t rng;         // xorshift32 状态，便于主机测试复现

    // 随机推荐的候选下标表，升序，指向 COOK_RECIPES。为 NULL 或 count 为 0 时退化为
    // 在 [0, recipe_count) 上均匀抽取。界面把它设成 COOK_RANDOM_POOL，从而跳过配料与
    // 饮品这类不是菜的条目——列表页仍然遍历完整的 recipe_count。
    const int16_t *random_pool;
    int random_pool_count;

    // 品类区间表，两张表长度都是 cat_count：offsets[i] 是品类 i 第一道菜的全局下标，
    // sizes[i] 是该品类的菜谱数。为 NULL 或未设置时，列表页退化为遍历整表。
    const int16_t *cat_offsets;
    const int16_t *cat_sizes;
} cook_model_t;

// 初始化：首页、选中第一个按钮，并用 seed 初始化随机序列。
// 随机池默认为空，即在整个菜谱范围内抽取；需要收窄时再调用
// cook_model_set_random_pool。
void cook_model_init(cook_model_t *model, int recipe_count, uint32_t seed);

// 设置随机候选下标表。pool 必须升序且互不相同，生命周期要覆盖模型的使用期；
// pool 为 NULL 或 count <= 0 时回退到整个菜谱范围。越界的下标会在抽取时被跳过。
void cook_model_set_random_pool(cook_model_t *model, const int16_t *pool, int count);

// 设置品类区间表。两张表长度都是 count：offsets[i] 是品类 i 第一道菜的全局下标，
// sizes[i] 是该品类的菜谱数，区间之间不重叠且覆盖全部菜谱。
// 未调用过时 cat_count 为 0，列表页退化成遍历整张菜谱表（主机测试的默认路径）。
void cook_model_set_categories(cook_model_t *model, const int16_t *offsets,
                               const int16_t *sizes, int count);

// 列表页当前分类的菜谱数。
int cook_model_list_count(const cook_model_t *model);

// 列表页里第 position 项对应的全局菜谱下标。position 越界时夹到合法范围。
int cook_model_list_recipe(const cook_model_t *model, int position);


// 处理一次按键事件，返回界面需要做的动作。
cook_action_t cook_model_handle(cook_model_t *model, cook_key_t key, cook_ev_t ev);

// 抽一道新菜。会尽量避开当前这道，避免连点两次还停在同一道菜上。
int cook_model_pick_random(cook_model_t *model);

// 列表页当前页的首行下标。列表按页切分而不是居中滚动：选中项在本页内逐项移动时
// 窗口不动，跨过页边界才整体换页，于是"选中末项后再按一下"就表现为翻到下一页、
// 并把选中项带回页首。
int cook_list_window(int selected, int visible_rows, int count);

// 详情页翻子页。up 为 true 时往前翻；已经在首/末子页时原地不动。
// 返回 REBUILD 表示子页变了，NONE 表示没变。
cook_action_t cook_model_detail_page(cook_model_t *model, bool up);

// --- 休眠阶段判定 ----------------------------------------------------------
// 放在这一层而不是 cook_settings.c，是因为它同样"不依赖 ESP-IDF 且值得被测"：
// 三级之间的边界（关掉休眠、刚到点、已经进低功耗）很容易写错。
typedef enum {
    COOK_SLEEP_AWAKE = 0,  // 正常显示
    COOK_SLEEP_BLANK,      // 关屏：背光灭 + 面板 Sleep In + 停止刷屏，CPU 仍全速
    COOK_SLEEP_LIGHT,      // 低功耗：ESP32-C3 反复进入 light sleep，只留按键轮询
} cook_sleep_phase_t;

// 距上次操作 idle_seconds 秒后应处于哪个阶段。
// BLANK 与 LIGHT 分开是为了兼顾两件事：刚到点那段还能全速响应（按键零延迟），
// 再没人动才真的去睡。中间不再插一个"息屏页"——那一页要显示时间，而本应用没有
// 可靠的时钟来源，画出来只会是错的。
//   timeout == 0                        -> 永远 AWAKE（关闭休眠）
//   idle < timeout                      -> AWAKE
//   timeout <= idle < timeout + delay   -> BLANK
//   idle >= timeout + delay             -> LIGHT
cook_sleep_phase_t cook_sleep_phase_for(uint32_t idle_seconds, uint16_t timeout,
                                        uint16_t light_delay);
