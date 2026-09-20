// main/cook_settings.h —— 亮度与休眠两项设置，以及三级息屏的状态判定。
//
// 三级：正常 -> 关屏 -> 低功耗。无操作满 timeout 秒先关屏（背光灭、面板进 Sleep In、
// 停止刷屏），若继续无人操作，再过 COOK_SLEEP_LIGHT_DELAY 秒让 ESP32-C3 反复进入
// light sleep，只留一个定时轮询等按键。
//
// 中间不再插一个「息屏页」：那类待机页通常要显示时间与设备状态，而本应用没有可靠
// 的时钟来源，画出来只会是错的。
//
// 参考了 leo-radio 的 main/radio_display_settings.cc（NVS 持久化 + 独立轮询），
// 但这里把阶段判定抽成纯函数 cook_sleep_phase_for()，能在主机上直接测。
#pragma once

#include "cook_model.h"   // cook_sleep_phase_t 与 cook_sleep_phase_for 在这一层

#include <stdbool.h>
#include <stdint.h>

// --- 需要 ESP-IDF（NVS 与背光）---------------------------------------------

// 亮度档位（百分比），与 leo-radio 一致取四档。
#define COOK_BRIGHTNESS_COUNT 4
// 休眠档位（秒），0 = 关闭休眠。
#define COOK_TIMEOUT_COUNT 5

// 关屏之后再等多久才进 light sleep。常量而非设置项：它只在"手快的人回来时还有没有
// 全速响应"和"到底多快进入低功耗"之间取折中，多一项反而让用户更难选。
#define COOK_SLEEP_LIGHT_DELAY 5

// 读 NVS、应用亮度、重置空闲计时。失败时退回默认档位，不致命。
bool cook_settings_init(void);

// 记一次用户操作。返回 true 表示这次调用把设备从息屏中唤醒了（调用方据此决定
// 要不要吞掉这个按键，避免唤醒的那一下顺带改了菜谱）。
bool cook_settings_note_activity(void);

// 每秒调一次，返回当前阶段；阶段变化时在本模块内部完成关屏 / 恢复显示。
// 调用方只需要看返回值是不是 COOK_SLEEP_LIGHT——是就该把 CPU 交给 light sleep 循环。
cook_sleep_phase_t cook_settings_tick(void);

cook_sleep_phase_t cook_settings_phase(void);

// 空闲秒数。light sleep 循环用它确认"确实还没人动"，不重复实现一套计时。
uint32_t cook_settings_idle_seconds(void);

// 亮度：0..COOK_BRIGHTNESS_COUNT-1 的档位下标，以及对应的百分比。
int cook_settings_brightness_index(void);
uint8_t cook_settings_brightness(void);
void cook_settings_cycle_brightness(void);
const char *cook_settings_brightness_text(void);

// 休眠档位：下标与秒数。0 号档是"关闭"。
int cook_settings_timeout_index(void);
uint16_t cook_settings_timeout(void);
void cook_settings_cycle_timeout(void);
const char *cook_settings_timeout_text(void);
