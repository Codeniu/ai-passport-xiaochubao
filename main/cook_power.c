// main/cook_power.c —— 见 cook_power.h。
//
// 睡眠循环的形状：睡 -> 醒 -> 读一次 ADC -> 没键就再睡。之所以是"定时轮询"而不是
// 中断唤醒，是因为三个按键共用 GPIO0 的一路电阻分压，其中「上」键按下时电压仍在
// 数字输入的低电平阈值之上，配成 GPIO 唤醒会漏掉它；只有读电压才能区分三键。
#include "cook_power.h"

#include "bsp_audio.h"
#include "bsp_button.h"
#include "cook_settings.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"

static const char *TAG = "cook_power";

// 轮询间隔。越小唤醒越跟手、耗电越多：120 ms 时一次按键几乎必被采到，而醒着做一次
// ADC 采样占用不到 5 ms，占空比约 4%。
#define COOK_POLL_PERIOD_US (120ULL * 1000ULL)

// 判定"有键按下"的电压上限。松开约 3300 mV，bsp_pins.h 的三档窗口最高到 1900 mV，
// 取 2400 mV 两侧都留了余量。
#define COOK_KEY_PRESSED_MV 2400

// 连续这么多次读不到 ADC 就退出低功耗：宁可退回"关屏但不睡"，也不能让用户按不动。
#define COOK_READ_FAIL_LIMIT 5

// 等手指松开的最长时间，防止一直按着把任务卡住。
#define COOK_RELEASE_WAIT_MS 2000

#define COOK_POWER_TASK_STACK 2048
#define COOK_POWER_TASK_PRIORITY 1

// 睡到按键为止。返回 true 表示是被按键叫醒的。
static bool light_sleep_until_key(void) {
    // 按键组件每 5 ms 采一次 ADC，它那个 esp_timer 会把 light sleep 掐成 5 ms 一段，
    // 低功耗等于白做。进循环前先停掉，退出前再恢复。
    if (iot_button_stop() != ESP_OK) {
        ESP_LOGW(TAG, "按键定时器停止失败，本次不进入低功耗");
        return false;
    }
    // 本应用不用音频，但 codec 还挂在 I2S 上，睡着之前让它也歇着。
    const esp_err_t audio_err = bsp_audio_sleep();
    if (audio_err != ESP_OK) {
        ESP_LOGD(TAG, "音频 suspend 失败(%s)，继续进入低功耗",
                 esp_err_to_name(audio_err));
    }

    bool woke_by_key = false;
    int read_failures = 0;
    unsigned rounds = 0;
    while (cook_settings_phase() == COOK_SLEEP_LIGHT) {
        if (esp_sleep_enable_timer_wakeup(COOK_POLL_PERIOD_US) != ESP_OK) {
            break;
        }
        if (esp_light_sleep_start() != ESP_OK) {
            ESP_LOGW(TAG, "light sleep 未能进入，退回关屏状态");
            break;
        }
        rounds++;
        const int mv = bsp_button_read_mv();
        if (mv < 0) {
            if (++read_failures >= COOK_READ_FAIL_LIMIT) {
                ESP_LOGW(TAG, "低功耗下读不到按键电压，退回关屏状态");
                break;
            }
            continue;
        }
        read_failures = 0;
        if (mv < COOK_KEY_PRESSED_MV) {
            ESP_LOGI(TAG, "按键唤醒: %d mV（低功耗期间醒了 %u 次）", mv, rounds);
            woke_by_key = true;
            break;
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    const esp_err_t wake_err = bsp_audio_wake();
    if (wake_err != ESP_OK) {
        ESP_LOGD(TAG, "音频 resume 失败(%s)", esp_err_to_name(wake_err));
    }

    if (woke_by_key) {
        // 等手指松开再交还按键组件，否则"唤醒那一下"会被它当成一次点击，
        // 醒来就翻了页。
        for (int waited = 0; waited < COOK_RELEASE_WAIT_MS / 20; waited++) {
            const int mv = bsp_button_read_mv();
            if (mv < 0 || mv >= COOK_KEY_PRESSED_MV) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (iot_button_resume() != ESP_OK) {
        ESP_LOGW(TAG, "按键定时器恢复失败，按键可能不可用");
    }
    return woke_by_key;
}

static void cook_power_task(void *arg) {
    (void)arg;
    for (;;) {
        // 1 秒一查：关屏与进低功耗都不需要更细的分辨率，而查得越勤越费电。
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (cook_settings_tick() != COOK_SLEEP_LIGHT) {
            continue;
        }
        if (light_sleep_until_key()) {
            cook_settings_note_activity();
        }
    }
}

void cook_power_start(void) {
    // 栈深单位是 StackType_t（4 字节），2048 即 8 KB：只做 ADC 采样与日志，够用。
    if (xTaskCreate(cook_power_task, "cook_power", COOK_POWER_TASK_STACK, NULL,
                    COOK_POWER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "低功耗任务创建失败，息屏将只关背光");
    }
}
