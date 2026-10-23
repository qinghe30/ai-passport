// main/badge_upload.c —— HTTP 上传与同步实现。
// 约定的后台接口(badge-backend 演示后台实现):
//   POST /api/recordings/stream  流式 PCM 上传,X-* 头携带元数据
//   GET  /api/badge/tasks?device=ID  任务摘要 JSON
#include "badge_upload.h"

#include "badge_config.h"
#include "badge_fmt.h"
#include "badge_store.h"
#include "badge_tasks.h"
#include "badge_wifi.h"

#include "cJSON.h"
#include "esp_efuse.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "badge_upload";

// 上传子任务的读写块与网络超时。写超时要给弱网络留足余地。
#define STREAM_WRITE_TIMEOUT_MS 5000
#define HTTP_TIMEOUT_MS         10000
#define REPLAY_CHUNK            2048
#define TASKS_BUF_SIZE          4096

typedef struct {
    StreamBufferHandle_t buf;
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool stop;        // 请求子任务收尾
    volatile bool discard;     // 收尾时丢弃未发数据
    volatile bool delivered;   // 本次会话数据完整送达
    volatile bool opened;      // HTTP 连接已建立
    volatile bool running;
    size_t sent_bytes;
} stream_session_t;

static struct {
    QueueHandle_t sync_queue;      // 深度 1:忙时新请求合并
    TaskHandle_t worker;
    SemaphoreHandle_t session_lock;
    stream_session_t session;
    char device_id[13];
    bool inited;
} s_up;

// --- 公共小工具 ---

void badge_upload_device_id(char *buf, unsigned size)
{
    if (!buf || size == 0) return;
    snprintf(buf, size, "%s", s_up.device_id);
}

static void device_id_init(void)
{
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(s_up.device_id, sizeof(s_up.device_id),
             "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void set_common_headers(esp_http_client_handle_t client)
{
    char value[64];
    snprintf(value, sizeof(value), "%s", s_up.device_id);
    esp_http_client_set_header(client, "X-Device-Id", value);
}

// --- 任务拉取 ---

static void tasks_fetch_once(void)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/api/badge/tasks?device=%s",
             BADGE_SERVER_URL, s_up.device_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 创建失败(任务拉取)");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    set_common_headers(client);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "任务拉取连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGW(TAG, "任务拉取 HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    char *body = heap_caps_malloc(TASKS_BUF_SIZE, MALLOC_CAP_8BIT);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    int total = 0;
    while (total < TASKS_BUF_SIZE - 1) {
        int n = esp_http_client_read(client, body + total, TASKS_BUF_SIZE - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    badge_task_t parsed[BADGE_TASK_MAX];
    int count = 0;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *arr = cJSON_GetObjectItem(root, "tasks");
        if (cJSON_IsArray(arr)) {
            cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                if (count >= BADGE_TASK_MAX) break;
                cJSON *jid = cJSON_GetObjectItem(item, "id");
                cJSON *title = cJSON_GetObjectItem(item, "title");
                cJSON *assignee = cJSON_GetObjectItem(item, "assignee");
                cJSON *due = cJSON_GetObjectItem(item, "due");
                cJSON *status = cJSON_GetObjectItem(item, "status");
                if (!cJSON_IsNumber(jid) || !cJSON_IsString(title)) continue;
                badge_task_t *t = &parsed[count];
                memset(t, 0, sizeof(*t));
                t->id = (int)jid->valuedouble;
                badge_fmt_clip_utf8(title->valuestring, t->title, sizeof(t->title));
                if (cJSON_IsString(assignee)) {
                    badge_fmt_clip_utf8(assignee->valuestring, t->assignee,
                                        sizeof(t->assignee));
                }
                if (cJSON_IsString(due)) {
                    badge_fmt_clip_utf8(due->valuestring, t->due, sizeof(t->due));
                }
                t->pending = !(status && cJSON_IsString(status) &&
                               strcmp(status->valuestring, "done") == 0);
                count++;
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    badge_tasks_store(parsed, count);
    ESP_LOGI(TAG, "任务已刷新: %d 条", count);
}

// --- 补传 ---

// 上传一个缓存文件。完整送达且后台返回 2xx 才算成功。
static esp_err_t replay_one_file(const char *name, size_t bytes)
{
    esp_err_t err = badge_store_read_open(name);
    if (err != ESP_OK) return err;

    char url[160];
    snprintf(url, sizeof(url), "%s/api/recordings/stream", BADGE_SERVER_URL);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        badge_store_read_close();
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    char value[48];
    snprintf(value, sizeof(value), "%u", (unsigned)BADGE_SAMPLE_RATE_HZ);
    esp_http_client_set_header(client, "X-Sample-Rate", value);
    esp_http_client_set_header(client, "X-Sample-Bits", "16");
    esp_http_client_set_header(client, "X-Channels", "1");
    esp_http_client_set_header(client, "X-Recording-Mode", "replay");

    esp_err_t result = ESP_FAIL;
    err = esp_http_client_open(client, (int)bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "补传连接失败: %s", esp_err_to_name(err));
        goto out;
    }
    // 这里不能提前读响应头:请求体还没发,服务器要收完整个 body 才会回响应,
    // 提前 fetch 会一直阻塞到超时,补传必然失败。

    char *chunk = heap_caps_malloc(REPLAY_CHUNK, MALLOC_CAP_8BIT);
    if (!chunk) goto out;
    bool all_sent = true;
    size_t remaining = bytes;
    while (remaining > 0) {
        size_t want = remaining < REPLAY_CHUNK ? remaining : REPLAY_CHUNK;
        size_t got = 0;
        if (badge_store_read(chunk, want, &got) != ESP_OK || got == 0) {
            all_sent = false;
            break;
        }
        int w = esp_http_client_write(client, chunk, (int)got);
        if (w < 0 || (size_t)w != got) {
            all_sent = false;
            break;
        }
        remaining -= got;
    }
    free(chunk);

    if (all_sent) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            result = ESP_OK;
        } else {
            ESP_LOGW(TAG, "补传 %s HTTP %d", name, status);
        }
    }
out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    badge_store_read_close();
    return result;
}

static int replay_pending_files(void)
{
    int uploaded = 0;
    // 迭代期间成功即删除,目录会收缩,因此每次都取第 0 项。
    for (;;) {
        char name[24];
        size_t bytes = 0;
        if (badge_store_entry(0, name, sizeof(name), &bytes) != ESP_OK) break;
        if (bytes == 0) {
            // 空文件(如断电残留):直接清理。
            badge_store_delete(name);
            continue;
        }
        if (replay_one_file(name, bytes) == ESP_OK) {
            badge_store_delete(name);
            uploaded++;
            ESP_LOGI(TAG, "补传完成: %s (%u 字节)", name, (unsigned)bytes);
        } else {
            break;   // 网络不可用/服务异常:下次再试
        }
    }
    return uploaded;
}

// --- 流式会话 ---

static void stream_url(char *buf, unsigned size)
{
    snprintf(buf, size, "%s/api/recordings/stream", BADGE_SERVER_URL);
}

// 写一个 HTTP 分块(chunked)帧:<十六进制长度>\r\n<数据>\r\n。
// esp_http_client_write 在 chunked 模式下只发原始字节、不加分块帧,
// 因此这里自行封装,后台才能按 Transfer-Encoding: chunked 正确解析。
// 整帧必须一次写出:若拆成"分块头/数据/分块尾"三次写,会产生两个只有几字节的
// TCP 小段;lwIP 的发送队列(TCP_SND_QUEUELEN, 16 段)会很快被小段占满,
// 之后 send 一直等不到可写(实测录制约 1 秒、发送约 6KB 后卡死 10 秒)。
static bool stream_write_chunk(esp_http_client_handle_t client, char *frame,
                               const char *data, size_t len)
{
    int n = snprintf(frame, 16, "%x\r\n", (unsigned)len);
    memcpy(frame + n, data, len);
    memcpy(frame + n + len, "\r\n", 2);
    int total = n + (int)len + 2;
    return esp_http_client_write(client, frame, total) == total;
}

static void stream_task(void *arg)
{
    (void)arg;
    stream_session_t *s = &s_up.session;

    char url[160];
    stream_url(url, sizeof(url));
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 创建失败(流式)");
        s->running = false;
        xSemaphoreGive(s->done);
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    set_common_headers(client);
    char value[48];
    snprintf(value, sizeof(value), "%u", (unsigned)BADGE_SAMPLE_RATE_HZ);
    esp_http_client_set_header(client, "X-Sample-Rate", value);
    esp_http_client_set_header(client, "X-Sample-Bits", "16");
    esp_http_client_set_header(client, "X-Channels", "1");
    esp_http_client_set_header(client, "X-Recording-Mode", "stream");

    s->delivered = false;
    bool opened = esp_http_client_open(client, -1) == ESP_OK;   // -1 => chunked
    s->opened = opened;   // 供录音任务判断连接是否已就绪
    // 分块上传时不能在这里 fetch_headers:此时请求体尚未发出,服务器不会立刻回响应,
    // 该调用会一直阻塞到超时,期间采集侧写入全部堆积(表现为"上传缓冲拥塞 0/N")。
    // 响应头在收尾阶段(发完数据后)再读。
    char *chunk = heap_caps_malloc(BADGE_CHUNK_SAMPLES * 2, MALLOC_CAP_8BIT);
    // 整帧缓冲:分块头(最多 15 字节)+ PCM + 分块尾 CRLF,一次写出。
    char *frame = heap_caps_malloc(BADGE_CHUNK_SAMPLES * 2 + 16, MALLOC_CAP_8BIT);
    bool ok = opened && chunk && frame;
    if (!opened) {
        ESP_LOGE(TAG, "流式连接失败: %s", url);
    } else if (!chunk || !frame) {
        ESP_LOGE(TAG, "流式缓冲分配失败");
    } else {
        ESP_LOGI(TAG, "流式上传已开始: %s", url);
        // 诊断:录音期间 Wi-Fi/lwIP 需要内部 RAM,余量不足会导致发送长期不推进。
        ESP_LOGI(TAG, "流式开始时: 空闲堆 %u, 内部最大块 %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    while (ok) {
        size_t got = xStreamBufferReceive(s->buf, chunk,
                                          BADGE_CHUNK_SAMPLES * 2,
                                          pdMS_TO_TICKS(100));
        if (got > 0) {
            if (!stream_write_chunk(client, frame, chunk, got)) {
                ESP_LOGW(TAG, "流式写入失败(网络中断)");
                ok = false;
                break;
            }
            s->sent_bytes += got;
        }
        if (s->stop) {
            if (!s->discard) {
                // 收尾:排干缓冲剩余数据,再发结束块 0\r\n\r\n,服务器据此判定上传完成。
                while ((got = xStreamBufferReceive(s->buf, chunk,
                                                   BADGE_CHUNK_SAMPLES * 2, 0)) > 0) {
                    if (!stream_write_chunk(client, frame, chunk, got)) {
                        ok = false;
                        break;
                    }
                    s->sent_bytes += got;
                }
                if (ok && esp_http_client_write(client, "0\r\n\r\n", 5) == 5) {
                    esp_http_client_fetch_headers(client);
                    int status = esp_http_client_get_status_code(client);
                    s->delivered = (status >= 200 && status < 300);
                    ESP_LOGI(TAG, "流式收尾: sent=%u HTTP=%d",
                             (unsigned)s->sent_bytes, status);
                }
            } else {
                // 丢弃:不发结束块,直接断开,服务器按不完整上传处理。
                ESP_LOGI(TAG, "录音取消:丢弃已缓冲数据");
            }
            break;
        }
    }

    if (chunk) free(chunk);
    if (frame) free(frame);
    if (opened) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (!opened) ESP_LOGE(TAG, "流式会话连接失败");

    vStreamBufferDelete(s->buf);
    s->buf = NULL;
    s->running = false;
    xSemaphoreGive(s->done);
    vTaskDelete(NULL);
}

esp_err_t badge_upload_stream_begin(void)
{
    if (xSemaphoreTake(s_up.session_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;   // 已有会话
    }
    // done 信号量跨会话复用:流式任务可能因网络中断自行退出,那时调用方无法
    // 安全删除该信号量(任务可能正好在 give),所以只在首次创建,不随会话销毁。
    SemaphoreHandle_t done = s_up.session.done;
    memset(&s_up.session, 0, sizeof(s_up.session));
    s_up.session.done = done ? done : xSemaphoreCreateBinary();
    if (s_up.session.done) {
        xSemaphoreTake(s_up.session.done, 0);   // 清掉上次会话可能残留的令牌
    }
    s_up.session.buf = xStreamBufferCreate(BADGE_STREAM_BUFFER_BYTES, 1);
    if (!s_up.session.buf || !s_up.session.done) {
        if (s_up.session.buf) vStreamBufferDelete(s_up.session.buf);
        if (s_up.session.done) vSemaphoreDelete(s_up.session.done);
        s_up.session.buf = NULL;
        s_up.session.done = NULL;
        xSemaphoreGive(s_up.session_lock);
        return ESP_ERR_NO_MEM;
    }
    s_up.session.running = true;
    if (xTaskCreate(stream_task, "badge_stream", 6144, NULL, 4,
                    &s_up.session.task) != pdPASS) {
        vStreamBufferDelete(s_up.session.buf);
        vSemaphoreDelete(s_up.session.done);
        s_up.session.buf = NULL;
        s_up.session.done = NULL;
        s_up.session.running = false;
        xSemaphoreGive(s_up.session_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t badge_upload_stream_write(const void *pcm, size_t bytes)
{
    stream_session_t *s = &s_up.session;
    if (!s->running || !s->buf) return ESP_ERR_INVALID_STATE;
    size_t sent = xStreamBufferSend(s->buf, pcm, bytes,
                                    pdMS_TO_TICKS(STREAM_WRITE_TIMEOUT_MS));
    if (sent != bytes) {
        ESP_LOGW(TAG, "上传缓冲拥塞: %u/%u 字节", (unsigned)sent, (unsigned)bytes);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t badge_upload_stream_end(bool discard)
{
    stream_session_t *s = &s_up.session;
    if (!s->running) {
        // 会话已自行结束(如网络中断导致流式任务退出):必须交还会话锁,
        // 否则后续再也建立不了新会话,断网恢复后无法续传。
        s->task = NULL;
        xSemaphoreGive(s_up.session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s->stop = true;
    s->discard = discard;
    bool finished = xSemaphoreTake(s->done, pdMS_TO_TICKS(15000)) == pdTRUE;
    // 收尾工作(删除流缓冲、置 running=false)由流式任务自己完成后以 vTaskDelete(NULL)
    // 退出。调用方不能再 vTaskDelete(s->task),否则同一个 TCB 被释放两次,
    // 空闲任务清理终止链表时会 uxListRemove 崩溃(Guru Meditation: Load access fault)。
    // done 信号量跨会话复用,不在此删除(见 badge_upload_stream_begin)。
    s->task = NULL;
    xSemaphoreGive(s_up.session_lock);
    if (!finished) {
        ESP_LOGE(TAG, "流式收尾超时");
        return ESP_ERR_TIMEOUT;
    }
    return s->delivered || discard ? ESP_OK : ESP_FAIL;
}

bool badge_upload_stream_delivered(void)
{
    return s_up.session.delivered;
}

int badge_upload_stream_state(void)
{
    if (!s_up.session.running) return 0;
    return s_up.session.opened ? 2 : 1;
}

// --- 常驻同步 worker ---

static void sync_worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t wake = 0;
        if (xQueueReceive(s_up.sync_queue, &wake, portMAX_DELAY) != pdTRUE) continue;
        if (!badge_wifi_net_up()) continue;

        int files = badge_store_count();
        if (files > 0) {
            int uploaded = replay_pending_files();
            ESP_LOGI(TAG, "补传: %d/%d 个文件", uploaded, files);
        }
        tasks_fetch_once();
    }
}

esp_err_t badge_upload_init(void)
{
    if (s_up.inited) return ESP_OK;
    memset(&s_up, 0, sizeof(s_up));
    device_id_init();
    s_up.session_lock = xSemaphoreCreateBinary();
    xSemaphoreGive(s_up.session_lock);
    s_up.sync_queue = xQueueCreate(1, sizeof(uint32_t));
    if (!s_up.sync_queue || !s_up.session_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sync_worker, "badge_sync", 6144, NULL, 3,
                    &s_up.worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_up.inited = true;
    ESP_LOGI(TAG, "同步 worker 就绪, 设备 ID %s", s_up.device_id);
    return ESP_OK;
}

esp_err_t badge_upload_request_sync(void)
{
    if (!s_up.inited) return ESP_ERR_INVALID_STATE;
    uint32_t wake = 1;
    xQueueSend(s_up.sync_queue, &wake, 0);   // 队列满 = 正在同步,合并
    return ESP_OK;
}
