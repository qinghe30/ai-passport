// main/badge_settings.c —— 运行时配置的读写实现(NVS 持久化)。
//
// 为什么需要运行时配置:固件发布到玩法社区后,后台地址因使用者不同而不同,
// 不能只靠 badge_config.h 的编译期常量。配置网页(见 badge_setup.c)写入 NVS,
// 上传/任务拉取在每次请求前读取当前值。
#include "badge_settings.h"

#include "badge_config.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "badge_settings";

#define SETTINGS_NAMESPACE "badge_cfg"
#define KEY_SERVER_URL     "srv_url"

static char s_server_url[BADGE_SERVER_URL_MAX] = BADGE_SERVER_URL;
static bool s_inited;

static void normalize(const char *src, char *dst, unsigned size)
{
    snprintf(dst, size, "%s", src);
    // 去掉结尾斜杠,后续拼接路径时不会出现 "//api/..."。
    size_t n = strlen(dst);
    while (n > 0 && dst[n - 1] == '/') {
        dst[--n] = '\0';
    }
}

bool badge_settings_url_valid(const char *url)
{
    if (!url) return false;
    size_t len = strlen(url);
    // 最短合法值形如 "http://a"(9 字节)。
    if (len < 9 || len >= BADGE_SERVER_URL_MAX) return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)url[i];
        // 空格与控制字符会让 HTTP 客户端解析失败,直接拒收。
        if (c <= 0x20 || c == 0x7F) return false;
    }
    return true;
}

esp_err_t badge_settings_init(void)
{
    if (s_inited) return ESP_OK;

    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // 命名空间不存在 = 还没配置过任何自定义值,使用编译期默认地址。
        s_inited = true;
        ESP_LOGI(TAG, "后台地址使用默认值: %s", s_server_url);
        return err;
    }
    char buf[BADGE_SERVER_URL_MAX];
    size_t len = sizeof(buf);
    err = nvs_get_str(h, KEY_SERVER_URL, buf, &len);
    nvs_close(h);
    if (err == ESP_OK && badge_settings_url_valid(buf)) {
        normalize(buf, s_server_url, sizeof(s_server_url));
        ESP_LOGI(TAG, "后台地址来自 NVS: %s", s_server_url);
    } else {
        ESP_LOGI(TAG, "后台地址使用默认值: %s", s_server_url);
    }
    s_inited = true;
    return ESP_OK;
}

const char *badge_settings_server_url(void)
{
    return s_server_url;
}

esp_err_t badge_settings_set_server_url(const char *url)
{
    if (!badge_settings_url_valid(url)) return ESP_ERR_INVALID_ARG;

    char norm[BADGE_SERVER_URL_MAX];
    normalize(url, norm, sizeof(norm));

    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存后台地址失败, NVS 打开出错: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, KEY_SERVER_URL, norm);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存后台地址失败: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(s_server_url, sizeof(s_server_url), "%s", norm);
    ESP_LOGI(TAG, "后台地址已更新: %s", s_server_url);
    return ESP_OK;
}
