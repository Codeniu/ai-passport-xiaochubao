// main/windmill_model.h —— 「吹气转风车」的纯逻辑状态机。
//
// 刻意不依赖 LVGL 与 ESP-IDF：风力、吹气驱动、转动角速度、云与花的动画量都能在
// 主机上直接跑测试（见 tests/test_windmill_model.c）。界面层只负责把状态画出来，
// 低功耗任务只负责按阶段关屏 / 进 light sleep。
//
// 动画模型对齐 windmill/windmill-blow-dom.html：用连续的角速度 omega 而不是
// 「等级 -> 转速」的离散表，起风快、收风慢，松口后风车会慢慢滑行到停。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 风力等级上限（与预览图的 10 段风力条一致）。
#define WM_LEVEL_MAX 10

// 屏幕宽度。云的回绕要用到，必须与 demo_windmill.c 的 WM_W 一致。
#define WM_SCREEN_W 240

// 场景里会动的装饰元素数量（与 demo_windmill.c 里建的对象一一对应）。
#define WM_CLOUD_COUNT  3
#define WM_FLOWER_COUNT 2

// 息屏三级阶段：正常显示 -> 关屏(背光灭+面板睡) -> 低功耗(light sleep)。
typedef enum {
    WM_SLEEP_AWAKE = 0,
    WM_SLEEP_BLANK,
    WM_SLEEP_LIGHT,
} windmill_sleep_phase_t;

// 云。x 用 Q8 定点（1 像素 = 256）：无风时基础速度只有 7 px/s，33ms 一步只走
// 0.23 像素，用整像素会被截断成 0，云就永远不动了。
typedef struct {
    int32_t x;   // 左边缘位置，Q8 像素
    int32_t d;   // 速度因子，Q8（256 = 1.0，越大飘得越快）
    int     w;   // 像素宽度，用于判断何时完全出屏
} wm_cloud_t;

// 花丛。随风力摆动，角度单位 0.1 度。
typedef struct {
    int32_t a;
} wm_flower_t;

typedef struct {
    int      level;            // 当前显示风力等级 0..WM_LEVEL_MAX
    int      mic_level;        // 由麦克风 RMS 推导的目标等级(吹气时>0)
    uint32_t blow_boost_ms;    // OK 长按手动吹的剩余助推时间
    int32_t  level_q;          // 平滑后的风力，Q16（65536 = 满力 1.0）
    int32_t  omega_q;          // 角速度，Q16（单位 rad/s）
    int32_t  angle;            // 转子角度(0.1 度), [0,3600)
    int32_t  sway_phase;       // 花朵摆动相位(1/512 圈)
    uint32_t rpm;              // 由 omega 换算的转速(转/分),仅用于 HUD 显示
    wm_cloud_t  cloud[WM_CLOUD_COUNT];
    wm_flower_t flower[WM_FLOWER_COUNT];
    uint32_t last_activity_ms; // 最近一次有效操作(按键或吹气)的时间戳
    uint32_t idle_timeout_ms;  // 关屏空闲阈值,0 表示永不关屏
    uint32_t light_delay_ms;   // 关屏后多久进入低功耗
} windmill_state_t;

// 初始化。idle_timeout_ms/light_delay_ms 决定息屏节奏;seed 预留给未来需要随机抖动的场景。
void windmill_init(windmill_state_t *s, uint32_t idle_timeout_ms,
                   uint32_t light_delay_ms, uint32_t seed);

// 登记一朵云的初始几何。x_px 为初始左边缘(像素)，w_px 为宽度，d_q8 为速度因子
// (256 = 1.0)。不调用则默认 x=0、d=1.0、宽 0。
void windmill_set_cloud(windmill_state_t *s, int idx, int32_t x_px, int w_px, int32_t d_q8);

// 麦克风采集线程每收到一块 PCM 就喂一次 RMS(0..32767)。
// rms 高于门限视为在吹气,会抬高目标风力;否则目标回落到 0。
void windmill_feed_rms(windmill_state_t *s, uint16_t rms, uint32_t now_ms);

// OK 长按:手动吹一口气(无麦克风或没吹到时的兜底)。
void windmill_press_blow(windmill_state_t *s, uint32_t now_ms);

// 任意按键:算作有效操作,重置空闲计时(用于息屏唤醒判断)。
void windmill_touch(windmill_state_t *s, uint32_t now_ms);

// 由 RMS 推导风力等级(纯函数,可测)。低于门限返回 0。
int windmill_blow_level(uint16_t rms);

// 距上次操作多少毫秒(无符号减法,时间戳回绕也正确)。
uint32_t windmill_idle_ms(const windmill_state_t *s, uint32_t now_ms);

// 三级息屏阶段判定(纯函数,可测):
//   timeout == 0                        -> 永远 AWAKE
//   idle < timeout                      -> AWAKE
//   timeout <= idle < timeout + delay   -> BLANK
//   idle >= timeout + delay             -> LIGHT
windmill_sleep_phase_t windmill_sleep_phase_for(uint32_t idle_ms,
                                                 uint32_t idle_timeout_ms,
                                                 uint32_t light_delay_ms);

// 动画步进:每帧调用,dt_ms 为真实经过时间。把风力与角速度拉向目标并推进
// 转子角、云的横向位置、花丛的摆动角。
// 返回 true 表示显示等级变化,界面需要刷新 HUD 文字与风力条。
bool windmill_anim_step(windmill_state_t *s, uint32_t dt_ms);

// 读取接口
int      windmill_level(const windmill_state_t *s);
uint32_t windmill_rpm(const windmill_state_t *s);
int32_t  windmill_angle(const windmill_state_t *s);
int32_t  windmill_cloud_x(const windmill_state_t *s, int idx);      // 像素
int32_t  windmill_flower_angle(const windmill_state_t *s, int idx);  // 0.1 度
