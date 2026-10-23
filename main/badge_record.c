// main/badge_record.c —— 录音采集任务:读取 I2S PCM,按网络状态实时上传或写离线缓存。
// 独立任务执行采集(参考 demo_audio 的任务生命周期模式);按键/UI 只调用 start/stop。
// 落点规则:
//   启动时在线   -> badge_upload 流式会话(chunked 实时上传);
//   启动时离线   -> badge_store 缓存文件(联网后补传);
//   录音中断网/积压 -> 作废流式会话(服务器收到不完整数据),切换文件续录。
#include "badge_record.h"

#include "badge_config.h"
#include "badge_upload.h"
#include "badge_store.h"
#include "badge_wifi.h"
#include "bsp_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "badge_record";

// 停止收尾的最大等待:包含发送剩余数据与等待后台响应的时间。
// 超时后任务自身仍会完成收尾并置回空闲(下次 start 前会清掉残留信号量)。
#define REC_STOP_TIMEOUT_MS 10000
#define REC_CMD_START 1

// 离线续录期间网络恢复后切回流式续传的重试间隔。
// 太短会在服务端不可达时反复创建/销毁 HTTP 会话,太长则白白消耗离线缓存空间。
#define REC_RESUME_INTERVAL_US (5 * 1000000)

typedef enum {
    REC_PHASE_IDLE = 0,   // 空闲
    REC_PHASE_ACTIVE,     // 采集中(流式或文件)
    REC_PHASE_FINISHING,  // 停止收尾中(关闭会话/文件)
} rec_phase_t;

static SemaphoreHandle_t s_lock;
static rec_phase_t s_phase = REC_PHASE_IDLE;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;

static volatile bool s_stop_req;     // 采集循环每块检查
static volatile bool s_discard_req;  // 停止时是否丢弃数据
static volatile bool s_streaming;    // 当前落点:实时上传
static volatile bool s_start_req;    // 启动请求的期望落点(在线流式)

static int64_t s_start_us;
static uint32_t s_duration_ms;
static esp_err_t s_last_result = ESP_OK;
static int64_t s_resume_try_us;   // 上次尝试切回流式的时间点
static bool s_resume_pending;     // 已发起切回流式,正在等待连接建立

// 从流式切换到离线缓存:作废当前会话(服务器记为不完整),后续数据写文件。
// 切换失败返回 false(缓存不可用或建文件失败),调用方结束录音并报错。
static bool switch_to_file(char *name, size_t name_size, badge_store_writer_t *w)
{
    badge_upload_stream_end(true);
    s_streaming = false;
    if (!badge_store_ready()) {
        ESP_LOGE(TAG, "离线缓存不可用,无法续录");
        return false;
    }
    if (badge_store_open(name, name_size, w) != ESP_OK) {
        ESP_LOGE(TAG, "创建缓存文件失败");
        return false;
    }
    return true;
}

// 执行一次完整录音:格式化音频 -> 选定落点 -> 循环采集分发 -> 收尾。
static void record_once(void)
{
    badge_store_writer_t w;
    char name[16];
    bool have_file = false;

    s_last_result = ESP_OK;
    s_start_us = esp_timer_get_time();
    s_duration_ms = 0;
    s_resume_try_us = 0;
    s_resume_pending = false;

    if (bsp_audio_init() != ESP_OK ||
        bsp_audio_set_format(BADGE_SAMPLE_RATE_HZ, BADGE_SAMPLE_BITS,
                             BADGE_SAMPLE_CHANNELS) != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败");
        s_last_result = ESP_ERR_INVALID_STATE;
        return;
    }

    if (s_start_req && badge_wifi_net_up() &&
        badge_upload_stream_begin() == ESP_OK) {
        s_streaming = true;
    }
    if (!s_streaming) {
        // 离线(或流式会话建立失败):直接写缓存文件。
        if (!badge_store_ready() ||
            badge_store_open(name, sizeof(name), &w) != ESP_OK) {
            ESP_LOGE(TAG, "无网络且缓存不可用,放弃本次录音");
            s_last_result = ESP_ERR_NO_MEM;
            bsp_audio_deinit();
            return;
        }
        have_file = true;
    }

    uint8_t *buf = malloc(BADGE_CHUNK_SAMPLES * 2);
    if (!buf) {
        ESP_LOGE(TAG, "采集缓冲分配失败");
        s_last_result = ESP_ERR_NO_MEM;
        goto finish;
    }

    while (!s_stop_req) {
        if (bsp_audio_read(buf, BADGE_CHUNK_SAMPLES * 2) != ESP_OK) {
            ESP_LOGE(TAG, "I2S 读取失败");
            s_last_result = ESP_FAIL;
            break;
        }
        s_duration_ms =
            (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);

        if (s_streaming) {
            if (!badge_wifi_net_up()) {
                // 网络断开:作废会话,切换文件续录。
                if (!switch_to_file(name, sizeof(name), &w)) {
                    s_last_result = ESP_FAIL;
                    break;
                }
                have_file = true;
            } else if (badge_upload_stream_write(buf,
                                                 BADGE_CHUNK_SAMPLES * 2) != ESP_OK) {
                // 网络过慢导致上传缓冲持续写满:同样切换文件续录。
                ESP_LOGW(TAG, "上传缓冲积压,切换离线缓存续录");
                if (!switch_to_file(name, sizeof(name), &w)) {
                    s_last_result = ESP_FAIL;
                    break;
                }
                have_file = true;
            }
        } else {
            // 离线续录期间若网络恢复,切回流式续传:当前缓存文件保留(由同步 worker
            // 事后补传),否则断网会一直吃掉有限的离线缓存额度。
            // 会话在后台建立,确认连上后才切,采集循环不阻塞,音频不丢。
            int state = badge_upload_stream_state();
            if (state == 2) {
                if (badge_upload_stream_write(buf, BADGE_CHUNK_SAMPLES * 2) == ESP_OK) {
                    badge_store_close(&w);
                    have_file = false;
                    s_streaming = true;
                    s_resume_pending = false;
                    ESP_LOGI(TAG, "网络恢复,切回流式续传");
                    continue;
                }
                badge_upload_stream_end(true);   // 写入失败:作废本次尝试
                s_resume_pending = false;
            } else if (state == 1) {
                // 连接中:继续写缓存,等下一次循环再判断。
            } else {
                if (s_resume_pending) {
                    // 上次尝试的会话已自行退出:交还会话锁,进入冷却后重试。
                    s_resume_pending = false;
                    badge_upload_stream_end(true);
                } else if (badge_wifi_net_up() &&
                           esp_timer_get_time() - s_resume_try_us > REC_RESUME_INTERVAL_US) {
                    s_resume_try_us = esp_timer_get_time();
                    if (badge_upload_stream_begin() == ESP_OK) {
                        s_resume_pending = true;
                    }
                }
            }
            if (badge_store_write(&w, buf, BADGE_CHUNK_SAMPLES * 2) != ESP_OK) {
                ESP_LOGE(TAG, "缓存写入失败");
                s_last_result = ESP_FAIL;
                break;
            }
        }
    }

    free(buf);
    s_duration_ms = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);

finish:
    // 收尾:discard=true 丢弃全部数据;false 正常保存/送达。
    if (s_resume_pending) {
        // 还没切过去的候选会话:直接作废,避免会话锁残留导致下次无法建立流式。
        badge_upload_stream_end(true);
        s_resume_pending = false;
    }
    if (s_streaming) {
        esp_err_t r = badge_upload_stream_end(s_discard_req);
        s_streaming = false;
        if (r != ESP_OK || (!s_discard_req && !badge_upload_stream_delivered())) {
            s_last_result = ESP_FAIL;
        }
    }
    if (have_file) {
        badge_store_close(&w);
        if (s_discard_req) {
            badge_store_delete(name);
        }
    }
    // 录音结束即释放 codec 与 I2S 通道(约十几 KB):配网启动 BLE 与 HTTP 上传
    // 都依赖大块连续堆,不释放会导致 NimBLE 启动后余量不足。
    bsp_audio_deinit();
}

static void record_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t cmd = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd != REC_CMD_START) {
            continue;
        }
        record_once();
        // 收尾完成,回到空闲并唤醒等待停止的调用方。
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_phase = REC_PHASE_IDLE;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_done);
    }
}

esp_err_t badge_record_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "录音互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t badge_record_start(bool streaming)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase != REC_PHASE_IDLE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    // 惰性创建采集任务(首次使用时)。
    if (!s_task) {
        s_done = xSemaphoreCreateBinary();
        if (!s_done) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(record_task, "badge_rec", 5120, NULL, 4, &s_task) != pdPASS) {
            vSemaphoreDelete(s_done);
            s_done = NULL;
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
    } else {
        // 清掉上次收尾超时残留的信号量令牌。
        xSemaphoreTake(s_done, 0);
    }
    s_phase = REC_PHASE_ACTIVE;
    s_stop_req = false;
    s_discard_req = false;
    s_start_req = streaming;
    xSemaphoreGive(s_lock);

    xTaskNotify(s_task, REC_CMD_START, eSetValueWithOverwrite);
    return ESP_OK;
}

esp_err_t badge_record_stop(bool discard)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase != REC_PHASE_ACTIVE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_phase = REC_PHASE_FINISHING;
    s_discard_req = discard;
    s_stop_req = true;
    xSemaphoreGive(s_lock);

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(REC_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "录音收尾超时(任务将自行完成收尾)");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool badge_record_active(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_phase != REC_PHASE_IDLE;
    xSemaphoreGive(s_lock);
    return active;
}

uint32_t badge_record_duration_ms(void)
{
    if (s_phase == REC_PHASE_ACTIVE) {
        return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
    }
    return s_duration_ms;
}

bool badge_record_streaming(void)
{
    return s_streaming && s_phase == REC_PHASE_ACTIVE;
}

esp_err_t badge_record_last_result(void)
{
    return s_last_result;
}
