// tests/test_cook_model.c —— 「小厨宝」页面与按键状态机的主机测试。
#include <assert.h>
#include <stdbool.h>

#include "cook_model.h"

#define RECIPES 340

static cook_model_t fresh(void) {
    cook_model_t model;
    cook_model_init(&model, RECIPES, 20240918u);
    return model;
}

// 三个假品类：0 -> [0,10)，1 -> [10,30)，2 -> [30,340)。区间刻意不等长，
// 这样"分类内位置"与"全局下标"一旦混用，断言立刻就能看出来。
static const int16_t k_cat_offsets[3] = { 0, 10, 30 };
static const int16_t k_cat_sizes[3] = { 10, 20, RECIPES - 30 };

static cook_model_t fresh_categorized(void) {
    cook_model_t model = fresh();
    cook_model_set_categories(&model, k_cat_offsets, k_cat_sizes, 3);
    return model;
}

static void test_home_selection_wraps(void) {
    cook_model_t model = fresh();
    assert(model.page == COOK_PAGE_HOME);
    assert(model.home_selected == 0);

    // 两个按钮之间循环切换：上键从第一项回到第二项。
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.home_selected == 1);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.home_selected == 0);

    // 首页是根页面，没有"上一级"可回，长按确定键改作进入设置页。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_SETTINGS);
}

static void test_home_entries(void) {
    cook_model_t model = fresh();
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_RANDOM);
    assert(model.random_index >= 0 && model.random_index < RECIPES);

    model = fresh();
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    // 「进入菜谱」先到分类页，再按一次才进某个品类的列表。
    assert(model.page == COOK_PAGE_CATEGORY);
    assert(model.cat_selected == 0);
}

static void test_random_rolls_and_opens_detail(void) {
    cook_model_t model = fresh();
    cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK);
    assert(model.page == COOK_PAGE_RANDOM);

    // 上下键换一道菜，且必须换到别的菜，否则用户会以为按键失灵。
    for (int i = 0; i < 50; i++) {
        const int previous = model.random_index;
        const cook_ev_t ev = (i % 2 == 0) ? COOK_EV_CLICK : COOK_EV_LONG;
        assert(cook_model_handle(&model, COOK_KEY_DOWN, ev) == COOK_ACTION_REFRESH);
        assert(model.random_index != previous);
        assert(model.random_index >= 0 && model.random_index < RECIPES);
    }

    // 短按确定键进这道菜的做法，并记住来路是随机页。
    const int picked = model.random_index;
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_DETAIL);
    assert(model.detail_index == picked);
    assert(model.detail_from == COOK_PAGE_RANDOM);

    // 从详情页返回时回到随机页，而不是跳去列表页。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_RANDOM);

    // 随机页长按确定键回首页。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_HOME);
}

static void test_random_covers_whole_range(void) {
    // 随机序列要能覆盖整个下标空间，不能只在小范围里打转。
    cook_model_t model = fresh();
    bool seen[RECIPES] = {false};
    int distinct = 0;
    for (int i = 0; i < 20000 && distinct < RECIPES; i++) {
        const int index = cook_model_pick_random(&model);
        assert(index >= 0 && index < RECIPES);
        if (!seen[index]) {
            seen[index] = true;
            distinct++;
        }
    }
    assert(distinct == RECIPES);
}

static void test_single_recipe_random_is_deterministic(void) {
    cook_model_t model;
    cook_model_init(&model, 1, 7u);
    assert(cook_model_pick_random(&model) == 0);
    assert(cook_model_pick_random(&model) == 0);
}

// --- 随机候选池 -----------------------------------------------------------
// 界面上「炒个啥呢」只该从真正的菜里抽，配料与饮品不参与。这一组用例锁住
// cook_model_set_random_pool 的语义：收窄范围、避开当前这道、坏下标被跳过。

// 与生成表一致的形状：RECIPES 道菜里前 279 道是菜，后 61 道是配料与饮品。
#define POOL_DISHES 279
static int16_t g_pool[POOL_DISHES];
static int16_t g_small_pool[2];

static void build_pool(void) {
    for (int i = 0; i < POOL_DISHES; i++) {
        g_pool[i] = (int16_t)i;
    }
}

static void test_random_pool_restricts_and_covers(void) {
    build_pool();
    cook_model_t model;
    cook_model_init(&model, RECIPES, 20240918u);
    cook_model_set_random_pool(&model, g_pool, POOL_DISHES);

    bool seen[POOL_DISHES] = {false};
    int distinct = 0;
    for (int i = 0; i < 20000 && distinct < POOL_DISHES; i++) {
        const int index = cook_model_pick_random(&model);
        // 抽到的必须落在池子里：配料与饮品不能出现。
        assert(index >= 0 && index < POOL_DISHES);
        if (!seen[index]) {
            seen[index] = true;
            distinct++;
        }
    }
    // 池子里的每一道菜都要有机会出现，不能被截断在半路。
    assert(distinct == POOL_DISHES);
}

static void test_random_pool_avoids_current(void) {
    build_pool();
    cook_model_t model;
    cook_model_init(&model, RECIPES, 5u);
    cook_model_set_random_pool(&model, g_pool, POOL_DISHES);
    model.random_index = 0;

    for (int i = 0; i < 500; i++) {
        const int previous = model.random_index;
        const int index = cook_model_pick_random(&model);
        assert(index != previous);
        assert(index < POOL_DISHES);
    }
}

static void test_random_pool_of_two_alternates(void) {
    // 只有两个候选时也必须每次换一道，不能连出同一个。
    g_small_pool[0] = 5;
    g_small_pool[1] = 9;
    cook_model_t model;
    cook_model_init(&model, RECIPES, 11u);
    cook_model_set_random_pool(&model, g_small_pool, 2);
    model.random_index = 5;

    int previous = model.random_index;
    for (int i = 0; i < 20; i++) {
        const int index = cook_model_pick_random(&model);
        assert(index == 5 || index == 9);
        assert(index != previous);
        previous = index;
    }
}

static void test_random_pool_of_one_stays_put(void) {
    // 池子里只剩当前这一道时保持不动，而不是越界或跳到池外。
    g_small_pool[0] = 7;
    g_small_pool[1] = 7;
    cook_model_t model;
    cook_model_init(&model, RECIPES, 3u);
    cook_model_set_random_pool(&model, g_small_pool, 1);
    assert(cook_model_pick_random(&model) == 7);
    assert(cook_model_pick_random(&model) == 7);
}

static void test_random_pool_skips_invalid_entries(void) {
    // 越界与负下标会被跳过；剩下的合法项照常参与抽取。
    // 注意 int16_t 的范围：这里用一个能装进 int16_t 但不存在的菜谱下标。
    static int16_t pool[] = { -1, RECIPES, 4, 6, 32000 };
    cook_model_t model;
    cook_model_init(&model, RECIPES, 13u);
    cook_model_set_random_pool(&model, pool, 5);

    for (int i = 0; i < 200; i++) {
        const int index = cook_model_pick_random(&model);
        assert(index == 4 || index == 6);
    }
}

static void test_random_pool_all_invalid_falls_back(void) {
    // 池子整体非法时退回全表，界面至少还能换菜，而不是卡死或产生坏下标。
    static int16_t pool[] = { -1, -2, RECIPES + 5 };
    cook_model_t model;
    cook_model_init(&model, RECIPES, 17u);
    cook_model_set_random_pool(&model, pool, 3);

    bool seen[RECIPES] = {false};
    for (int i = 0; i < 5000; i++) {
        const int index = cook_model_pick_random(&model);
        assert(index >= 0 && index < RECIPES);
        seen[index] = true;
    }
    // 全表回退必须是覆盖整表的，不能缩成某一段。
    assert(seen[0]);
    assert(seen[RECIPES - 1]);
}

static void test_random_pool_can_be_cleared(void) {
    build_pool();
    cook_model_t model;
    cook_model_init(&model, RECIPES, 19u);
    cook_model_set_random_pool(&model, g_pool, POOL_DISHES);
    assert(model.random_pool != NULL);
    assert(model.random_pool_count == POOL_DISHES);

    // 传 NULL 或非正数量都表示"取消收窄"，回到全表抽取。
    cook_model_set_random_pool(&model, NULL, POOL_DISHES);
    assert(model.random_pool == NULL);
    assert(model.random_pool_count == 0);
    assert(cook_model_pick_random(&model) < RECIPES);

    cook_model_set_random_pool(&model, g_pool, 0);
    assert(model.random_pool == NULL);
    assert(model.random_pool_count == 0);
}

static void test_random_pool_applies_on_home_entry(void) {
    // 首页按确定键进入随机页时，也要走收窄后的池子，而不是全表。
    build_pool();
    cook_model_t model;
    cook_model_init(&model, RECIPES, 23u);
    cook_model_set_random_pool(&model, g_pool, POOL_DISHES);
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_RANDOM);
    assert(model.random_index < POOL_DISHES);
}

static void test_list_page_still_sees_every_recipe(void) {
    // 随机池只影响随机推荐；列表页必须在完整的 340 条上翻到最后一页。
    build_pool();
    cook_model_t model;
    cook_model_init(&model, RECIPES, 29u);
    cook_model_set_random_pool(&model, g_pool, POOL_DISHES);
    model.page = COOK_PAGE_LIST;

    for (int i = 0; i < 1000 && model.list_selected < RECIPES - 1; i++) {
        cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_LONG);
    }
    assert(model.list_selected == RECIPES - 1);
}

static void test_empty_recipe_table_is_safe(void) {
    cook_model_t model;
    cook_model_init(&model, 0, 1u);
    assert(cook_model_pick_random(&model) == 0);
    // 没有菜谱时列表页不能越界，也不能产生滚动动作。
    model.page = COOK_PAGE_LIST;
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_LONG) == COOK_ACTION_NONE);
    assert(model.list_selected == 0);
}

static void test_list_short_press_steps_one(void) {
    cook_model_t model = fresh();
    model.page = COOK_PAGE_LIST;
    model.list_selected = 100;

    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.list_selected == 101);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.list_selected == 100);

    // 首项再按上键就到头了，不循环也不越界。
    model.list_selected = 0;
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.list_selected == 0);

    // 末项同理。
    model.list_selected = RECIPES - 1;
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.list_selected == RECIPES - 1);
}

static void test_list_long_press_pages(void) {
    cook_model_t model = fresh();
    model.page = COOK_PAGE_LIST;

    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_LONG) == COOK_ACTION_REFRESH);
    assert(model.list_selected == COOK_LIST_VISIBLE_ROWS);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_LONG) == COOK_ACTION_REFRESH);
    assert(model.list_selected == 0);

    // 翻页在两端夹紧，不溢出。
    model.list_selected = RECIPES - 1;
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_LONG) == COOK_ACTION_NONE);
    assert(model.list_selected == RECIPES - 1);
}

static void test_list_enter_detail_and_back(void) {
    cook_model_t model = fresh();
    model.page = COOK_PAGE_LIST;
    model.list_selected = 42;

    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_DETAIL);
    assert(model.detail_index == 42);
    assert(model.detail_from == COOK_PAGE_LIST);

    // 短按上下键交给界面层翻正文页，状态机不变。
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.detail_sub == COOK_DETAIL_SUB_COVER);

    // 长按上下键整页翻子页：封面 -> 配料 -> 步骤，两端停住。
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.detail_sub == COOK_DETAIL_SUB_INGREDIENT);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.detail_sub == COOK_DETAIL_SUB_STEP);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_LONG) == COOK_ACTION_NONE);
    assert(model.detail_sub == COOK_DETAIL_SUB_STEP);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.detail_sub == COOK_DETAIL_SUB_INGREDIENT);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.detail_sub == COOK_DETAIL_SUB_COVER);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_LONG) == COOK_ACTION_NONE);
    assert(model.detail_sub == COOK_DETAIL_SUB_COVER);

    // 短按与长按确定键都返回来路页面，并且仍停在原来那一道菜上。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_LIST);
    assert(model.list_selected == 42);

    // 列表页长按确定键回分类页，而不是首页——从哪个品类进来就退回哪里。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_CATEGORY);
    // 分类页再长按一次才回首页。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_HOME);
}

// 列表分页：本页内逐项移动，跨过页边界才换页。
static void test_list_paging_moves_then_turns_page(void) {
    cook_model_t model = fresh();
    model.page = COOK_PAGE_LIST;

    // 第 0 页内一路往下：0..8 都是同页内移动，窗口不动。
    for (int expected = 1; expected <= 8; expected++) {
        assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK)
               == COOK_ACTION_REFRESH);
        assert(model.list_selected == expected);
        assert(cook_list_window(model.list_selected, COOK_LIST_VISIBLE_ROWS, RECIPES) == 0);
    }

    // 选中末项后再按一下：翻到下一页，并回到该页顶部。
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.list_selected == 9);
    assert(cook_list_window(model.list_selected, COOK_LIST_VISIBLE_ROWS, RECIPES) == 9);

    // 反向：页首再按上键回到上一页的末项。
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.list_selected == 8);

    // 最后一页只有 340 - 37*9 = 7 项，翻不出去。
    model.list_selected = RECIPES - 1;
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.list_selected == RECIPES - 1);
}

static void test_list_window(void) {
    // 一屏装得下时窗口固定从头开始。
    assert(cook_list_window(0, 9, 5) == 0);

    // 按页对齐：第 11 页（下标 99..107）的首行是 99。
    assert(cook_list_window(100, 9, 340) == 99);
    assert(cook_list_window(99, 9, 340) == 99);
    assert(cook_list_window(107, 9, 340) == 99);
    assert(cook_list_window(108, 9, 340) == 108);

    // 两端夹紧。
    assert(cook_list_window(0, 9, 340) == 0);
    assert(cook_list_window(8, 9, 340) == 0);
    assert(cook_list_window(339, 9, 340) == 333);

    // 越界输入被收敛。
    assert(cook_list_window(-5, 9, 340) == 0);
    assert(cook_list_window(9999, 9, 340) == 333);

    // 每个选中项都必须落在窗口内，否则界面上会看不到当前光标。
    for (int selected = 0; selected < 340; selected++) {
        const int first = cook_list_window(selected, COOK_LIST_VISIBLE_ROWS, 340);
        assert(first <= selected);
        assert(selected < first + COOK_LIST_VISIBLE_ROWS);
    }

    // 空表与非法行数不产生负数下标。
    assert(cook_list_window(3, 9, 0) == 0);
    assert(cook_list_window(3, 0, 340) == 0);
}

// --- 设置页与关于页 -------------------------------------------------------

// --- 分类页 ---------------------------------------------------------------

static void test_category_moves_like_the_list(void) {
    // 分类页与列表页共用同一套分页规则，这里把几条关键性质各验一遍。
    cook_model_t model = fresh_categorized();
    model.page = COOK_PAGE_CATEGORY;

    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.cat_selected == 1);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.cat_selected == 2);
    // 末项再往下不动。
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.cat_selected == 2);
    // 首项再往上也不动。
    model.cat_selected = 0;
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.cat_selected == 0);
}

static void test_category_opens_its_own_list(void) {
    cook_model_t model = fresh_categorized();
    model.page = COOK_PAGE_CATEGORY;
    model.cat_selected = 1;

    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_LIST);
    assert(model.list_category == 1);
    assert(model.list_selected == 0);

    // 列表页的条目数就是该品类的菜谱数，不再是全表。
    assert(cook_model_list_count(&model) == 20);
    assert(cook_model_list_recipe(&model, 0) == 10);
    assert(cook_model_list_recipe(&model, 19) == 29);
    // 越界位置被夹住，不会读到别的品类里去。
    assert(cook_model_list_recipe(&model, 999) == 29);
    assert(cook_model_list_recipe(&model, -5) == 10);

    // 在列表页里走到头只能走到本品类最后一道菜。
    for (int i = 0; i < 100 && model.list_selected < 19; i++) {
        cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK);
    }
    assert(model.list_selected == 19);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
}

static void test_detail_index_is_global_within_category(void) {
    // 详情页拿的是全局下标，所以从第二个品类的第 3 道菜进去，看到的是第 12 道菜。
    cook_model_t model = fresh_categorized();
    model.page = COOK_PAGE_LIST;
    model.list_category = 1;
    model.list_selected = 2;

    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_DETAIL);
    assert(model.detail_index == 12);

    // 返回列表页后仍停在同一分类的同一位置。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_LIST);
    assert(model.list_category == 1);
    assert(model.list_selected == 2);
}

static void test_without_categories_the_list_is_the_whole_table(void) {
    // 没设分类表（主机测试的默认路径）时，列表页退化成遍历整表，行为与改动前一致。
    cook_model_t model = fresh();
    assert(model.cat_count == 0);
    assert(cook_model_list_count(&model) == RECIPES);
    assert(cook_model_list_recipe(&model, 42) == 42);

    model.page = COOK_PAGE_LIST;
    model.list_selected = 42;
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.detail_index == 42);
}

static void test_settings_navigation(void) {
    cook_model_t model = fresh();
    // 首页长按确定键进设置页，默认停在亮度上。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_SETTINGS);
    assert(model.set_selected == COOK_SET_BRIGHTNESS);

    // 上下键在三个条目间循环。
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.set_selected == COOK_SET_TIMEOUT);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.set_selected == COOK_SET_ABOUT);
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.set_selected == COOK_SET_BRIGHTNESS);
    assert(cook_model_handle(&model, COOK_KEY_UP, COOK_EV_CLICK) == COOK_ACTION_REFRESH);
    assert(model.set_selected == COOK_SET_ABOUT);

    // 短按确定键只报告"确认了哪一项"，具体动作交给界面层（那里才有 NVS 与背光）。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_CONFIRM);

    // 长按确定键回首页。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_HOME);
}

static void test_about_returns_to_settings(void) {
    cook_model_t model = fresh();
    model.page = COOK_PAGE_ABOUT;

    // 上下键交给界面滚动正文。
    assert(cook_model_handle(&model, COOK_KEY_DOWN, COOK_EV_CLICK) == COOK_ACTION_NONE);
    assert(model.page == COOK_PAGE_ABOUT);

    // 长短按确定键都回设置页：这一页没有别的地方可去。
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_CLICK) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_SETTINGS);
    model.page = COOK_PAGE_ABOUT;
    assert(cook_model_handle(&model, COOK_KEY_OK, COOK_EV_LONG) == COOK_ACTION_REBUILD);
    assert(model.page == COOK_PAGE_SETTINGS);
}

// --- 息屏 -----------------------------------------------------------------

static void test_sleep_phase_boundaries(void) {
    // 关掉休眠就永远不熄屏，连关屏那一级都不会到。
    for (uint32_t idle = 0; idle < 100000; idle += 997) {
        assert(cook_sleep_phase_for(idle, 0, 5) == COOK_SLEEP_AWAKE);
    }

    // 到点前一直醒着；到点关屏；再过 5 秒才进低功耗。中间不再有息屏页。
    assert(cook_sleep_phase_for(0, 60, 5) == COOK_SLEEP_AWAKE);
    assert(cook_sleep_phase_for(59, 60, 5) == COOK_SLEEP_AWAKE);
    assert(cook_sleep_phase_for(60, 60, 5) == COOK_SLEEP_BLANK);
    assert(cook_sleep_phase_for(64, 60, 5) == COOK_SLEEP_BLANK);
    assert(cook_sleep_phase_for(65, 60, 5) == COOK_SLEEP_LIGHT);
    assert(cook_sleep_phase_for(100000, 60, 5) == COOK_SLEEP_LIGHT);

    // 关屏窗口为 0 时到点直接进低功耗：判定式不能写死"一定要经过关屏"。
    assert(cook_sleep_phase_for(60, 60, 0) == COOK_SLEEP_LIGHT);

    // 阶段必须单调：时间只会往前走，不该出现关屏后自己又亮回来。
    cook_sleep_phase_t previous = COOK_SLEEP_AWAKE;
    for (uint32_t idle = 0; idle < 200; idle++) {
        const cook_sleep_phase_t phase = cook_sleep_phase_for(idle, 60, 5);
        assert(phase >= previous);
        previous = phase;
    }
}

int main(void) {
    test_home_selection_wraps();
    test_home_entries();
    test_random_rolls_and_opens_detail();
    test_random_covers_whole_range();
    test_single_recipe_random_is_deterministic();
    test_empty_recipe_table_is_safe();
    test_random_pool_restricts_and_covers();
    test_random_pool_avoids_current();
    test_random_pool_of_two_alternates();
    test_random_pool_of_one_stays_put();
    test_random_pool_skips_invalid_entries();
    test_random_pool_all_invalid_falls_back();
    test_random_pool_can_be_cleared();
    test_random_pool_applies_on_home_entry();
    test_list_page_still_sees_every_recipe();
    test_list_short_press_steps_one();
    test_list_long_press_pages();
    test_list_paging_moves_then_turns_page();
    test_list_enter_detail_and_back();
    test_list_window();
    test_category_moves_like_the_list();
    test_category_opens_its_own_list();
    test_detail_index_is_global_within_category();
    test_without_categories_the_list_is_the_whole_table();
    test_settings_navigation();
    test_about_returns_to_settings();
    test_sleep_phase_boundaries();
    return 0;
}
