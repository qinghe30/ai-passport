// main/main.c —— 数字工牌应用入口:外设初始化 + badge 服务启动 + 按键分发。
//
// 应用结构(区别于原 demo 菜单):四页面 UI 由 badge_ui 持有,main 只负责
// 外设与服务的初始化顺序,并把按键事件从回调上下文转交输入分发任务。
//
// 按键语义(全局统一,详见 badge_ui):
//   上/下/确定 单击  状态机输入(待机开始录音/录音结束/任务翻页/配网页动作)
//   确定      长按  配网页=重置网络;其余页面由状态机定义
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "badge_ui.h"
#include "badge_wifi.h"
#include "badge_store.h"
#include "badge_upload.h"
#include "badge_record.h"
#include "key_guard.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "badge";

#define INPUT_QUEUE_DEPTH 8
// 输入任务里会执行 badge_ui_key 的阻塞型副作用(录音收尾写文件、配网启停),
// 比 demo 的纯渲染更深,栈给大一点。
#define INPUT_TASK_STACK 6144

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
            badge_ui_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "badge_input", INPUT_TASK_STACK, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later, registers a press that
// is already down, and grades it a long press once it has been down for its
// threshold (500 ms here), so an unintended action fires almost immediately. Drop
// key actions until that press comes up; the deadline is only a safety net so a
// stuck key cannot leave the pad dead. The state machine lives in key_guard.h so it
// can be unit-tested without the rest of the app.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        // The release and the deadline both arrive on this task; log the disarm
        // transition once it happens.
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy 数字工牌启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    // 按键唤醒时,那颗键在整个启动期间还按着。先挡住它的动作(见 on_key)。
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是工牌的 UI 载体,失败就没有界面可言 —— 打清楚日志后退出,
    // 不做"串口菜单"降级(那会让本文件复杂一倍)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,工牌无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键输入:分发任务先建,再注册按键回调。失败只记日志 —— 屏幕仍能展示,
    // 但没有按键就无法操作,因此不继续初始化 badge 服务。
    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
        return;
    }
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        input_dispatch_deinit();
        return;
    }
    if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。初始化完成后按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }

    // 电量计单项失败不阻塞:顶栏隐藏电池图标即可。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败,顶栏不显示电量");
    }

    // badge 服务链:Wi-Fi(含 NVS/netif) -> 离线缓存 -> 同步 worker。
    // Wi-Fi 失败时仍可录音并写入离线缓存,联网后再补传,因此不退出。
    if (badge_wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 服务初始化失败,请通过配网页重新配网");
    }
    if (badge_store_init() != ESP_OK) {
        ESP_LOGW(TAG, "离线缓存不可用,录音仅支持在线流式上传");
    }
    if (badge_upload_init() != ESP_OK) {
        ESP_LOGW(TAG, "同步服务启动失败,任务列表可能无法刷新");
    }
    if (badge_record_init() != ESP_OK) {
        ESP_LOGE(TAG, "录音模块初始化失败,按键将无法开始录音");
    }

    // UI 最后启动(内部持 LVGL 锁建页 + 启动 500ms 周期刷新定时器)。
    badge_ui_init();

    if (bsp_lvgl_lock(1000)) {
        s_input_ready = true;
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪");
}
