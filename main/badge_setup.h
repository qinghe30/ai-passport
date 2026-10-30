// main/badge_setup.h —— 配置热点(SoftAP)与网页配置服务。
// 进入配网页时启动:工牌开热点 FoloBadge-XXXX,手机连上后打开 192.168.4.1
// 即可填写后台地址与 Wi-Fi;保存后写入 NVS 并重启生效。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 启动配置热点与网页服务(可在配网页反复进出,重复调用幂等)。
esp_err_t badge_setup_start(void);

// 停止网页服务与热点,恢复纯 STA 模式。
esp_err_t badge_setup_stop(void);

// 配置热点是否已启动。
bool badge_setup_running(void);

// 当前配置热点名称("FoloBadge-XXXX",XXXX 为 MAC 后两字节)。
void badge_setup_ap_ssid(char *buf, unsigned size);
