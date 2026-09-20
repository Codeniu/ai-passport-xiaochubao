// main/cook_power.h —— 关屏之后真正把 ESP32-C3 睡下去的那一层。
//
// 只关背光省不掉多少电：屏幕灯灭了，CPU 还在全速跑、LVGL 还在刷、按键还在每 5 毫秒
// 采一次 ADC。这一层负责在"关屏之后仍然没人动"时，让芯片反复进入 light sleep，只
// 留一个定时轮询等按键。
//
// 用 light sleep 而不是 deep sleep，是因为唤醒后要原样回到刚才那一页：light sleep
// 保留 RAM 与外设状态，唤醒只是"接着跑"；deep sleep 等于重启，页面状态得自己存档，
// 而且三个按键共用一路 ADC，没有可靠的唤醒源。
#pragma once

// 建一个低优先级的任务，每秒查一次阶段、该睡就睡。开机调一次即可。
void cook_power_start(void);
