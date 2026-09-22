// tests/test_windmill_model.c —— windmill_model.c 的主机测试(不依赖 LVGL/ESP-IDF)。
// 运行: cc -std=c11 -Wall -Wextra -Imain tests/test_windmill_model.c main/windmill_model.c -o /tmp/t && /tmp/t
#include "windmill_model.h"

#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); s_fail++; } \
} while (0)

static uint32_t now_seq;

// 推进若干帧,每帧 dt_ms 毫秒。feed_rms < 0 表示保持上一次的喂值。
static void run_frames(windmill_state_t *s, int frames, uint32_t dt_ms, int feed_rms) {
    for (int i = 0; i < frames; i++) {
        if (feed_rms >= 0) {
            windmill_feed_rms(s, (uint16_t)feed_rms, now_seq);
        }
        windmill_anim_step(s, dt_ms);
        now_seq += dt_ms;
    }
}

static void test_blow_level(void) {
    CHECK(windmill_blow_level(0) == 0, "rms=0 -> 0");
    CHECK(windmill_blow_level(1000) == 0, "rms=1000(<gate) -> 0");
    CHECK(windmill_blow_level(1500) == 0, "rms=gate -> 0");
    CHECK(windmill_blow_level(800) == 0, "rms=floor -> 0");
    // 中点附近应有非零中间等级
    const int mid = windmill_blow_level((800 + 11000) / 2);
    CHECK(mid > 0 && mid < WM_LEVEL_MAX, "rms=mid -> 中间等级");
    CHECK(windmill_blow_level(11000) == WM_LEVEL_MAX, "rms=full -> 满级");
    CHECK(windmill_blow_level(32767) == WM_LEVEL_MAX, "rms>full -> 满级(夹紧)");
}

static void test_sleep_phase(void) {
    // timeout == 0 -> 永远 AWAKE
    CHECK(windmill_sleep_phase_for(999999, 0, 5000) == WM_SLEEP_AWAKE,
          "timeout=0 -> AWAKE");
    // 空闲未到阈值
    CHECK(windmill_sleep_phase_for(30000, 60000, 5000) == WM_SLEEP_AWAKE,
          "idle<timeout -> AWAKE");
    // 刚好到点 -> BLANK
    CHECK(windmill_sleep_phase_for(60000, 60000, 5000) == WM_SLEEP_BLANK,
          "idle==timeout -> BLANK");
    // 关屏后未到 light 延迟 -> BLANK
    CHECK(windmill_sleep_phase_for(62000, 60000, 5000) == WM_SLEEP_BLANK,
          "timeout<idle<timeout+delay -> BLANK");
    // 超过延迟 -> LIGHT
    CHECK(windmill_sleep_phase_for(110000, 60000, 5000) == WM_SLEEP_LIGHT,
          "idle>=timeout+delay -> LIGHT");
}

// 满力吹 -> 松口 -> 缓慢滑停。这是「转动应当缓慢停止」这条需求的回归测试。
static void test_spin_up_and_coast(void) {
    windmill_state_t s;
    windmill_init(&s, 60000, 5000, 1);
    now_seq = 1000;

    // 持续吹 2 秒(30fps)拉到接近满速。
    run_frames(&s, 60, 33, 11000);
    CHECK(windmill_level(&s) == WM_LEVEL_MAX, "持续满级吹气 -> level 满");
    const uint32_t peak = windmill_rpm(&s);
    CHECK(peak >= 90 && peak <= 120, "满力转速落在 90~120 转/分(网页 11 rad/s)");

    // 松口 1 秒:应当还在明显地转,不能一松就停。
    run_frames(&s, 30, 33, 0);
    const uint32_t t1 = windmill_rpm(&s);
    CHECK(t1 > peak / 4, "松口 1 秒后仍在惯性转动(不是立刻停)");

    // 松口 10 秒:应当退回微风底噪。
    run_frames(&s, 270, 33, 0);
    const uint32_t t10 = windmill_rpm(&s);
    CHECK(windmill_level(&s) == 0, "停吹后 level 归零");
    CHECK(t10 <= 3, "松口 10 秒后回到微风底噪");
    // 转速必须单调下降,否则说明减速曲线写反了。
    CHECK(peak > t1 && t1 > t10, "转速单调下降");
}

// 单帧转角不能太大:四叶风车 90 度对称,每帧超过 45 度就会出现「看着倒转」。
static void test_per_frame_step(void) {
    windmill_state_t s;
    windmill_init(&s, 60000, 5000, 1);
    now_seq = 1000;
    run_frames(&s, 60, 33, 11000);

    const int32_t a0 = windmill_angle(&s);
    windmill_anim_step(&s, 33);
    int32_t d = windmill_angle(&s) - a0;
    if (d < 0) d += 3600;
    CHECK(d > 0, "满速时角度在推进");
    CHECK(d < 450, "满速单帧转角 < 45 度(不会频闪/倒转)");
}

static void test_manual_blow(void) {
    windmill_state_t s;
    windmill_init(&s, 60000, 5000, 1);
    now_seq = 5000;
    // 无麦克风,靠 OK 长按手动吹
    windmill_press_blow(&s, now_seq);
    for (int i = 0; i < 10; i++) {
        windmill_anim_step(&s, 33);
    }
    CHECK(windmill_level(&s) > 0, "手动吹 -> level 上升");
    for (int i = 0; i < 600; i++) {
        windmill_anim_step(&s, 33);
    }
    CHECK(windmill_level(&s) == 0, "手动吹结束后 level 归零");
}

static void test_idle_wrap(void) {
    windmill_state_t s;
    windmill_init(&s, 60000, 5000, 1);
    // 模拟时间戳接近回绕边界
    const uint32_t near_wrap = 0xFFFFFFF0u;
    windmill_touch(&s, near_wrap);
    CHECK(windmill_idle_ms(&s, near_wrap) == 0, "刚操作 idle=0");
    // 回绕后差值仍正确(无符号减法)
    CHECK(windmill_idle_ms(&s, 0x00000005u) == 0x15u, "时间戳回绕后 idle 正确");
}

// 无风也要飘(基础 7 px/s),有风要明显更快,而且要能出屏回绕。
static void test_cloud_drift(void) {
    windmill_state_t s;
    windmill_init(&s, 60000, 5000, 1);
    windmill_set_cloud(&s, 0, 18, 64, 256);
    now_seq = 1000;

    // 无风 3 秒:7 px/s * 3s = 21 px。用整像素会被截断成 0,这里必须真的动。
    run_frames(&s, 90, 33, 0);
    const int32_t x_calm = windmill_cloud_x(&s, 0);
    CHECK(x_calm > 18 + 15 && x_calm < 18 + 30, "无风 3 秒云飘了约 21 px");

    // 满风再飘 3 秒:速度约 69 px/s,应当远快于无风。
    const int32_t before = x_calm;
    run_frames(&s, 90, 33, 11000);
    const int32_t x_wind = windmill_cloud_x(&s, 0);
    CHECK(x_wind - before > 120, "满风 3 秒云明显加速");

    // 一直吹到出屏:应当回绕到左侧负坐标,而不是一路飞到无穷大。
    for (int i = 0; i < 600 && windmill_cloud_x(&s, 0) < WM_SCREEN_W; i++) {
        run_frames(&s, 1, 33, 11000);
    }
    run_frames(&s, 60, 33, 11000);
    const int32_t x_wrap = windmill_cloud_x(&s, 0);
    CHECK(x_wrap < 0, "云出屏后回绕到屏幕左侧");
    CHECK(x_wrap > -(64 + 20) - 1, "回绕位置是 -(宽+20)");
}

// 花丛摆动:静止时也有小幅抖动,风大时幅度变大,且两丛花相位不同。
static void test_flower_sway(void) {
    windmill_state_t s;
    windmill_init(&s, 60000, 5000, 1);
    now_seq = 1000;

    // 满风下采样一段,看幅度与相位。
    run_frames(&s, 60, 33, 11000);
    int32_t lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
    bool differ = false;
    for (int i = 0; i < 120; i++) {
        run_frames(&s, 1, 33, 11000);
        const int32_t a0 = windmill_flower_angle(&s, 0);
        const int32_t a1 = windmill_flower_angle(&s, 1);
        if (a0 < lo0) lo0 = a0;
        if (a0 > hi0) hi0 = a0;
        if (a1 < lo1) lo1 = a1;
        if (a1 > hi1) hi1 = a1;
        if (a0 != a1) differ = true;
    }
    CHECK(hi0 > 20 && lo0 < -20, "满风时花丛摆幅接近 ±5 度");
    CHECK(hi0 <= 60 && lo0 >= -60, "摆幅不超过设计上限");
    CHECK(differ, "两丛花摆动相位不同");

    // 静置后幅度收窄。
    run_frames(&s, 600, 33, 0);
    int32_t lo_c = 0, hi_c = 0;
    for (int i = 0; i < 400; i++) {
        run_frames(&s, 1, 33, 0);
        const int32_t a0 = windmill_flower_angle(&s, 0);
        if (a0 < lo_c) lo_c = a0;
        if (a0 > hi_c) hi_c = a0;
    }
    CHECK(hi_c <= 20 && lo_c >= -20, "无风时花丛只剩微风小幅摆动");
    CHECK(hi_c > 0, "无风时花丛仍在轻微摆动");
}

int main(void) {
    printf("windmill_model 主机测试\n");
    test_blow_level();
    test_sleep_phase();
    test_spin_up_and_coast();
    test_per_frame_step();
    test_manual_blow();
    test_idle_wrap();
    test_cloud_drift();
    test_flower_sway();
    if (s_fail == 0) {
        printf("全部通过\n");
        return 0;
    }
    printf("%d 项失败\n", s_fail);
    return 1;
}
