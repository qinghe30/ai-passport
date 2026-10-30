// main/badge_settings.h —— 运行时配置(保存在 NVS,可由配置网页修改)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 后台地址最长字节数(含结尾 NUL)。该值同时决定 HTTP 客户端 URL 缓冲区上限。
#define BADGE_SERVER_URL_MAX 96

// 从 NVS 读取配置。NVS 中没有保存过时使用 badge_config.h 的编译期默认值。
// 须在 NVS 初始化之后调用(本工程由 badge_wifi_init 完成)。
esp_err_t badge_settings_init(void);

// 当前后台地址(不带结尾斜杠),永不返回 NULL。
const char *badge_settings_server_url(void);

// 校验并保存后台地址(只接受 http:// 或 https:// 开头,保存时去掉结尾斜杠)。
esp_err_t badge_settings_set_server_url(const char *url);

// URL 合法性检查(供配置页表单校验使用)。
bool badge_settings_url_valid(const char *url);
