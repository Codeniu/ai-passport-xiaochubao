// main/windmill_model.c —— 「吹气转风车」纯逻辑状态机。见 windmill_model.h。
//
// 不依赖 LVGL / ESP-IDF,可被主机直接编译测试。所有字段都是 32 位对齐整数,
// 在单核 ESP32-C3 上读写为原子的,故界面线程(动画定时器)与音频线程(喂 RMS)
// 共享本结构体时无需额外加锁。
//
// 动画手感全部对齐 windmill/windmill-blow-dom.html:
//   风力     level += (raw - level) * dt * (raw>level ? 16 : 4)
//   角速度   target = OMEGA_IDLE/2 + level*OMEGA_MAX
//            omega += (target - omega) * dt * (target>omega ? 3.2 : 1.0)
//   云       x += (7 + level*62) * d * dt,出屏后回到左侧
//   花       a = sin(1.6*angle + i*1.7) * (0.030 + level*0.055)
// 之所以用定点整数而不是 float:ESP32-C3 没有浮点单元,且整数能保证主机测试与
// 真机数值逐位一致。
#include "windmill_model.h"

#include <string.h>

// 吹气判定与等级映射的标定常量。麦克风经 ES8311 采集 16bit 单声道 PCM,
// 平静环境底噪 RMS 通常 < 800,正常吹气可达数千。这些值在真机上按手感微调。
#define WM_RMS_GATE   1500   // 高于此值视为在吹气
#define WM_RMS_FLOOR  800    // 映射起点(低于按 0 处理)
#define WM_RMS_FULL   11000  // 映射到满级(10)的 RMS
#define WM_RMS_MAX    32767

// 手动吹(OK 长按)持续时间与对应的等级。
#define WM_BOOST_MS   1200
#define WM_BOOST_LEVEL 7

// 定点小数位宽:Q16 表示 1.0 = 65536。
#define WM_Q16 65536

// 角速度标定(与网页一致,单位 rad/s):
//   无风时仍有 OMEGA_IDLE/2 的徐徐微风,转起来才不会显得死掉。
//   满力 OMEGA_MAX = 11 rad/s ≈ 105 转/分 ≈ 30fps 下每帧 21 度 —— 恰好低于
//   四叶风车 90 度对称的一半,不会出现「看着倒转」的频闪。之前的 600 转/分是
//   每帧 120 度,这才是画面不流畅的主因。
#define WM_OMEGA_IDLE_Q  (WM_Q16 / 4)   // 0.25 rad/s
#define WM_OMEGA_MAX     11             // 11 rad/s(乘以 Q16 风力得到 rad/s)

// 一阶低通的时间常数系数,传值时放大 10 倍(16 -> 160,3.2 -> 32)。
// 起风/加速都快,收风/减速都慢 —— 慢的那一侧就是「缓慢停止」的手感。
#define WM_K_LEVEL_UP     160   // 16.0
#define WM_K_LEVEL_DOWN   40    // 4.0
#define WM_K_OMEGA_UP     32    // 3.2
#define WM_K_OMEGA_DOWN   10    // 1.0

// 云的漂移:基础 7 px/s,满力再加 62 px/s(网页同一组数)。
#define WM_CLOUD_BASE_Q8  (7 << 8)
#define WM_CLOUD_WIND     62

// 花丛摆动:基础幅度 0.030 rad,满力再加 0.055 rad;换算成 0.1 度。
#define WM_FLOWER_AMP_BASE 17   // 0.030 rad * 572.958
#define WM_FLOWER_AMP_WIND 32   // 0.055 rad * 572.958
// 摆动相位比转子快 1.6 倍;两丛花之间错开 1.7 rad(1.7/(2π)*512 ≈ 139)。
#define WM_FLOWER_PHASE_STEP 139
// 摆动的「微风底噪」频率:1.2 rad/s ≈ 5 秒一个来回。
// 这是有意偏离网页的地方 —— 网页只按 1.6*omega 摆,无风时 omega 只有
// 0.125 rad/s,整丛花 30 秒才摆一个来回,看着是冻住的。
// 相位累加器比查表精度高 256 倍(见 WM_SWAY_SUB),否则这个底噪在 33ms 步长下
// 同样会被整数截断成 0。
#define WM_SWAY_IDLE_X1000 25033  // 每毫秒的相位增量 × 1000
#define WM_SWAY_SUB        8      // 相位累加器比 1/512 圈多 8 位小数

// 单帧最大步进:显示被别的事拖住时(比如刚从息屏恢复)别让动画一次跳一大截。
#define WM_MAX_DT_MS 100

// ---------------------------------------------------------------------------
// 正弦查表:四分之一周期 128 段(整圈 512),输出 Q15。
// ---------------------------------------------------------------------------
#define WM_SIN_STEPS 128
#define WM_SIN_TURN  (WM_SIN_STEPS * 4)

static const int16_t k_sin_quarter[WM_SIN_STEPS + 1] = {
    0, 402, 804, 1206, 1608, 2009, 2410, 2811,
    3212, 3612, 4011, 4410, 4808, 5205, 5602, 5998,
    6393, 6786, 7179, 7571, 7962, 8351, 8739, 9126,
    9512, 9896, 10278, 10659, 11039, 11417, 11793, 12167,
    12539, 12910, 13279, 13645, 14010, 14372, 14732, 15090,
    15446, 15800, 16151, 16499, 16846, 17189, 17530, 17869,
    18204, 18537, 18868, 19195, 19519, 19841, 20159, 20475,
    20787, 21096, 21403, 21705, 22005, 22301, 22594, 22884,
    23170, 23452, 23731, 24007, 24279, 24547, 24811, 25072,
    25329, 25582, 25832, 26077, 26319, 26556, 26790, 27019,
    27245, 27466, 27683, 27896, 28105, 28310, 28510, 28706,
    28898, 29085, 29268, 29447, 29621, 29791, 29956, 30117,
    30273, 30424, 30571, 30714, 30852, 30985, 31113, 31237,
    31356, 31470, 31580, 31685, 31785, 31880, 31971, 32057,
    32137, 32213, 32285, 32351, 32412, 32469, 32521, 32567,
    32609, 32646, 32678, 32705, 32728, 32745, 32757, 32765,
    32767,
};

// phase 单位是 1/512 圈。
static int16_t wm_sin_q15(uint32_t phase) {
    const uint32_t idx = phase & (uint32_t)(WM_SIN_TURN - 1);
    const uint32_t off = idx & (uint32_t)(WM_SIN_STEPS - 1);  // 0..127 段内偏移
    switch (idx >> 7) {  // 0..3 象限
        case 0:  return  k_sin_quarter[off];
        case 1:  return  k_sin_quarter[WM_SIN_STEPS - off];
        case 2:  return (int16_t)-k_sin_quarter[off];
        default: return (int16_t)-k_sin_quarter[WM_SIN_STEPS - off];
    }
}

// 一阶低通:返回 cur 朝 target 靠拢这一步的增量。k10 是 dt 的系数,放大 10 倍传入。
static int32_t wm_approach(int32_t cur, int32_t target, uint32_t dt_ms, uint32_t k10) {
    // alpha = dt(秒) * k,钳到 1,保证单帧不会过冲。
    uint64_t alpha = (uint64_t)dt_ms * k10 * WM_Q16 / 10000ULL;
    if (alpha > WM_Q16) alpha = WM_Q16;
    const int32_t diff = target - cur;
    return (int32_t)(((int64_t)diff * (int64_t)alpha) >> 16);
}

static int clamp_level(int v) {
    if (v < 0) return 0;
    if (v > WM_LEVEL_MAX) return WM_LEVEL_MAX;
    return v;
}

void windmill_init(windmill_state_t *s, uint32_t idle_timeout_ms,
                   uint32_t light_delay_ms, uint32_t seed) {
    memset(s, 0, sizeof(*s));
    s->idle_timeout_ms = idle_timeout_ms;
    s->light_delay_ms = light_delay_ms;
    for (int i = 0; i < WM_CLOUD_COUNT; i++) {
        s->cloud[i].x = 0;
        s->cloud[i].w = 0;
        s->cloud[i].d = 256;  // 默认 1.0
    }
    (void)seed;  // 预留;当前实现不需要随机性
}

void windmill_set_cloud(windmill_state_t *s, int idx, int32_t x_px, int w_px, int32_t d_q8) {
    if (s == NULL || idx < 0 || idx >= WM_CLOUD_COUNT) return;
    s->cloud[idx].x = x_px << 8;
    s->cloud[idx].w = w_px;
    s->cloud[idx].d = d_q8;
}

int windmill_blow_level(uint16_t rms) {
    if (rms <= WM_RMS_GATE) {
        return 0;
    }
    if (rms <= WM_RMS_FLOOR) {
        return 0;
    }
    const int span = WM_RMS_FULL - WM_RMS_FLOOR;
    int level = (int)((uint32_t)(rms - WM_RMS_FLOOR) * WM_LEVEL_MAX / (uint32_t)span);
    return clamp_level(level);
}

void windmill_feed_rms(windmill_state_t *s, uint16_t rms, uint32_t now_ms) {
    s->mic_level = windmill_blow_level(rms);
    if (s->mic_level > 0) {
        // 正在吹气算作有效操作,保持屏幕常亮。
        windmill_touch(s, now_ms);
    }
}

void windmill_press_blow(windmill_state_t *s, uint32_t now_ms) {
    s->blow_boost_ms = WM_BOOST_MS;
    windmill_touch(s, now_ms);
}

void windmill_touch(windmill_state_t *s, uint32_t now_ms) {
    s->last_activity_ms = now_ms;
}

uint32_t windmill_idle_ms(const windmill_state_t *s, uint32_t now_ms) {
    return now_ms - s->last_activity_ms;
}

windmill_sleep_phase_t windmill_sleep_phase_for(uint32_t idle_ms,
                                                 uint32_t idle_timeout_ms,
                                                 uint32_t light_delay_ms) {
    if (idle_timeout_ms == 0) {
        return WM_SLEEP_AWAKE;
    }
    if (idle_ms < idle_timeout_ms) {
        return WM_SLEEP_AWAKE;
    }
    if (idle_ms < idle_timeout_ms + light_delay_ms) {
        return WM_SLEEP_BLANK;
    }
    return WM_SLEEP_LIGHT;
}

bool windmill_anim_step(windmill_state_t *s, uint32_t dt_ms) {
    const int prev_level = s->level;
    if (dt_ms > WM_MAX_DT_MS) {
        dt_ms = WM_MAX_DT_MS;
    }

    // 手动吹助推递减。
    if (s->blow_boost_ms > 0) {
        if (s->blow_boost_ms > dt_ms) {
            s->blow_boost_ms -= dt_ms;
        } else {
            s->blow_boost_ms = 0;
        }
    }
    const int boost_level = (s->blow_boost_ms > 0) ? WM_BOOST_LEVEL : 0;
    const int drive = clamp_level(s->mic_level > boost_level ? s->mic_level : boost_level);

    // 目标风力(Q16):0..10 的等级映射到 0..1。
    const int32_t raw_q = (int32_t)drive * (WM_Q16 / WM_LEVEL_MAX);
    // 起风快、收风慢。
    s->level_q += wm_approach(s->level_q, raw_q, dt_ms,
                              raw_q > s->level_q ? WM_K_LEVEL_UP : WM_K_LEVEL_DOWN);
    if (s->level_q < 0) s->level_q = 0;
    if (s->level_q > WM_Q16) s->level_q = WM_Q16;

    // 角速度:目标 = 微风底噪 + 风力 * 满速;加速快、减速慢(松口后慢慢滑停)。
    const int32_t target_q = (WM_OMEGA_IDLE_Q / 2) + s->level_q * WM_OMEGA_MAX;
    s->omega_q += wm_approach(s->omega_q, target_q, dt_ms,
                              target_q > s->omega_q ? WM_K_OMEGA_UP : WM_K_OMEGA_DOWN);
    if (s->omega_q < 0) s->omega_q = 0;

    // 转子角度(0.1 度) += omega(rad/s) * dt(s) * 1800/π。
    // 5729578 = 572.9578 * 10000,655360000000 = 65536(Q16) * 1000(ms) * 10000。
    s->angle += (int32_t)(((int64_t)s->omega_q * (int64_t)dt_ms * 5729578LL) / 655360000000LL);
    if (s->angle >= 3600) s->angle %= 3600;
    if (s->angle < 0) s->angle += 3600;

    // 花朵摆动相位:1.6 倍转子角速度(5093120 = 1.6*512*256/(2π) * 1e4),
    // 再叠加固定的微风底噪。
    s->sway_phase += (int32_t)((uint64_t)dt_ms * WM_SWAY_IDLE_X1000 / 1000ULL)
                   + (int32_t)(((int64_t)s->omega_q * (int64_t)dt_ms * 5093120LL)
                               / 10000000000LL);
    s->sway_phase &= ((WM_SIN_TURN << WM_SWAY_SUB) - 1);

    const int32_t amp = WM_FLOWER_AMP_BASE
                      + (int32_t)(((int64_t)s->level_q * WM_FLOWER_AMP_WIND) >> 16);
    for (int i = 0; i < WM_FLOWER_COUNT; i++) {
        const int16_t sv = wm_sin_q15((uint32_t)(s->sway_phase >> WM_SWAY_SUB)
                                      + (uint32_t)(i * WM_FLOWER_PHASE_STEP));
        s->flower[i].a = (int32_t)(((int64_t)sv * amp) >> 15);
    }

    // 云:速度 = (7 + 风力*62) * 每朵云的因子。
    const int32_t speed_q8 = WM_CLOUD_BASE_Q8
                           + (int32_t)(((int64_t)s->level_q * WM_CLOUD_WIND) >> 8);
    for (int i = 0; i < WM_CLOUD_COUNT; i++) {
        wm_cloud_t *c = &s->cloud[i];
        c->x += (int32_t)(((int64_t)speed_q8 * (int64_t)c->d * (int64_t)dt_ms) / 256000LL);
        const int32_t limit = (int32_t)(WM_SCREEN_W + c->w) << 8;
        if (c->x > limit) {
            c->x = -((int32_t)(c->w + 20) << 8);  // 完全出屏后从左侧重新飘进来
        }
    }

    // HUD 读数。
    s->rpm = (uint32_t)(((int64_t)s->omega_q * 95493LL) / 655360000LL);  // rad/s -> 转/分
    s->level = (int)(((int64_t)s->level_q * WM_LEVEL_MAX + (WM_Q16 / 2)) >> 16);

    return s->level != prev_level;
}

int windmill_level(const windmill_state_t *s) { return s->level; }
uint32_t windmill_rpm(const windmill_state_t *s) { return s->rpm; }
int32_t windmill_angle(const windmill_state_t *s) { return s->angle; }
int32_t windmill_cloud_x(const windmill_state_t *s, int idx) { return s->cloud[idx].x >> 8; }
int32_t windmill_flower_angle(const windmill_state_t *s, int idx) { return s->flower[idx].a; }
