// main/cook_model.c —— 见 cook_model.h 的说明。
#include "cook_model.h"

// 随机池可以为空，因此会用到 NULL。不能指望 stdint.h 顺带把它带进来：
// MinGW 的 stdint.h 会间接引入 stddef.h，主机测试因此发现不了漏包含，
// 但 ESP-IDF 的 newlib 不会，编译会直接报 'NULL' undeclared。
#include <stddef.h>

static uint32_t cook_next_rng(cook_model_t *model) {
    uint32_t x = model->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0) {
        x = 0x9E3779B9u;  // xorshift 不允许全零状态，退化时换一个非零种子。
    }
    model->rng = x;
    return x;
}

// 在有界的候选池里抽一个，并避开当前这道：连点两次不该停在同一道菜上。
// 池子只有几百项，两次线性遍历就够了，不需要在栈上开临时数组——抽取发生在
// 输入任务里，那里的栈只有几 KB。
// 返回选中的菜谱下标；候选池里没有可用项时返回 -1，由调用方退回全表。
static int cook_pick_from_pool(cook_model_t *model) {
    int valid = 0;
    bool has_current = false;
    for (int i = 0; i < model->random_pool_count; i++) {
        const int index = model->random_pool[i];
        if (index < 0 || index >= model->recipe_count) {
            continue;
        }
        valid++;
        if (index == model->random_index) {
            has_current = true;
        }
    }

    // 去掉当前这道之后就没有候选了（池子只有一项，或者那一项正是当前这道）。
    const int span = valid - (has_current ? 1 : 0);
    if (span <= 0) {
        if (valid == 0) {
            return -1;
        }
        // 只剩当前这一道可选，保持不动。
        return model->random_index >= 0 && model->random_index < model->recipe_count
                   ? model->random_index : -1;
    }

    const int target = (int)(cook_next_rng(model) % (uint32_t)span);
    int rank = 0;
    for (int i = 0; i < model->random_pool_count; i++) {
        const int index = model->random_pool[i];
        if (index < 0 || index >= model->recipe_count) {
            continue;
        }
        if (has_current && index == model->random_index) {
            continue;
        }
        if (rank == target) {
            return index;
        }
        rank++;
    }
    return -1;  // 上面的计数与这里不一致才会走到，属于池子在遍历中被改写。
}

int cook_model_pick_random(cook_model_t *model) {
    if (model->recipe_count <= 0) {
        model->random_index = 0;
        return 0;
    }

    if (model->random_pool != NULL && model->random_pool_count > 0) {
        const int picked = cook_pick_from_pool(model);
        if (picked >= 0) {
            model->random_index = picked;
            return picked;
        }
        // 池子整体非法时退回全表，至少保证界面还能换菜。
    }

    if (model->recipe_count == 1) {
        model->random_index = 0;
        return 0;
    }
    // 在 count-1 个候选中挑一个，再跳过当前这道，保证一次点击就能看到变化。
    const uint32_t span = (uint32_t)(model->recipe_count - 1);
    int index = (int)(cook_next_rng(model) % span);
    if (index >= model->random_index) {
        index += 1;
    }
    model->random_index = index;
    return index;
}

int cook_list_window(int selected, int visible_rows, int count) {
    if (count <= 0 || visible_rows <= 0) {
        return 0;
    }
    if (selected < 0) {
        selected = 0;
    }
    if (selected > count - 1) {
        selected = count - 1;
    }
    // 按页对齐：第 n 页就是 [n*rows, n*rows + rows)。选中项在本页内移动时窗口
    // 不动，只有跨页时才整体换页——这正是"选中末项后再按一下翻页"的表现。
    const int page = selected / visible_rows;
    return page * visible_rows;
}

void cook_model_init(cook_model_t *model, int recipe_count, uint32_t seed) {
    model->page = COOK_PAGE_HOME;
    model->detail_from = COOK_PAGE_HOME;
    model->home_selected = 0;
    model->cat_selected = 0;
    model->list_category = 0;
    model->list_selected = 0;
    model->random_index = 0;
    model->detail_index = 0;
    model->detail_sub = COOK_DETAIL_SUB_COVER;
    model->set_selected = COOK_SET_BRIGHTNESS;
    model->recipe_count = recipe_count;
    model->cat_count = 0;
    model->rng = seed == 0 ? 0x1234567u : seed;
    model->random_pool = NULL;
    model->random_pool_count = 0;
    model->cat_offsets = NULL;
    model->cat_sizes = NULL;
}

void cook_model_set_random_pool(cook_model_t *model, const int16_t *pool, int count) {
    model->random_pool = (pool != NULL && count > 0) ? pool : NULL;
    model->random_pool_count = (pool != NULL && count > 0) ? count : 0;
}

void cook_model_set_categories(cook_model_t *model, const int16_t *offsets,
                               const int16_t *sizes, int count) {
    const bool valid = (offsets != NULL && sizes != NULL && count > 0);
    model->cat_offsets = valid ? offsets : NULL;
    model->cat_sizes = valid ? sizes : NULL;
    model->cat_count = valid ? count : 0;
    // 分类下标是内存里的状态，界面重建时会沿用。品类数变少（换数据源）时越界了
    // 就回到第一个品类，否则列表页会读到一个不存在的区间。
    if (model->cat_selected >= model->cat_count) {
        model->cat_selected = 0;
    }
    if (model->list_category >= model->cat_count) {
        model->list_category = 0;
    }
}

int cook_model_list_count(const cook_model_t *model) {
    if (model->cat_count <= 0 || model->cat_offsets == NULL || model->cat_sizes == NULL) {
        return model->recipe_count;  // 没有分类表时退化成整表
    }
    const int index = model->list_category;
    if (index < 0 || index >= model->cat_count) {
        return 0;
    }
    const int size = model->cat_sizes[index];
    return size > 0 ? size : 0;
}

int cook_model_list_recipe(const cook_model_t *model, int position) {
    const int count = cook_model_list_count(model);
    if (count <= 0) {
        return 0;
    }
    if (position < 0) {
        position = 0;
    }
    if (position > count - 1) {
        position = count - 1;
    }
    if (model->cat_count <= 0 || model->cat_offsets == NULL) {
        return position;
    }
    return model->cat_offsets[model->list_category] + position;
}

static cook_action_t handle_home(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    // 首页是根页面，长按 OK 没有"上一级"可回，改作进入设置页。
    if (key == COOK_KEY_OK && ev == COOK_EV_LONG) {
        model->set_selected = COOK_SET_BRIGHTNESS;
        model->page = COOK_PAGE_SETTINGS;
        return COOK_ACTION_REBUILD;
    }
    if (ev != COOK_EV_CLICK) {
        return COOK_ACTION_NONE;
    }
    switch (key) {
        case COOK_KEY_UP:
            model->home_selected =
                (model->home_selected + COOK_HOME_ITEM_COUNT - 1) % COOK_HOME_ITEM_COUNT;
            return COOK_ACTION_REFRESH;
        case COOK_KEY_DOWN:
            model->home_selected = (model->home_selected + 1) % COOK_HOME_ITEM_COUNT;
            return COOK_ACTION_REFRESH;
        case COOK_KEY_OK:
            if (model->home_selected == 0) {
                model->page = COOK_PAGE_RANDOM;
                cook_model_pick_random(model);
            } else {
                // 「进入菜谱」先到分类页。cat_selected 沿用上次停留的品类，重新进
                // 入时不用从头再翻一遍。
                model->page = COOK_PAGE_CATEGORY;
            }
            return COOK_ACTION_REBUILD;
        default:
            return COOK_ACTION_NONE;
    }
}

// 随机页：上下键换一道菜（整页翻），短按 OK 看这道菜的做法，长按 OK 回首页。
static cook_action_t handle_random(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    if (key == COOK_KEY_OK) {
        if (ev == COOK_EV_LONG) {
            model->page = COOK_PAGE_HOME;
            return COOK_ACTION_REBUILD;
        }
        // 短按 OK 进这道菜的做法。记住来路，详情页长按返回时才能回到随机页
        // 而不是跳去列表页。
        model->detail_from = COOK_PAGE_RANDOM;
        model->detail_index = model->random_index;
        model->detail_sub = COOK_DETAIL_SUB_COVER;
        model->page = COOK_PAGE_DETAIL;
        return COOK_ACTION_REBUILD;
    }
    // 上下键：换一道菜。长按与短按同义——这一页只有一道菜，没有"项"可移动。
    cook_model_pick_random(model);
    return COOK_ACTION_REFRESH;
}

// 分类页与菜谱列表页共用这一条移动规则，所以两页的手感必然一致：
// 短按在本页内逐项移动，长按整页翻；凡是跨过页边界，就直接落到相邻页的首（或末）
// 项，于是选中项永远停在当前可见的那一页里。
static cook_action_t move_paged(int *selected, int count, cook_key_t key,
                                cook_ev_t ev) {
    if (count <= 0) {
        return COOK_ACTION_NONE;
    }
    const int rows = COOK_LIST_VISIBLE_ROWS;
    const int dir = (key == COOK_KEY_UP) ? -1 : 1;
    const int page_first = cook_list_window(*selected, rows, count);
    int next;
    if (ev == COOK_EV_LONG) {
        next = page_first + dir * rows;
    } else {
        next = *selected + dir;
        if (dir > 0 && next >= page_first + rows) {
            next = page_first + rows;  // 越过本页末项 → 下一页页首
        } else if (dir < 0 && next < page_first) {
            next = page_first - 1;     // 越过本页首项 → 上一页页末
        }
    }
    if (next < 0) {
        next = 0;
    }
    if (next > count - 1) {
        // 最后一页不足 rows 行时不会翻出一个空页，而是停在真正的末项。
        next = count - 1;
    }
    if (next == *selected) {
        return COOK_ACTION_NONE;
    }
    *selected = next;
    return COOK_ACTION_REFRESH;
}

static cook_action_t handle_category(cook_model_t *model, cook_key_t key,
                                     cook_ev_t ev) {
    if (key == COOK_KEY_OK) {
        if (ev == COOK_EV_LONG) {
            model->page = COOK_PAGE_HOME;
            return COOK_ACTION_REBUILD;
        }
        // 选中项从本分类的第一道菜开始：换了个品类还停在上次的位置没有意义。
        model->list_category = model->cat_selected;
        model->list_selected = 0;
        model->page = COOK_PAGE_LIST;
        return COOK_ACTION_REBUILD;
    }
    return move_paged(&model->cat_selected, model->cat_count, key, ev);
}

static cook_action_t handle_list(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    if (key == COOK_KEY_OK) {
        if (ev == COOK_EV_LONG) {
            // 列表页的上一级是分类页而不是首页：从哪个品类进来，就退回那个品类。
            model->page = COOK_PAGE_CATEGORY;
            return COOK_ACTION_REBUILD;
        }
        model->detail_from = COOK_PAGE_LIST;
        model->detail_index = cook_model_list_recipe(model, model->list_selected);
        model->detail_sub = COOK_DETAIL_SUB_COVER;
        model->page = COOK_PAGE_DETAIL;
        return COOK_ACTION_REBUILD;
    }
    return move_paged(&model->list_selected, cook_model_list_count(model), key, ev);
}

static cook_action_t handle_detail(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    if (key == COOK_KEY_OK) {
        // 详情页没有更深的层级，长短按都是返回，回到进来时的那一页。
        model->page = model->detail_from;
        return COOK_ACTION_REBUILD;
    }
    if (ev == COOK_EV_LONG) {
        // 长按整页翻子页。
        return cook_model_detail_page(model, key == COOK_KEY_UP);
    }
    // 短按上下由界面滚动正文；滚到尽头后界面会自己调用 cook_model_detail_page。
    return COOK_ACTION_NONE;
}

static cook_action_t handle_settings(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    if (key == COOK_KEY_OK) {
        if (ev == COOK_EV_LONG) {
            model->page = COOK_PAGE_HOME;
            return COOK_ACTION_REBUILD;
        }
        // 具体动作（切档位 / 进关于页）由界面按 set_selected 执行：档位存在 NVS 里，
        // 状态机不该碰这些依赖。
        return COOK_ACTION_CONFIRM;
    }
    if (ev != COOK_EV_CLICK) {
        return COOK_ACTION_NONE;
    }
    const int dir = (key == COOK_KEY_UP) ? -1 : 1;
    model->set_selected =
        (model->set_selected + dir + COOK_SETTINGS_ITEM_COUNT) % COOK_SETTINGS_ITEM_COUNT;
    return COOK_ACTION_REFRESH;
}

static cook_action_t handle_about(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    (void)ev;
    if (key == COOK_KEY_OK) {
        model->page = COOK_PAGE_SETTINGS;
        return COOK_ACTION_REBUILD;
    }
    // 上下键由界面滚动正文。
    return COOK_ACTION_NONE;
}

cook_action_t cook_model_detail_page(cook_model_t *model, bool up) {
    const int next = model->detail_sub + (up ? -1 : 1);
    if (next < 0 || next >= COOK_DETAIL_SUB_COUNT) {
        return COOK_ACTION_NONE;
    }
    model->detail_sub = next;
    return COOK_ACTION_REBUILD;
}

cook_sleep_phase_t cook_sleep_phase_for(uint32_t idle_seconds, uint16_t timeout,
                                        uint16_t light_delay) {
    if (timeout == 0) {
        return COOK_SLEEP_AWAKE;  // 关闭休眠
    }
    if (idle_seconds < (uint32_t)timeout) {
        return COOK_SLEEP_AWAKE;
    }
    if (idle_seconds < (uint32_t)timeout + light_delay) {
        return COOK_SLEEP_BLANK;
    }
    return COOK_SLEEP_LIGHT;
}

cook_action_t cook_model_handle(cook_model_t *model, cook_key_t key, cook_ev_t ev) {
    switch (model->page) {
        case COOK_PAGE_HOME:
            return handle_home(model, key, ev);
        case COOK_PAGE_RANDOM:
            return handle_random(model, key, ev);
        case COOK_PAGE_CATEGORY:
            return handle_category(model, key, ev);
        case COOK_PAGE_LIST:
            return handle_list(model, key, ev);
        case COOK_PAGE_DETAIL:
            return handle_detail(model, key, ev);
        case COOK_PAGE_SETTINGS:
            return handle_settings(model, key, ev);
        case COOK_PAGE_ABOUT:
            return handle_about(model, key, ev);
        default:
            return COOK_ACTION_NONE;
    }
}
