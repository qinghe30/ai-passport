// main/badge_setup.c —— 配置热点 + 网页配置页实现。
//
// 交互链路:配网页 -> 启动 APSTA 热点 -> 手机连热点 -> 浏览器打开 192.168.4.1
// -> 填后台地址(可选同时改 Wi-Fi)-> 保存进 NVS -> 重启生效。
// 为什么用 SoftAP 而不是蓝牙:社区用户刷完固件只需手机浏览器,不必额外装小程序。
#include "badge_setup.h"

#include "badge_config.h"
#include "badge_settings.h"
#include "badge_wifi.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "badge_setup";

#define AP_CHANNEL       6
#define AP_MAX_CONN      2
#define HTTP_BODY_MAX    256
#define PAGE_BUF_SIZE    2048
#define RESTART_DELAY_MS 2000

// 表单字段名(与网页 <input name> 一致)。
#define FIELD_URL  "url"
#define FIELD_SSID "ssid"
#define FIELD_PASS "pass"

static struct {
    bool started;
    esp_netif_t *ap_netif;
    httpd_handle_t httpd;
    char ssid[32];
} s_setup;

// 配置页返回的静态缓冲:httpd 为单任务回调,直接复用可省下 2 KB 栈。
static char s_page[PAGE_BUF_SIZE];

void badge_setup_ap_ssid(char *buf, unsigned size)
{
    if (!buf || size == 0) return;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, size, "%s-%02X%02X", BADGE_AP_PREFIX, mac[4], mac[5]);
}

// --- 表单解析 ---

// 把 %XX 与 '+' 还原成原始字节;src..end 是一个字段值的编码区间。
static void url_decode(const char *src, const char *end, char *out, unsigned out_size)
{
    unsigned o = 0;
    for (const char *p = src; p < end && o + 1 < out_size; p++) {
        if (*p == '+') {
            out[o++] = ' ';
        } else if (*p == '%' && p + 2 < end) {
            const char hi = p[1];
            const char lo = p[2];
            unsigned char v = 0;
            const bool hi_ok = (hi >= '0' && hi <= '9') || (hi >= 'a' && hi <= 'f') ||
                               (hi >= 'A' && hi <= 'F');
            const bool lo_ok = (lo >= '0' && lo <= '9') || (lo >= 'a' && lo <= 'f') ||
                               (lo >= 'A' && lo <= 'F');
            if (hi_ok && lo_ok) {
                v = (unsigned char)((hi <= '9' ? hi - '0' : (hi | 0x20) - 'a' + 10) << 4);
                v |= (unsigned char)(lo <= '9' ? lo - '0' : (lo | 0x20) - 'a' + 10);
                out[o++] = (char)v;
                p += 2;
            } else {
                out[o++] = *p;
            }
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

// 从 a=1&b=2 形式的表单体里取一个字段。字段不存在时 out 置空串。
static void form_value(const char *body, const char *key, char *out, unsigned out_size)
{
    out[0] = '\0';
    const size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > key_len && strncmp(p, key, key_len) == 0 &&
            p[key_len] == '=') {
            url_decode(p + key_len + 1, end, out, out_size);
            return;
        }
        p = (*end == '&') ? end + 1 : end;
    }
}

// 网页输出前转义,避免存进来的地址里的引号/尖括号破坏页面结构。
static void html_escape(const char *in, char *out, unsigned out_size)
{
    unsigned o = 0;
    for (const char *p = in; *p && o + 6 < out_size; p++) {
        if (*p == '&' || *p == '<' || *p == '>' || *p == '"' || *p == '\'') {
            const char *rep = (*p == '&') ? "&amp;"
                            : (*p == '<') ? "&lt;"
                            : (*p == '>') ? "&gt;"
                            : (*p == '"') ? "&quot;" : "&#39;";
            const unsigned n = (unsigned)strlen(rep);
            memcpy(out + o, rep, n);
            o += n;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

// --- HTTP 处理 ---

static esp_err_t root_get(httpd_req_t *req)
{
    char ssid[33];
    char ip[16];
    char esc_url[BADGE_SERVER_URL_MAX * 2];
    char esc_ssid[sizeof(ssid) * 2];

    html_escape(badge_settings_server_url(), esc_url, sizeof(esc_url));
    badge_wifi_ssid(ssid, sizeof(ssid));
    html_escape(ssid, esc_ssid, sizeof(esc_ssid));
    badge_wifi_ip(ip, sizeof(ip));

    snprintf(s_page, sizeof(s_page),
             "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>数字工牌配置</title><style>"
             "body{margin:0;background:#f5f7fa;color:#1d2129;"
             "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
             "main{max-width:420px;margin:0 auto;padding:24px 18px}"
             "h1{font-size:20px;margin:0 0 4px}p.sub{color:#6b7280;font-size:13px;margin:0 0 18px}"
             ".card{background:#fff;border-radius:14px;padding:18px;"
             "box-shadow:0 1px 3px rgba(16,24,40,.06)}"
             "label{display:block;font-size:13px;color:#4e5969;margin:14px 0 6px}"
             "input{width:100%%;box-sizing:border-box;padding:11px 12px;font-size:15px;"
             "border:1px solid #d9dee7;border-radius:10px;background:#fff}"
             "button{width:100%%;margin-top:22px;padding:13px;font-size:16px;color:#fff;"
             "background:#1f6feb;border:0;border-radius:10px}"
             ".tip{font-size:12px;color:#86909c;line-height:1.7;margin-top:16px}"
             "</style></head><body><main><h1>数字工牌</h1>"
             "<p class=\"sub\">配置后台地址与 Wi-Fi</p><div class=\"card\">"
             "<form method=\"post\" action=\"/save\">"
             "<label>后台地址</label>"
             "<input name=\"url\" value=\"%s\" placeholder=\"http://192.168.1.100:8000\">"
             "<label>Wi-Fi 名称</label>"
             "<input name=\"ssid\" value=\"%s\" placeholder=\"与工牌要连的 Wi-Fi\">"
             "<label>Wi-Fi 密码</label>"
             "<input name=\"pass\" type=\"password\" placeholder=\"开放式网络留空\">"
             "<button type=\"submit\">保存并重启</button></form></div>"
             "<p class=\"tip\">后台地址填运行 badge_server 的电脑局域网地址加端口,"
             "例如 http://192.168.1.100:8000。当前 Wi-Fi:%s IP:%s</p>"
             "</main></body></html>",
             esc_url, esc_ssid, ssid[0] ? ssid : "未配置", ip[0] ? ip : "未连接");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    ESP_LOGI(TAG, "配置已保存,重启生效");
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[HTTP_BODY_MAX];
    size_t want = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int n = want > 0 ? httpd_req_recv(req, body, want) : 0;
    if (n < 0) {
        // 读体失败(连接断开等):直接回 500,让用户重试。
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取表单失败");
        return ESP_FAIL;
    }
    body[n > 0 ? n : 0] = '\0';

    char url[BADGE_SERVER_URL_MAX];
    char ssid[33];
    char pass[65];
    form_value(body, FIELD_URL, url, sizeof(url));
    form_value(body, FIELD_SSID, ssid, sizeof(ssid));
    form_value(body, FIELD_PASS, pass, sizeof(pass));

    bool url_ok = true;
    if (url[0]) {
        url_ok = badge_settings_set_server_url(url) == ESP_OK;
    }
    if (ssid[0]) {
        esp_err_t err = badge_wifi_apply_credentials(ssid, pass);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "保存 Wi-Fi 失败: %s", esp_err_to_name(err));
        }
    }

    const char *title = url_ok ? "已保存,工牌正在重启" : "后台地址无效";
    const char *sub = url_ok
        ? "重启后工牌会连接新网络并开始上传录音,可以断开这个热点。"
        : "地址需以 http:// 或 https:// 开头,例如 http://192.168.1.100:8000,请返回修改。";

    snprintf(s_page, sizeof(s_page),
             "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>数字工牌</title><style>"
             "body{margin:0;background:#f5f7fa;color:#1d2129;"
             "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
             "main{max-width:420px;margin:0 auto;padding:40px 18px}"
             "h1{font-size:19px;margin:0 0 10px}p{color:#6b7280;font-size:14px;line-height:1.7}"
             "a{color:#1f6feb;font-size:14px}</style></head><body><main>"
             "<h1>%s</h1><p>%s</p><p><a href=\"/\">返回配置页</a></p>"
             "</main></body></html>",
             title, sub);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);

    if (url_ok) {
        xTaskCreate(restart_task, "badge_restart", 3072, NULL, 3, NULL);
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get,
    .user_ctx = NULL,
};

static const httpd_uri_t s_uri_save = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = save_post,
    .user_ctx = NULL,
};

// --- 生命周期 ---

esp_err_t badge_setup_start(void)
{
    if (s_setup.started) return ESP_OK;

    if (!s_setup.ap_netif) {
        s_setup.ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_setup.ap_netif) {
            ESP_LOGE(TAG, "创建热点 netif 失败(内存不足)");
            return ESP_ERR_NO_MEM;
        }
    }

    badge_setup_ap_ssid(s_setup.ssid, sizeof(s_setup.ssid));

    // APSTA:配置页期间保持原有 STA 连接,保存后由重启统一生效。
    wifi_config_t ap_cfg = { 0 };
    memcpy(ap_cfg.ap.ssid, s_setup.ssid, strlen(s_setup.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_setup.ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 5120;
    cfg.lru_purge_enable = true;
    err = httpd_start(&s_setup.httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置网页服务启动失败: %s", esp_err_to_name(err));
        s_setup.httpd = NULL;
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }
    httpd_register_uri_handler(s_setup.httpd, &s_uri_root);
    httpd_register_uri_handler(s_setup.httpd, &s_uri_save);

    s_setup.started = true;
    ESP_LOGI(TAG, "配置热点已开启: %s, 网页 http://192.168.4.1/", s_setup.ssid);
    return ESP_OK;
}

esp_err_t badge_setup_stop(void)
{
    if (!s_setup.started) return ESP_OK;

    if (s_setup.httpd) {
        httpd_stop(s_setup.httpd);
        s_setup.httpd = NULL;
    }
    // 先退回纯 STA 再销毁热点 netif,避免 AP 仍在工作时释放其 DHCP 服务。
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (s_setup.ap_netif) {
        esp_netif_destroy(s_setup.ap_netif);
        s_setup.ap_netif = NULL;
    }
    s_setup.started = false;
    ESP_LOGI(TAG, "配置热点已关闭");
    return ESP_OK;
}

bool badge_setup_running(void)
{
    return s_setup.started;
}
