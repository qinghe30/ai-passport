// main/badge_wifi.h —— 数字工牌 Wi-Fi 服务:常驻 STA 连接 + 按需 BluFi 配网。
// Wi-Fi 凭据经 BLUFI(EspBlufi 微信小程序)写入,由 WIFI_STORAGE_FLASH 持久化在
// NVS;应用全程保活 STA,断线自动重连。BLE 仅在配网页打开时启用以省电。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Wi-Fi 服务状态(UI 直接轮询展示;状态机需要的"网络通/断"由 net_up 体现)。
typedef enum {
    BADGE_WIFI_IDLE = 0,      // 尚未初始化
    BADGE_WIFI_NO_CRED,       // 无凭据,等待配网
    BADGE_WIFI_CONNECTING,    // 有凭据,连接中
    BADGE_WIFI_CONNECTED,     // 已拿到 IP
    BADGE_WIFI_PROVISIONING,  // BluFi 配网中(BLE 广播)
    BADGE_WIFI_ERROR,         // 初始化失败,详见日志
} badge_wifi_state_t;

// 初始化 NVS/netif/事件循环并启动 STA。有已存凭据则自动连接,无凭据进入
// NO_CRED 等待配网。全程只需调用一次。
esp_err_t badge_wifi_init(void);

// 读取当前状态(无锁,内部为原子枚举;UI 定时器与各任务均可直接调用)。
badge_wifi_state_t badge_wifi_state(void);

// 是否已连接(拿到 IP)。录音落点与同步动作依赖此值。
bool badge_wifi_net_up(void);

// 是否存在已保存凭据(决定配网提示文案)。
bool badge_wifi_has_credentials(void);

// 当前 IP 字符串("x.x.x.x"),未连接返回空串。buf 至少 16 字节。
void badge_wifi_ip(char *buf, unsigned size);

// 当前 SSID(最长 32 字节,按 NUL 截断)。无凭据返回空串。
void badge_wifi_ssid(char *buf, unsigned size);

// 进入 BluFi 配网模式:启动 NimBLE + BLUFI 并开始广播 BADGE_BLE_NAME。
// 手机发送凭据后固件自动连接并上报状态;已连接时再次进入可更换网络。
esp_err_t badge_wifi_provisioning_start(void);

// 退出配网模式:停止广播并关闭 BLE host(Wi-Fi STA 不受影响)。
esp_err_t badge_wifi_provisioning_stop(void);

// 清除已保存凭据并断开连接(配网页长按触发的"重置网络"入口)。
esp_err_t badge_wifi_clear_credentials(void);

// 写入新的 Wi-Fi 凭据并立即发起连接(WIFI_STORAGE_FLASH 会同时持久化到 NVS)。
// 供配置网页提交表单时使用;password 为空表示开放网络。
esp_err_t badge_wifi_apply_credentials(const char *ssid, const char *password);
