// main/main.c —— 小厨宝固件入口：初始化 BSP、把按键事件派发给应用。
//
// 按键语义(全局统一,长按与短按不抢同一个动作):
//   上/下 短按   首页=切换按钮;列表页=逐项移动;详情页=正文翻一页;设置页=切换条目
//   上/下 长按   列表页=翻页;详情页=翻子页(封面/配料/步骤)
//   确定  短按   首页=进入;随机页=看做法;列表页=进详情;设置页=调整/进入
//   确定  长按   首页=进设置;其余各页=返回上一级
//   息屏中      任意键只用于唤醒,不触发动作
//
// 本应用开机直达,不经过 baseline 的演示菜单:菜单是板级能力展示界面,master
// 分支的 demo_*.c 仍然编译在固件里,但不再注册到界面上。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "cook_settings.h"
#include "cook_power.h"
#include "demo.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8
// xTaskCreate 的栈深单位是 StackType_t(4 字节),8192 即 32 KB。
// 按键处理会在这个任务里构建整页 LVGL 对象,并对封面跑一次 JPEG 解码
// (lv_tjpgd 的 decoder_info 单独就要约 4.5 KB 栈),必须留足余量。
#define INPUT_TASK_STACK 8192
#define INPUT_TASK_PRIORITY 5

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            demo_cook_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "cook_input", INPUT_TASK_STACK, NULL,
                    INPUT_TASK_PRIORITY, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// button 回调运行在共享的 esp_timer 任务上:只入队,立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport 小厨宝启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本应用的唯一输出。初始化失败就没有界面可言,打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    // 亮度与休眠档位存在 NVS 里:这里读出存档的亮度并应用,不再固定拉满。
    // 失败只影响"记住上次设置",不影响开机。
    if (!cook_settings_init()) {
        ESP_LOGW(TAG, "显示设置初始化失败,使用默认亮度");
        bsp_display_backlight(72);
    }

    // 音频与电量计只是可选能力:失败不影响菜谱浏览。
    const esp_err_t audio_err = bsp_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败: %s", esp_err_to_name(audio_err));
    }
    const esp_err_t battery_err = bsp_battery_init();
    if (battery_err != ESP_OK) {
        // 电量读值会退化成 -1,界面显示 "--"。
        ESP_LOGW(TAG, "电量计初始化失败: %s", esp_err_to_name(battery_err));
    }

    const esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = ESP_ERR_INVALID_STATE;
    if (input_err == ESP_OK) {
        button_err = bsp_button_init(on_key, NULL);
    }
    if (input_err != ESP_OK || button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键不可用(input=%s button=%s),界面无法操作",
                 esp_err_to_name(input_err), esp_err_to_name(button_err));
        return;
    }

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败,无法创建首页");
        return;
    }
    demo_cook_enter();
    bsp_lvgl_unlock();
    s_input_ready = true;
    // 息屏的第三级（light sleep）由一个独立任务驱动：它每秒查一次空闲时间，
    // 该睡就睡，睡醒靠读按键电压判断。放在这里是因为它要在界面就绪之后才需要工作。
    cook_power_start();

    ESP_LOGI(TAG, "就绪");
}
