// main/windmill_power.h —— 吹气转风车的息屏低功耗控制(见 demo_windmill.c)。
#pragma once

#include "windmill_model.h"

// 启动低功耗轮询任务:每秒查一次空闲时间,到点关屏,再没人动就进 light sleep。
void windmill_power_start(void);

// 息屏中任意键唤醒:恢复显示并把阶段置回 AWAKE。调用方无需持 LVGL 锁。
void windmill_power_wake(void);

// 当前息屏阶段(AWAKE / BLANK / LIGHT)。
windmill_sleep_phase_t windmill_power_phase(void);
