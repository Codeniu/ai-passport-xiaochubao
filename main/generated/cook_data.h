// 由 tools/build_cook_assets.py 生成，请勿手工修改。
#pragma once

#include <stdint.h>

#define COOK_RECIPE_COUNT 340

// 「炒个啥呢」的随机候选数量。它小于 COOK_RECIPE_COUNT，因为配料与饮品不参与随机。
#define COOK_RANDOM_POOL_COUNT 279

// 一道菜的全部展示文案。字符串指向 .rodata 中的字面量，生命周期覆盖整个应用。
typedef struct {
    const char *name;         // 菜名
    const char *category;     // 品类
    const char *ingredients;  // 配料，逐行以 \n 分隔
    const char *steps;        // 步骤，逐行以 \n 分隔
    int16_t cover;            // 封面在 cook_covers.c 中的下标，-1 表示无封面
} cook_recipe_t;

extern const cook_recipe_t COOK_RECIPES[];

// 「炒个啥呢」的随机候选下标，指向 COOK_RECIPES，升序排列。
// 只包含真正的菜，已排除 配料、饮品。菜谱列表页仍然展示全部 COOK_RECIPE_COUNT 条。
extern const int16_t COOK_RANDOM_POOL[];

// 一个品类在菜谱表中的连续区间。COOK_RECIPES 按品类分组存放，同类的菜一定挨在一起，
// 所以只要记下首下标与条数就能遍历整个品类，不必再建一张下标表。
// 顺序沿用菜谱原本的排列（也就是 CookLikeHOC 的目录顺序）。
typedef struct {
    const char *name;   // 品类名
    int16_t first;      // 该品类第一道菜在 COOK_RECIPES 中的下标
    int16_t count;      // 该品类的菜谱数
} cook_category_t;

#define COOK_CATEGORY_COUNT 15

extern const cook_category_t COOK_CATEGORIES[];

// 上表的纯下标版本。状态机不认识 cook_category_t，只吃两张 int16 表。
extern const int16_t COOK_CATEGORY_OFFSETS[COOK_CATEGORY_COUNT];
extern const int16_t COOK_CATEGORY_SIZES[COOK_CATEGORY_COUNT];
