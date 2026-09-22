// main/demo_windmill.c —— 「吹气转风车」：把 windmill-blow-dom 网页移植到 ESP32-C3。
//
// 玩法：对着 ES8311 麦克风吹气 -> 风车转得越快；OK 长按 = 手动吹一口气(无麦克风兜底)。
// 太久不操作 -> 关屏(背光灭+面板睡) -> 再没人动 -> 进 light sleep，任意键唤醒。
//
// 与 BSP 的边界(同小厨宝)：按键由 main.c 输入任务派发；LVGL 对象只在持锁时创建/删除；
// 状态机在 windmill_model.c(不依赖 LVGL)，息屏进出在 windmill_power(本文件末尾)。
//
// 素材：风车/花/云/海/草来自 windmill/scene，已预处理成 ARGB8888 的 lv_image_dsc_t
// (main/assets/windmill/)，运行时直接 lv_image_set_src，无需解码器。
#include "demo.h"

#include "windmill_model.h"
#include "windmill_power.h"
#include "windmill_imgs.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "windmill";

LV_FONT_DECLARE(cook_font_14);

// ---------------------------------------------------------------------------
// 视觉常量(与 windmill_240x320_preview 预览图一致)
// ---------------------------------------------------------------------------
#define WM_W 240
#define WM_H 320

#define WM_C_SKY_TOP  0x0F85E4
#define WM_C_SKY_BOT  0x7FC0EE
#define WM_C_NAVY     0x06284F
#define WM_C_CYAN     0x12D4FB
#define WM_C_INK      0xF2F8FF
#define WM_C_MUTED    0xA9D9FF
#define WM_C_GREEN    0x39E07A
#define WM_C_BAR_ON   0x79EBFF
#define WM_C_BAR_OFF  0x1654A4
#define WM_C_LOW      0xFF6B6B   // 低电量告警色

// 右上角电量:外框 + 内条 + 正极帽。
#define WM_BATT_W        18
#define WM_BATT_H        10
#define WM_BATT_LABEL_W  44   // 按最长文案 "100%" 给足,避免 WRAP 折成两行
// 电量走 I2C 读 CW2017,没必要每帧读。
#define WM_BATT_PERIOD_MS 3000

// 风车在屏幕上的钉点(与预览图一致):hub 钉在 (120,110)。
#define WM_HUB_X 120
#define WM_HUB_Y 110
#define WM_ROTOR_HALF 81   // rotor 图 163x163,半边长 81,中心即 hub

// 动画帧率与风力条段数
#define WM_ANIM_MS 33
#define WM_BARS    10

// 动画推进用「真实经过时间」而不是固定步长:屏刷一趟要 20~40ms 且抖动不小,
// 按固定 33ms 走会把抖动直接变成忽快忽慢。超过这个值就钳住(息屏刚恢复时会很大)。
#define WM_ANIM_MAX_DT_MS 100

// 息屏节奏(可被 NVS/设置扩展,这里用固定值)
#define WM_BRIGHTNESS      72
#define WM_IDLE_TIMEOUT_MS 60000   // 1 分钟无操作关屏
#define WM_LIGHT_DELAY_MS  5000    // 关屏后再过 5 秒进 light sleep

// 低功耗轮询(复用小厨宝的判定:三键共 ADC,只能靠定时+读电压区分)
#define WM_POLL_PERIOD_US  (120ULL * 1000ULL)
#define WM_KEY_PRESSED_MV  2400
#define WM_READ_FAIL_LIMIT 5
#define WM_RELEASE_WAIT_MS 2000

// 麦克风采集
#define WM_SAMPLE_HZ 16000
#define WM_CHUNK     256            // 每次读 256 个 16bit 样本
// 栈深单位是【字节】(实测:xTaskCreate 传 2048 得到的栈边界跨度是 2032 字节)。
// wm_audio 里会跑 bsp_audio_set_format() —— 它会 close/open ES8311 并重建 I2S
// channel,调用链很深;2KB 栈必然溢出(真机表现为 Stack protection fault 反复重启)。
#define WM_AUDIO_STACK 8192
#define WM_AUDIO_PRIO  5

// wm_power 会持 LVGL 锁做界面操作 + 调 bsp_audio_sleep/wake,同样不能给 2KB。
#define WM_POWER_STACK 8192
#define WM_POWER_PRIO  1

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------
static windmill_state_t s_state;

static lv_obj_t *s_screen;
static lv_obj_t *s_rotor;
static lv_obj_t *s_cloud[WM_CLOUD_COUNT];
static lv_obj_t *s_flower[WM_FLOWER_COUNT];
static lv_obj_t *s_lvl_label;
static lv_obj_t *s_rpm_label;
static lv_obj_t *s_bar[WM_BARS];
static lv_obj_t *s_batt_label;
static lv_obj_t *s_batt_fill;
static lv_timer_t *s_timer;

// 动画计时:真实 dt 的基准时刻 + 电量上次刷新时刻。
static uint32_t s_last_ms;
static uint32_t s_batt_ms;

static TaskHandle_t s_audio_task;
static volatile bool s_audio_run;

static TaskHandle_t s_power_task;
static volatile bool s_power_run;

// 息屏阶段与"是否已关屏"标志,供按键唤醒与低功耗任务协调。
static windmill_sleep_phase_t s_phase = WM_SLEEP_AWAKE;
static bool s_blanked = false;
static portMUX_TYPE s_pmux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// 画面构建
// ---------------------------------------------------------------------------
static lv_obj_t *wm_add_img(lv_obj_t *parent, const lv_image_dsc_t *dsc, int x, int y) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
}

static void wm_build_scene(void) {
    // 天空渐变(屏幕背景)
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(WM_C_SKY_TOP), 0);
    lv_obj_set_style_bg_grad_color(s_screen, lv_color_hex(WM_C_SKY_BOT), 0);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(s_screen, 0, 0);
    lv_obj_set_style_bg_grad_stop(s_screen, 255, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    // 海 / 草(三段式背景)
    wm_add_img(s_screen, &windmill_sea, 0, 150);
    wm_add_img(s_screen, &windmill_grass, 0, 192);

    // 云(按预览图位置)。d 是每朵云的漂移速度因子(256 = 1.0):大的/靠前的飘得快,
    // 三朵同速会像一整块背景在平移,看不出远近。
    static const struct {
        const lv_image_dsc_t *dsc;
        int x, y, d;
    } k_clouds[WM_CLOUD_COUNT] = {
        { &windmill_cloud2, 18,  34, 256 },  // 64x32
        { &windmill_cloud4, 150, 70, 192 },  // 56x29
        { &windmill_cloud1, 70,  96, 140 },  // 40x20
    };
    for (int i = 0; i < WM_CLOUD_COUNT; i++) {
        s_cloud[i] = wm_add_img(s_screen, k_clouds[i].dsc, k_clouds[i].x, k_clouds[i].y);
        windmill_set_cloud(&s_state, i, k_clouds[i].x,
                           (int)k_clouds[i].dsc->header.w, k_clouds[i].d);
    }

    // 风车:机身(静态) + 转子(旋转)。hub 钉在 (WM_HUB_X, WM_HUB_Y)。
    const int sx = WM_HUB_X - (int)(52.5f * 1.4f);   // 静态图 hub 在 52.5,缩放 1.4
    const int sy = WM_HUB_Y - (int)(53.8f * 1.4f);
    wm_add_img(s_screen, &windmill_static, sx, sy);

    s_rotor = lv_image_create(s_screen);
    lv_image_set_src(s_rotor, &windmill_rotor);
    lv_obj_set_pos(s_rotor, WM_HUB_X - WM_ROTOR_HALF, WM_HUB_Y - WM_ROTOR_HALF);
    lv_image_set_pivot(s_rotor, WM_ROTOR_HALF, WM_ROTOR_HALF);
    lv_image_set_rotation(s_rotor, 0);
    // 163x163 的 ARGB8888 每帧做带插值的旋转变换是全场最贵的一步,关掉抗锯齿
    // 能省掉逐像素的双线性采样 —— 叶片边缘略硬,但帧率换回来了。
    lv_image_set_antialias(s_rotor, false);
    lv_obj_clear_flag(s_rotor, LV_OBJ_FLAG_CLICKABLE);

    // 花朵(风车底座两侧草地前景)。以「底部中心」为轴,整丛跟着风倒而不是原地打转。
    static const int k_flower_x[WM_FLOWER_COUNT] = { 16, 160 };
    static const int k_flower_y[WM_FLOWER_COUNT] = { 222, 230 };
    for (int i = 0; i < WM_FLOWER_COUNT; i++) {
        s_flower[i] = wm_add_img(s_screen, &windmill_flowers, k_flower_x[i], k_flower_y[i]);
        lv_image_set_pivot(s_flower[i], (int)windmill_flowers.header.w / 2,
                           (int)windmill_flowers.header.h);
        lv_image_set_rotation(s_flower[i], 0);
    }
}

static lv_obj_t *wm_panel(int x, int y, int w, int h, lv_color_t border) {
    lv_obj_t *p = lv_obj_create(s_screen);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_radius(p, 7, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(WM_C_NAVY), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, border, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *wm_label(lv_obj_t *parent, const char *text, lv_color_t color, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &cook_font_14, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// 右上角电量。soc < 0 表示 CW2017 没应答,显示 "--" 而不是编一个数字。
static void wm_update_battery(void) {
    if (s_batt_label == NULL) {
        return;
    }
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_batt_label, "--");
        lv_obj_set_style_text_color(s_batt_label, lv_color_hex(WM_C_MUTED), 0);
        lv_obj_set_width(s_batt_fill, 0);
        return;
    }
    lv_label_set_text_fmt(s_batt_label, "%d%%", soc);
    lv_obj_set_style_text_color(s_batt_label,
        lv_color_hex(soc < 20 ? WM_C_LOW : WM_C_MUTED), 0);

    // 内条最多占 (宽 - 边框 - 内壁),最少留 1 像素,空电也看得出是个电池。
    int fill = (WM_BATT_W - 4) * soc / 100;
    if (fill < 1) {
        fill = 1;
    }
    if (fill > WM_BATT_W - 4) {
        fill = WM_BATT_W - 4;
    }
    lv_obj_set_width(s_batt_fill, fill);
    lv_obj_set_style_bg_color(s_batt_fill,
        lv_color_hex(soc < 20 ? WM_C_LOW : WM_C_INK), 0);
}

// 顶栏里的电池图形:外框 + 内条 + 正极帽。坐标相对顶栏(228x26)。
static void wm_build_battery(lv_obj_t *top) {
    const int frame_x = 228 - 10 - WM_BATT_W - 2;
    s_batt_label = wm_label(top, "", lv_color_hex(WM_C_MUTED),
                            frame_x - 4 - WM_BATT_LABEL_W, 6);
    lv_obj_set_width(s_batt_label, WM_BATT_LABEL_W);
    lv_label_set_long_mode(s_batt_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_batt_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *frame = lv_obj_create(top);
    lv_obj_set_size(frame, WM_BATT_W, WM_BATT_H);
    lv_obj_set_pos(frame, frame_x, 8);
    lv_obj_set_style_radius(frame, 2, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(WM_C_MUTED), 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_shadow_width(frame, 0, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_fill = lv_obj_create(frame);
    lv_obj_set_pos(s_batt_fill, 2, 2);
    lv_obj_set_size(s_batt_fill, 0, WM_BATT_H - 4);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(WM_C_INK), 0);
    lv_obj_set_style_border_width(s_batt_fill, 0, 0);
    lv_obj_set_style_radius(s_batt_fill, 0, 0);
    lv_obj_set_style_shadow_width(s_batt_fill, 0, 0);
    lv_obj_clear_flag(s_batt_fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap = lv_obj_create(top);
    lv_obj_set_size(cap, 2, 4);
    lv_obj_set_pos(cap, frame_x + WM_BATT_W, 11);
    lv_obj_set_style_bg_color(cap, lv_color_hex(WM_C_MUTED), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_set_style_radius(cap, 0, 0);
    lv_obj_set_style_shadow_width(cap, 0, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);

    wm_update_battery();
}

static void wm_build_hud(void) {
    // 顶栏:标题 + 「监听」指示(绿点) + 右上角电量
    lv_obj_t *top = wm_panel(6, 6, 228, 26, lv_color_hex(WM_C_CYAN));
    wm_label(top, "吹气转风车", lv_color_hex(WM_C_INK), 8, 6);
    wm_label(top, "监听", lv_color_hex(WM_C_MUTED), 100, 6);
    lv_obj_t *dot = lv_obj_create(top);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, 132, 10);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(WM_C_GREEN), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    wm_build_battery(top);

    // 底栏:风力读数 + 转速读数 + 10 段风力条 + 提示
    lv_obj_t *bot = wm_panel(6, 260, 228, 50, lv_color_hex(WM_C_CYAN));
    s_lvl_label = wm_label(bot, "风力 0 级", lv_color_hex(WM_C_INK), 14, 5);
    s_rpm_label = wm_label(bot, "转速 0 转/分", lv_color_hex(WM_C_CYAN), 120, 5);

    const int bx = 14, by = 25, bw_total = 200, bh = 7, gap = 2;
    const int seg = (bw_total - gap * (WM_BARS - 1)) / WM_BARS;
    for (int i = 0; i < WM_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(bot);
        lv_obj_set_size(bar, seg, bh);
        lv_obj_set_pos(bar, bx + i * (seg + gap), by);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(WM_C_BAR_OFF), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        s_bar[i] = bar;
    }

    wm_label(bot, "对着麦克风吹气，风车转动越快", lv_color_hex(WM_C_MUTED), 14, 35);
}

// ---------------------------------------------------------------------------
// 动画定时器(运行在 LVGL 任务内,可直接操作对象,无需额外加锁)
// ---------------------------------------------------------------------------
static void wm_anim_cb(lv_timer_t *t) {
    (void)t;
    if (s_phase != WM_SLEEP_AWAKE) {
        return;  // 关屏/低功耗时画面冻结
    }

    // 用真实经过时间推进:屏刷一趟的耗时抖动很大,固定步长会把它原样变成忽快忽慢。
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t dt = now - s_last_ms;
    s_last_ms = now;
    if (dt > WM_ANIM_MAX_DT_MS) {
        dt = WM_ANIM_MAX_DT_MS;
    }

    const bool changed = windmill_anim_step(&s_state, dt);

    // 转子、云、花每帧都动(风力为 0 时转子仍有微风底噪,不会完全静止)。
    lv_image_set_rotation(s_rotor, s_state.angle);
    for (int i = 0; i < WM_CLOUD_COUNT; i++) {
        lv_obj_set_x(s_cloud[i], windmill_cloud_x(&s_state, i));
    }
    for (int i = 0; i < WM_FLOWER_COUNT; i++) {
        lv_image_set_rotation(s_flower[i], windmill_flower_angle(&s_state, i));
    }

    if (now - s_batt_ms >= WM_BATT_PERIOD_MS) {
        s_batt_ms = now;
        wm_update_battery();
    }

    if (changed) {
        char buf[32];
        snprintf(buf, sizeof(buf), "风力 %d 级", windmill_level(&s_state));
        lv_label_set_text(s_lvl_label, buf);
        snprintf(buf, sizeof(buf), "转速 %lu 转/分", (unsigned long)windmill_rpm(&s_state));
        lv_label_set_text(s_rpm_label, buf);
        for (int i = 0; i < WM_BARS; i++) {
            const bool on = i < windmill_level(&s_state);
            lv_obj_set_style_bg_color(s_bar[i],
                on ? lv_color_hex(WM_C_BAR_ON) : lv_color_hex(WM_C_BAR_OFF), 0);
        }
    }
}

// ---------------------------------------------------------------------------
// 麦克风采集线程
// ---------------------------------------------------------------------------
static uint16_t wm_rms(const int16_t *buf, int n) {
    if (n <= 0) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        const int32_t v = buf[i];
        sum += (uint32_t)(v * v);
    }
    const float mean = (float)sum / (float)n;
    return (uint16_t)sqrtf(mean);
}

static void wm_audio_task(void *arg) {
    (void)arg;
    bsp_audio_set_format(WM_SAMPLE_HZ, 16, 1);
    int16_t *buf = (int16_t *)malloc(WM_CHUNK * sizeof(int16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "音频缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }
    while (s_audio_run) {
        // 息屏/低功耗时别占着 codec 轮询,等唤醒再说。
        if (windmill_power_phase() != WM_SLEEP_AWAKE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const esp_err_t err = bsp_audio_read(buf, WM_CHUNK * sizeof(int16_t));
        if (err == ESP_OK) {
            const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
            windmill_feed_rms(&s_state, wm_rms(buf, WM_CHUNK), now);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    free(buf);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 息屏低功耗(显示关/开 + light sleep 唤醒)
// ---------------------------------------------------------------------------
static void panel_sleep_in(void) {
    esp_lcd_panel_io_handle_t io = bsp_display_io();
    if (io != NULL) {
        esp_lcd_panel_io_tx_param(io, 0x10, NULL, 0);  // ST7789 Sleep In
    }
}

static void panel_sleep_out(void) {
    esp_lcd_panel_io_handle_t io = bsp_display_io();
    if (io != NULL) {
        esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0);  // ST7789 Sleep Out
    }
}

// 关屏:停刷屏 -> 等一帧走完 -> 面板睡 -> 背光灭。调用方不应持 LVGL 锁。
static void wm_display_suspend(void) {
    lvgl_port_stop();
    vTaskDelay(pdMS_TO_TICKS(30));
    panel_sleep_in();
    bsp_display_backlight(0);
    s_blanked = true;
}

// 恢复显示(假设当前持 LVGL 锁)。
static void wm_display_resume_locked(void) {
    bsp_display_backlight(WM_BRIGHTNESS);
    panel_sleep_out();
    vTaskDelay(pdMS_TO_TICKS(120));
    lvgl_port_resume();
    lv_obj_invalidate(lv_screen_active());
    s_blanked = false;
}

// 恢复显示(自行加锁,供低功耗任务使用)。
static void wm_display_resume(void) {
    if (bsp_lvgl_lock(200)) {
        wm_display_resume_locked();
        bsp_lvgl_unlock();
    }
}

// 睡到按键为止。返回 true 表示被按键唤醒。
static bool wm_light_sleep_until_key(void) {
    if (iot_button_stop() != ESP_OK) {
        ESP_LOGW(TAG, "按键定时器停止失败,本次不进入低功耗");
        return false;
    }
    const esp_err_t audio_err = bsp_audio_sleep();
    if (audio_err != ESP_OK) {
        ESP_LOGD(TAG, "音频 suspend 失败(%s),继续进入低功耗", esp_err_to_name(audio_err));
    }

    bool woke = false;
    int read_failures = 0;
    unsigned rounds = 0;
    while (s_phase == WM_SLEEP_LIGHT) {
        if (esp_sleep_enable_timer_wakeup(WM_POLL_PERIOD_US) != ESP_OK) break;
        if (esp_light_sleep_start() != ESP_OK) {
            ESP_LOGW(TAG, "light sleep 未能进入,退回关屏状态");
            break;
        }
        rounds++;
        const int mv = bsp_button_read_mv();
        if (mv < 0) {
            if (++read_failures >= WM_READ_FAIL_LIMIT) {
                ESP_LOGW(TAG, "低功耗下读不到按键电压,退回关屏状态");
                break;
            }
            continue;
        }
        read_failures = 0;
        if (mv < WM_KEY_PRESSED_MV) {
            ESP_LOGI(TAG, "按键唤醒: %d mV(低功耗期间醒了 %u 次)", mv, rounds);
            woke = true;
            break;
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    const esp_err_t wake_err = bsp_audio_wake();
    if (wake_err != ESP_OK) {
        ESP_LOGD(TAG, "音频 resume 失败(%s)", esp_err_to_name(wake_err));
    }
    if (woke) {
        for (int waited = 0; waited < WM_RELEASE_WAIT_MS / 20; waited++) {
            const int mv = bsp_button_read_mv();
            if (mv < 0 || mv >= WM_KEY_PRESSED_MV) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (iot_button_resume() != ESP_OK) {
        ESP_LOGW(TAG, "按键定时器恢复失败,按键可能不可用");
    }
    return woke;
}

static void wm_power_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!s_power_run) {
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        const uint32_t idle = windmill_idle_ms(&s_state, now);
        const windmill_sleep_phase_t phase =
            windmill_sleep_phase_for(idle, s_state.idle_timeout_ms, s_state.light_delay_ms);

        portENTER_CRITICAL(&s_pmux);
        const windmill_sleep_phase_t prev = s_phase;
        portEXIT_CRITICAL(&s_pmux);

        if (phase == prev) continue;

        if (phase == WM_SLEEP_BLANK && prev == WM_SLEEP_AWAKE) {
            wm_display_suspend();
            portENTER_CRITICAL(&s_pmux); s_phase = WM_SLEEP_BLANK; portEXIT_CRITICAL(&s_pmux);
            ESP_LOGI(TAG, "关屏: 空闲 %u 秒", (unsigned)(idle / 1000));
        } else if (phase == WM_SLEEP_LIGHT) {
            if (prev == WM_SLEEP_AWAKE) {
                wm_display_suspend();
            }
            portENTER_CRITICAL(&s_pmux); s_phase = WM_SLEEP_LIGHT; portEXIT_CRITICAL(&s_pmux);
            ESP_LOGI(TAG, "进入低功耗: 空闲 %u 秒", (unsigned)(idle / 1000));
            const bool woke = wm_light_sleep_until_key();
            if (woke) {
                windmill_touch(&s_state, now);
                wm_display_resume();  // 自行加锁
                portENTER_CRITICAL(&s_pmux); s_phase = WM_SLEEP_AWAKE; portEXIT_CRITICAL(&s_pmux);
            }
        } else if (phase == WM_SLEEP_AWAKE && prev != WM_SLEEP_AWAKE) {
            // 被按键路径唤醒(显示已恢复),这里只同步阶段
            portENTER_CRITICAL(&s_pmux); s_phase = WM_SLEEP_AWAKE; portEXIT_CRITICAL(&s_pmux);
        }
    }
}

void windmill_power_start(void) {
    s_power_run = true;
    if (xTaskCreate(wm_power_task, "wm_power", WM_POWER_STACK, NULL,
                    WM_POWER_PRIO, &s_power_task) != pdPASS) {
        ESP_LOGE(TAG, "低功耗任务创建失败,息屏将只关背光");
    }
}

windmill_sleep_phase_t windmill_power_phase(void) {
    portENTER_CRITICAL(&s_pmux);
    const windmill_sleep_phase_t p = s_phase;
    portEXIT_CRITICAL(&s_pmux);
    return p;
}

void windmill_power_wake(void) {
    portENTER_CRITICAL(&s_pmux);
    const bool was_asleep = (s_phase != WM_SLEEP_AWAKE);
    s_phase = WM_SLEEP_AWAKE;
    portEXIT_CRITICAL(&s_pmux);
    if (!was_asleep) return;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    windmill_touch(&s_state, now);
    if (s_blanked) {
        wm_display_resume();  // 自行加锁
    }
}

// ---------------------------------------------------------------------------
// demo_entry 接口
// ---------------------------------------------------------------------------
void demo_windmill_enter(void) {
    windmill_init(&s_state, WM_IDLE_TIMEOUT_MS, WM_LIGHT_DELAY_MS, 0x5EED5EEDu);
    s_phase = WM_SLEEP_AWAKE;
    s_blanked = false;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, WM_W, WM_H);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_shadow_width(s_screen, 0, 0);

    wm_build_scene();
    wm_build_hud();
    lv_screen_load(s_screen);

    // 动画用真实 dt,基准时刻必须在起定时器前设好,否则第一帧会拿到一个天文数字。
    s_last_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_batt_ms = s_last_ms;
    s_timer = lv_timer_create(wm_anim_cb, WM_ANIM_MS, NULL);

    s_audio_run = true;
    if (xTaskCreate(wm_audio_task, "wm_audio", WM_AUDIO_STACK, NULL,
                    WM_AUDIO_PRIO, &s_audio_task) != pdPASS) {
        ESP_LOGE(TAG, "音频任务创建失败,只能用 OK 长按手动吹");
        s_audio_run = false;
    }

    windmill_power_start();
    ESP_LOGI(TAG, "吹气转风车就绪");
}

void demo_windmill_exit(void) {
    s_audio_run = false;
    s_power_run = false;
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_rotor = NULL;
    s_batt_label = NULL;
    s_batt_fill = NULL;
    for (int i = 0; i < WM_CLOUD_COUNT; i++) s_cloud[i] = NULL;
    for (int i = 0; i < WM_FLOWER_COUNT; i++) s_flower[i] = NULL;
}

void demo_windmill_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) {
        return;  // PRESS / DOUBLE 本应用无语义
    }
    const bool is_long = (ev == BSP_BTN_LONG);

    // 息屏中:第一下按键只唤醒,不触发任何动作(避免「唤醒那一下」顺带改状态)。
    if (windmill_power_phase() != WM_SLEEP_AWAKE) {
        windmill_power_wake();
        return;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    windmill_touch(&s_state, now);
    // OK 长按 = 手动吹一口气(无麦克风/没吹到时的兜底)
    if (is_long && btn == BSP_BTN_OK) {
        windmill_press_blow(&s_state, now);
    }
}
