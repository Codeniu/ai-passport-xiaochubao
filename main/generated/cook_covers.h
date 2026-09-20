// 由 tools/build_cook_assets.py 生成，请勿手工修改。
#pragma once

#include <stdint.h>

// 菜的封面下标在 cook_data.h 的 cover 字段里，两档封面共用同一个下标。
#define COOK_COVER_LARGE_COUNT 177
#define COOK_COVER_LARGE_W 200
#define COOK_COVER_LARGE_H 150

// large 档封面 JPEG 二进制块，由 main/CMakeLists.txt 的 EMBED_FILES 提供。
extern const uint8_t cook_covers_large_blob_start[] asm("_binary_covers_large_bin_start");
extern const uint8_t cook_covers_large_blob_end[] asm("_binary_covers_large_bin_end");

// 取第 index 张封面在本档 blob 中的偏移与长度；越界时返回全 0，由调用方跳过绘制。
void cook_cover_large_locate(int index, uint32_t *offset, uint32_t *size);
#define COOK_COVER_SMALL_COUNT 177
#define COOK_COVER_SMALL_W 144
#define COOK_COVER_SMALL_H 108

// small 档封面 JPEG 二进制块，由 main/CMakeLists.txt 的 EMBED_FILES 提供。
extern const uint8_t cook_covers_small_blob_start[] asm("_binary_covers_small_bin_start");
extern const uint8_t cook_covers_small_blob_end[] asm("_binary_covers_small_bin_end");

// 取第 index 张封面在本档 blob 中的偏移与长度；越界时返回全 0，由调用方跳过绘制。
void cook_cover_small_locate(int index, uint32_t *offset, uint32_t *size);
