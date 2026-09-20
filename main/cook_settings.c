// main/cook_settings.c —— 见 cook_settings.h。
//
// 持久化用 NVS，只存档位下标而不存原始值：以后调整档位表，旧的原始值可能落不到
// 任何一档上，而下标永远能夹回合法区间。
#include "cook_settings.h"

#include "bsp_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cook_settings";

static const uint8_t k_brightness_choices[COOK_BRIGHTNESS_COUNT] = {25, 50, 72, 100};
static const uint16_t k_timeout_choices[COOK_TIMEOUT_COUNT] = {0, 30, 60, 300, 600};

// 亮度默认 72%，休眠默认 1 分钟：前者在室内看得清又省电，后者够短能真省电、
// 够长不至于看一道菜看到一半就黑屏。
#define COOK_DEFAULT_BRIGHTNESS_INDEX 2
#define COOK_DEFAULT_TIMEOUT_INDEX 2

#define COOK_NVS_NAMESPACE "cook_display"
#define COOK_NVS_KEY_BRIGHTNESS "bright"
#define COOK_NVS_KEY_TIMEOUT "sleep"

// 关屏/唤醒会同时被输入任务（按键）与低功耗任务（轮询到按键）触发，两处都要读写
// 阶段与空闲计时；单核上用临界区挡住互相踩踏就够了。
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_brightness_index = COOK_DEFAULT_BRIGHTNESS_INDEX;
static int s_timeout_index = COOK_DEFAULT_TIMEOUT_INDEX;
static uint32_t s_last_activity_ms;
static cook_sleep_phase_t s_phase = COOK_SLEEP_AWAKE;

// 面板的 Sleep In / Sleep Out。BSP 只提供了一次性的 deep sleep 接口（会锁住引脚
// 且不可恢复），而息屏之后还得醒过来，所以这里自己发 ST7789 的这两条命令。
// 关背光只断了灯珠，面板自身的静态电流还在，Sleep In 之后才降到微安级。
static void panel_sleep_in(void) {
    esp_lcd_panel_io_handle_t io = bsp_display_io();
    if (io != NULL) {
        esp_lcd_panel_io_tx_param(io, 0x10, NULL, 0);
    }
}

static void panel_sleep_out(void) {
    esp_lcd_panel_io_handle_t io = bsp_display_io();
    if (io != NULL) {
        esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0);
    }
}

static void set_phase(cook_sleep_phase_t phase) {
    portENTER_CRITICAL(&s_lock);
    s_phase = phase;
    portEXIT_CRITICAL(&s_lock);
}

// 关屏：先停掉 LVGL 的刷屏，等在途的那一帧走完，再让面板睡下、背光熄灭。
// 顺序反了会把命令混进正在传输的一帧里，屏幕可能停在半帧。
static void suspend_display(void) {
    lvgl_port_stop();
    vTaskDelay(pdMS_TO_TICKS(30));
    panel_sleep_in();
    bsp_display_backlight(0);
}

// 恢复：背光与面板都要等，面板从 Sleep Out 到能收下一条命令需要约 120 ms。
// 整屏标脏一次，因为睡过之后 GRAM 里剩了什么并不保证。
static void resume_display(void) {
    bsp_display_backlight(k_brightness_choices[s_brightness_index]);
    panel_sleep_out();
    vTaskDelay(pdMS_TO_TICKS(120));
    lvgl_port_resume();
    if (bsp_lvgl_lock(200)) {
        lv_obj_invalidate(lv_screen_active());
        bsp_lvgl_unlock();
    }
}

// esp_timer 的毫秒数在 uint32 里约 49 天回绕一次；下面一律用无符号减法算间隔，
// 回绕时差值依然正确，不需要额外处理。
static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t idle_seconds(void) {
    return (now_ms() - s_last_activity_ms) / 1000u;
}

static void save_index(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open(COOK_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(handle, key, value) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static uint8_t load_index(const char *key, uint8_t fallback, uint8_t limit) {
    nvs_handle_t handle;
    uint8_t value = fallback;
    if (nvs_open(COOK_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, key, &value);
        nvs_close(handle);
    }
    return value < limit ? value : fallback;
}

bool cook_settings_init(void) {
    // 重复调用是幂等的；分区满或版本升级时擦一次再试。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要擦除: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s)，设置项不会被保存", esp_err_to_name(err));
    }

    s_brightness_index = load_index(COOK_NVS_KEY_BRIGHTNESS,
                                    COOK_DEFAULT_BRIGHTNESS_INDEX,
                                    COOK_BRIGHTNESS_COUNT);
    s_timeout_index = load_index(COOK_NVS_KEY_TIMEOUT,
                                 COOK_DEFAULT_TIMEOUT_INDEX,
                                 COOK_TIMEOUT_COUNT);
    s_last_activity_ms = now_ms();
    s_phase = COOK_SLEEP_AWAKE;
    bsp_display_backlight(k_brightness_choices[s_brightness_index]);
    ESP_LOGI(TAG, "亮度 %d%%，休眠 %u 秒关屏",
             k_brightness_choices[s_brightness_index],
             (unsigned)k_timeout_choices[s_timeout_index]);
    return true;
}

bool cook_settings_note_activity(void) {
    const uint32_t now = now_ms();
    bool was_asleep = false;
    portENTER_CRITICAL(&s_lock);
    if (s_phase != COOK_SLEEP_AWAKE) {
        was_asleep = true;
        s_phase = COOK_SLEEP_AWAKE;
    }
    s_last_activity_ms = now;
    portEXIT_CRITICAL(&s_lock);
    if (was_asleep) {
        resume_display();
        ESP_LOGI(TAG, "唤醒: 已恢复显示");
    }
    return was_asleep;
}

cook_sleep_phase_t cook_settings_tick(void) {
    const cook_sleep_phase_t next = cook_sleep_phase_for(
        idle_seconds(), k_timeout_choices[s_timeout_index], COOK_SLEEP_LIGHT_DELAY);
    // 每 20 秒打一条心跳。息屏这类问题只有真机上才看得见，日志是唯一的探针；
    // 关掉休眠时不打，免得日志里全是没用的行。
    static int ticks;
    if (k_timeout_choices[s_timeout_index] != 0 && ++ticks % 20 == 0) {
        ESP_LOGI(TAG, "心跳: 空闲 %u 秒, 阶段 %d", (unsigned)idle_seconds(),
                 (int)s_phase);
    }
    if (next != s_phase) {
        const cook_sleep_phase_t previous = s_phase;
        set_phase(next);
        if (next == COOK_SLEEP_BLANK) {
            suspend_display();
            ESP_LOGI(TAG, "关屏: 空闲 %u 秒, 背光与面板已关", (unsigned)idle_seconds());
        } else if (next == COOK_SLEEP_LIGHT) {
            ESP_LOGI(TAG, "进入低功耗: 空闲 %u 秒", (unsigned)idle_seconds());
        } else if (next == COOK_SLEEP_AWAKE && previous != COOK_SLEEP_AWAKE) {
            resume_display();
        }
    }
    return s_phase;
}

cook_sleep_phase_t cook_settings_phase(void) {
    portENTER_CRITICAL(&s_lock);
    const cook_sleep_phase_t phase = s_phase;
    portEXIT_CRITICAL(&s_lock);
    return phase;
}

uint32_t cook_settings_idle_seconds(void) {
    return idle_seconds();
}

int cook_settings_brightness_index(void) {
    return s_brightness_index;
}

uint8_t cook_settings_brightness(void) {
    return k_brightness_choices[s_brightness_index];
}

void cook_settings_cycle_brightness(void) {
    s_brightness_index = (s_brightness_index + 1) % COOK_BRIGHTNESS_COUNT;
    save_index(COOK_NVS_KEY_BRIGHTNESS, (uint8_t)s_brightness_index);
    bsp_display_backlight(k_brightness_choices[s_brightness_index]);
    s_last_activity_ms = now_ms();
}

const char *cook_settings_brightness_text(void) {
    static char text[8];
    snprintf(text, sizeof(text), "%d%%", k_brightness_choices[s_brightness_index]);
    return text;
}

int cook_settings_timeout_index(void) {
    return s_timeout_index;
}

uint16_t cook_settings_timeout(void) {
    return k_timeout_choices[s_timeout_index];
}

void cook_settings_cycle_timeout(void) {
    s_timeout_index = (s_timeout_index + 1) % COOK_TIMEOUT_COUNT;
    save_index(COOK_NVS_KEY_TIMEOUT, (uint8_t)s_timeout_index);
    s_last_activity_ms = now_ms();
    // 切档位本身就是一次操作，重置空闲计时，避免刚调完就立刻关屏。
    // 档位从"关闭"切走时界面可能正处在关屏态，这里只改阶段、不重复恢复显示：
    // 恢复动作由 note_activity 统一做。
    set_phase(COOK_SLEEP_AWAKE);
}

const char *cook_settings_timeout_text(void) {
    switch (k_timeout_choices[s_timeout_index]) {
        case 0:
            return "关闭";
        case 30:
            return "30秒";
        case 60:
            return "1分钟";
        case 300:
            return "5分钟";
        default:
            return "10分钟";
    }
}
