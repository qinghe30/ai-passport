// main/badge_state.h —— 数字工牌应用状态机(纯逻辑,可 host 测试)。
// 只描述"页面 + 录音 + 网络"的合法转换,不含任何 ESP-IDF/LVGL 依赖;
// 副作用(开录音、停录音、触发同步)由应用层按返回的动作执行。
#pragma once

#include <stdbool.h>

// 应用页面。待机/录音/任务/配网四页,录音页承载录音状态展示。
typedef enum {
    BADGE_PAGE_HOME = 0,
    BADGE_PAGE_RECORD,
    BADGE_PAGE_TASKS,
    BADGE_PAGE_PROVISION,
} badge_page_t;

// 录音子状态。
typedef enum {
    BADGE_REC_IDLE = 0,      // 未录音
    BADGE_REC_STREAMING,     // 录音中:实时流式上传
    BADGE_REC_OFFLINE,       // 录音中:离线写 Flash 缓存
    BADGE_REC_SAVING,        // 已停止:等待收尾(上传结束/文件落盘)后回待机
} badge_rec_state_t;

// 输入事件(按键经应用层转换,网络/收尾由各任务上报)。
typedef enum {
    BADGE_EV_KEY_OK_CLICK = 0,
    BADGE_EV_KEY_OK_LONG,
    BADGE_EV_KEY_UP_CLICK,
    BADGE_EV_KEY_DOWN_CLICK,
    BADGE_EV_NET_UP,
    BADGE_EV_NET_DOWN,
    BADGE_EV_REC_SAVED,      // 一次录音收尾完成(无论保存还是丢弃)
} badge_event_t;

// 状态机要求应用层执行的动作。
typedef enum {
    BADGE_ACT_NONE = 0,
    BADGE_ACT_START_REC,     // 开始录音(实时/离线按当前网络状态决定)
    BADGE_ACT_STOP_REC,      // 停止录音并保存
    BADGE_ACT_CANCEL_REC,    // 停止录音并丢弃本次数据
    BADGE_ACT_SYNC_NOW,      // 触发离线文件补传 + 任务列表刷新
} badge_action_t;

// 应用状态模型(可整体快照、逐步驱动)。
typedef struct {
    badge_page_t page;
    badge_rec_state_t rec;
    bool net_up;             // Wi-Fi 已连接(拿到 IP)
    bool has_creds;          // NVS 中已有 Wi-Fi 凭据(决定配网页是否可用)
} badge_model_t;

// 处理一个事件:变更模型并带回需要执行的动作。
// 返回 true 表示模型或动作有变化(应用层刷新 UI/执行副作用)。
bool badge_model_handle(badge_model_t *m, badge_event_t ev, badge_action_t *out);
