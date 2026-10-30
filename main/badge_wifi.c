// main/badge_wifi.c —— Wi-Fi STA + BluFi 配网实现。
// 结构提取自 demo/blufi-provisioning 参考实现,差异:
//   1. STA 常驻(应用级服务),不随页面销毁;断线带退避自动重连。
//   2. BLE host 仅在配网期间启用,退出即回收,为录音/Wi-Fi 释放内存。
//   3. 状态以原子枚举暴露,UI/任务轮询,避免跨任务回调。
#include "badge_wifi.h"

#include "badge_blufi_security.h"
#include "badge_config.h"
#include "demo_radio.h"

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "lwip/ip4_addr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include <string.h>

static const char *TAG = "badge_wifi";

// 断线重连退避:前 3 次立即重试,之后每 10 秒一次(esp_timer 回调里触发)。
#define WIFI_RETRY_FAST   3
#define WIFI_RETRY_MS     10 * 1000

typedef struct {
    bool wifi_initialized;
    bool wifi_started;
    bool handler_registered;
    esp_netif_t *sta_netif;

    // BLE 配网生命周期
    bool host_initialized;
    bool host_running;
    bool gatt_initialized;
    bool btc_initialized;
    bool profile_initialized;
    bool ble_connected;
    SemaphoreHandle_t host_stopped;

    wifi_config_t sta_config;
    char ip[16];
    volatile badge_wifi_state_t state;
    volatile bool reconnect_after_disconnect;  // 换网时先断开再连,抑制误报
    volatile unsigned retry_count;
    esp_timer_handle_t retry_timer;
} badge_wifi_t;

static badge_wifi_t s_wifi;

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_wifi.wifi_started && s_wifi.state == BADGE_WIFI_CONNECTING) {
        ESP_LOGI(TAG, "重连定时器触发");
        esp_wifi_connect();
    }
}

static void schedule_reconnect(void)
{
    s_wifi.retry_count++;
    if (s_wifi.retry_count <= WIFI_RETRY_FAST) {
        esp_wifi_connect();
        return;
    }
    if (!s_wifi.retry_timer) {
        const esp_timer_create_args_t args = {
            .callback = retry_timer_cb,
            .name = "wifi_retry",
        };
        if (esp_timer_create(&args, &s_wifi.retry_timer) == ESP_OK) {
            esp_timer_start_periodic(s_wifi.retry_timer, WIFI_RETRY_MS * 1000ULL);
        }
    } else {
        esp_timer_start_periodic(s_wifi.retry_timer, WIFI_RETRY_MS * 1000ULL);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        // 启动即读取已存凭据,有则自动连接(与 BLUFI 写入路径共用一套配置)。
        esp_wifi_get_config(WIFI_IF_STA, &s_wifi.sta_config);
        if (s_wifi.sta_config.sta.ssid[0] != '\0') {
            s_wifi.state = BADGE_WIFI_CONNECTING;
            esp_wifi_connect();
        } else {
            s_wifi.state = BADGE_WIFI_NO_CRED;
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi 断开, reason=%u", event->reason);
        if (s_wifi.reconnect_after_disconnect) {
            // 换网流程中的主动断开,直接重连新凭据。
            s_wifi.reconnect_after_disconnect = false;
            esp_wifi_connect();
            return;
        }
        s_wifi.ip[0] = '\0';
        if (esp_wifi_get_config(WIFI_IF_STA, &s_wifi.sta_config) == ESP_OK &&
            s_wifi.sta_config.sta.ssid[0] != '\0') {
            s_wifi.state = BADGE_WIFI_CONNECTING;
            schedule_reconnect();
        } else {
            s_wifi.state = BADGE_WIFI_NO_CRED;
        }
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *event = data;
    snprintf(s_wifi.ip, sizeof(s_wifi.ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi.retry_count = 0;
    if (s_wifi.retry_timer) esp_timer_stop(s_wifi.retry_timer);
    s_wifi.state = BADGE_WIFI_CONNECTED;
    ESP_LOGI(TAG, "Wi-Fi 已连接, IP=%s", s_wifi.ip);
}

// --- BLUFI 配网回调(与参考实现一致,状态写回应用枚举) ---

static void send_wifi_report(esp_blufi_sta_conn_state_t state)
{
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = { 0 };
    size_t ssid_len = strnlen((const char *)s_wifi.sta_config.sta.ssid,
                              sizeof(s_wifi.sta_config.sta.ssid));
    if (ssid_len > 0) {
        info.sta_ssid = s_wifi.sta_config.sta.ssid;
        info.sta_ssid_len = ssid_len;
    }
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

static void blufi_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 复位: %d", reason);
    if (s_wifi.state == BADGE_WIFI_PROVISIONING) {
        s_wifi.state = BADGE_WIFI_ERROR;
    }
}

static void blufi_sync(void)
{
    int rc = esp_blufi_profile_init();
    ESP_LOGI(TAG, "blufi_sync: profile_init rc=%d, 空闲堆 %u 字节",
             rc, (unsigned)esp_get_free_heap_size());
    if (rc == 0) {
        s_wifi.profile_initialized = true;
    } else if (s_wifi.state == BADGE_WIFI_PROVISIONING) {
        s_wifi.state = BADGE_WIFI_ERROR;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_wifi.host_stopped) xSemaphoreGive(s_wifi.host_stopped);
    nimble_port_freertos_deinit();
}

static void request_wifi_connect(void)
{
    bool was_connected = (s_wifi.state == BADGE_WIFI_CONNECTED);
    if (was_connected) {
        s_wifi.reconnect_after_disconnect = true;
        if (esp_wifi_disconnect() == ESP_OK) return;
        s_wifi.reconnect_after_disconnect = false;
    }
    s_wifi.retry_count = 0;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
    } else {
        s_wifi.state = BADGE_WIFI_CONNECTING;
    }
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    // 诊断:打印所有 BluFi 事件,便于确认小程序连接与配网数据传输。
    ESP_LOGI(TAG, "BLUFI 事件: %d", (int)event);
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "BLUFI 初始化完成,开始广播 %s", BADGE_BLE_NAME);
        esp_blufi_adv_start_with_name(BADGE_BLE_NAME);
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "BLUFI 手机已连接");
        s_wifi.ble_connected = true;
        esp_blufi_adv_stop();
        if (badge_blufi_security_init() != 0) {
            ESP_LOGE(TAG, "BLUFI 安全模块初始化失败");
        }
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_wifi.ble_connected = false;
        badge_blufi_security_deinit();
        esp_blufi_adv_start_with_name(BADGE_BLE_NAME);
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        esp_wifi_set_mode(WIFI_MODE_STA);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(s_wifi.sta_config.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(&s_wifi.sta_config, 0, sizeof(s_wifi.sta_config));
        s_wifi.sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        memcpy(s_wifi.sta_config.sta.ssid, param->sta_ssid.ssid,
               param->sta_ssid.ssid_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_wifi.sta_config);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(s_wifi.sta_config.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_wifi.sta_config.sta.password, 0,
               sizeof(s_wifi.sta_config.sta.password));
        memcpy(s_wifi.sta_config.sta.password, param->sta_passwd.passwd,
               param->sta_passwd.passwd_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_wifi.sta_config);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        request_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (s_wifi.state == BADGE_WIFI_CONNECTED) {
            send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
        } else if (s_wifi.state == BADGE_WIFI_CONNECTING) {
            send_wifi_report(ESP_BLUFI_STA_CONNECTING);
        } else {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(param->report_error.state);
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = badge_blufi_negotiate,
    .encrypt_func = badge_blufi_encrypt,
    .decrypt_func = badge_blufi_decrypt,
    .checksum_func = badge_blufi_checksum,
};

esp_err_t badge_wifi_init(void)
{
    if (s_wifi.wifi_initialized) return ESP_OK;
    memset(&s_wifi, 0, sizeof(s_wifi));

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;

    // 使用带检查步骤的创建流程,避免便利接口在内存不足时直接 abort。
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_wifi.sta_netif = esp_netif_new(&netif_cfg);
    if (!s_wifi.sta_netif) return ESP_ERR_NO_MEM;
    err = esp_netif_attach_wifi_station(s_wifi.sta_netif);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    s_wifi.wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event, NULL, NULL);
    if (err != ESP_OK) return err;
    s_wifi.handler_registered = true;

    // WIFI_STORAGE_FLASH:BLUFI/手动 set_config 写入的凭据自动存 NVS,重启仍有效。
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) {
        // 关闭 Wi-Fi 省电:省电模式下收发要等 DTIM 唤醒,录音期间 TCP 发送
        // 会长时间等不到 ACK(实测发出约 6KB 后 send 卡死 10 秒)。
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_wifi.wifi_started = true;
        s_wifi.state = BADGE_WIFI_CONNECTING;   // STA_START 事件里会修正实际状态
    } else {
        s_wifi.state = BADGE_WIFI_ERROR;
    }
    return err;
}

badge_wifi_state_t badge_wifi_state(void)
{
    return s_wifi.state;
}

bool badge_wifi_net_up(void)
{
    return s_wifi.state == BADGE_WIFI_CONNECTED;
}

bool badge_wifi_has_credentials(void)
{
    wifi_config_t cfg;
    if (!s_wifi.wifi_initialized) return false;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    return cfg.sta.ssid[0] != '\0';
}

void badge_wifi_ip(char *buf, unsigned size)
{
    if (!buf || size == 0) return;
    snprintf(buf, size, "%s", s_wifi.ip);
}

void badge_wifi_ssid(char *buf, unsigned size)
{
    if (!buf || size == 0) return;
    wifi_config_t cfg;
    buf[0] = '\0';
    if (!s_wifi.wifi_initialized) return;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return;
    snprintf(buf, size, "%s", (const char *)cfg.sta.ssid);
}

esp_err_t badge_wifi_provisioning_start(void)
{
    if (!s_wifi.wifi_initialized) return ESP_ERR_INVALID_STATE;
    if (s_wifi.host_initialized) return ESP_OK;   // 已在配网中

    // 诊断:C3 堆内存紧张,NimBLE host 启动前记录余量。
    ESP_LOGI(TAG, "BLUFI 启动前空闲堆: %u 字节", (unsigned)esp_get_free_heap_size());

    esp_err_t err = esp_blufi_register_callbacks(&s_blufi_callbacks);
    ESP_LOGI(TAG, "blufi 回调注册: %s", esp_err_to_name(err));
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    ESP_LOGI(TAG, "nimble_port_init: %s, 空闲堆 %u 字节",
             esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
    if (err != ESP_OK) return err;
    s_wifi.host_initialized = true;
    s_wifi.host_stopped = xSemaphoreCreateBinary();
    if (!s_wifi.host_stopped) return ESP_ERR_NO_MEM;
    ble_hs_cfg.reset_cb = blufi_reset;
    ble_hs_cfg.sync_cb = blufi_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    int rc = esp_blufi_gatt_svr_init();
    ESP_LOGI(TAG, "gatt_svr_init rc=%d, 空闲堆 %u 字节",
             rc, (unsigned)esp_get_free_heap_size());
    if (rc != 0) return ESP_FAIL;
    s_wifi.gatt_initialized = true;
    rc = ble_svc_gap_device_name_set(BADGE_BLE_NAME);
    ESP_LOGI(TAG, "gap 设备名设置 rc=%d", rc);
    if (rc != 0) return ESP_FAIL;
    esp_blufi_btc_init();
    s_wifi.btc_initialized = true;
    err = esp_nimble_enable(host_task);
    ESP_LOGI(TAG, "esp_nimble_enable: %s, 空闲堆 %u 字节",
             esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
    if (err == ESP_OK) {
        s_wifi.host_running = true;
        s_wifi.state = BADGE_WIFI_PROVISIONING;
    }
    return err;
}

esp_err_t badge_wifi_provisioning_stop(void)
{
    if (!s_wifi.host_initialized) return ESP_OK;
    s_wifi.ble_connected = false;
    badge_blufi_security_deinit();
    if (s_wifi.profile_initialized) esp_blufi_adv_stop();
    if (s_wifi.gatt_initialized) {
        esp_blufi_gatt_svr_deinit();
        s_wifi.gatt_initialized = false;
    }
    bool host_stopped = !s_wifi.host_running;
    if (s_wifi.host_running) {
        int rc = nimble_port_stop();
        if (rc == 0) {
            xSemaphoreTake(s_wifi.host_stopped, portMAX_DELAY);
            host_stopped = true;
        } else if (rc == BLE_HS_EALREADY) {
            // 主机侧已处于停止状态(ble_hs 返回"已完成"),此前的 host 已不可用。
            // 这里必须继续走 nimble_port_deinit,否则控制器保持初始化状态,
            // 下次进入配网会报 controller init failed / ESP_ERR_INVALID_STATE。
            ESP_LOGW(TAG, "nimble 主机已停止,直接反初始化");
            host_stopped = true;
        } else {
            ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
        }
    }
    if (host_stopped) nimble_port_deinit();
    s_wifi.host_running = false;
    if (s_wifi.profile_initialized) {
        esp_blufi_profile_deinit();
        s_wifi.profile_initialized = false;
    }
    if (s_wifi.btc_initialized) {
        esp_blufi_btc_deinit();
        s_wifi.btc_initialized = false;
    }
    s_wifi.host_initialized = false;
    if (s_wifi.host_stopped) {
        vSemaphoreDelete(s_wifi.host_stopped);
        s_wifi.host_stopped = NULL;
    }
    // 退出配网后按实际网络状态恢复展示。
    s_wifi.state = badge_wifi_net_up() ? BADGE_WIFI_CONNECTED
                 : (badge_wifi_has_credentials() ? BADGE_WIFI_CONNECTING
                                                 : BADGE_WIFI_NO_CRED);
    return ESP_OK;
}

esp_err_t badge_wifi_clear_credentials(void)
{
    if (!s_wifi.wifi_initialized) return ESP_ERR_INVALID_STATE;
    wifi_config_t empty = { 0 };
    s_wifi.reconnect_after_disconnect = false;
    s_wifi.retry_count = 0;
    if (s_wifi.retry_timer) esp_timer_stop(s_wifi.retry_timer);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    memset(&s_wifi.sta_config, 0, sizeof(s_wifi.sta_config));
    s_wifi.ip[0] = '\0';
    if (s_wifi.state == BADGE_WIFI_PROVISIONING) {
        // 配网中清凭据:保持广播,等待手机重新发送。
        return err;
    }
    s_wifi.state = BADGE_WIFI_NO_CRED;
    return err;
}

esp_err_t badge_wifi_apply_credentials(const char *ssid, const char *password)
{
    if (!s_wifi.wifi_initialized) return ESP_ERR_INVALID_STATE;
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= sizeof(s_wifi.sta_config.sta.ssid)) return ESP_ERR_INVALID_ARG;

    // 与 BluFi 写入路径保持同一套配置结构,阈值放到最低以兼容 WPA/WPA2/WPA3。
    memset(&s_wifi.sta_config, 0, sizeof(s_wifi.sta_config));
    s_wifi.sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    memcpy(s_wifi.sta_config.sta.ssid, ssid, strlen(ssid));
    if (password && password[0]) {
        if (strlen(password) >= sizeof(s_wifi.sta_config.sta.password)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(s_wifi.sta_config.sta.password, password, strlen(password));
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_wifi.sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 Wi-Fi 凭据失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "已写入 Wi-Fi 凭据, SSID=%s", ssid);
    request_wifi_connect();
    return ESP_OK;
}
