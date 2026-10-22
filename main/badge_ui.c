// main/badge_ui.c —— 数字工牌中文 UI。
// 结构:
//   - 四个静态页面(待机/录音/任务/配网),lv_screen_load 切换;
//   - 500ms 周期刷新:Wi-Fi 状态/电池/录音时长与红点/任务快照/toast 自动隐藏;
//   - 按键 -> badge_state 状态机 -> 动作(badge_record/badge_upload/badge_wifi);
//   - 中文字体:badge_font_16/20(见 assets/fonts)。16px 覆盖 GB2312 第一级
//     常用字,动态内容(任务标题/负责人/SSID)按此契约显示;20px 仅静态文案。
//     LV_SYMBOL_* 图标经字体描述符的 fallback(montserrat)渲染。
// 线程纪律:本模块函数可能运行在非 LVGL 任务(main 输入分发任务),所有 lv_*
// 调用都在 bsp_lvgl_lock 临界区内;周期回调运行在 LVGL 任务,无需加锁。
#include "badge_ui.h"

#include "bsp_display.h"
#include "bsp_battery.h"
#include "lvgl.h"

#include "badge_config.h"
#include "badge_state.h"
#include "badge_fmt.h"
#include "badge_wifi.h"
#include "badge_record.h"
#include "badge_upload.h"
#include "badge_store.h"
#include "badge_tasks.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "badge_ui";

// ---- 配色(深色底,圆角遮罩内留边) ----
#define C_BG      lv_color_hex(0x101418)
#define C_CARD    lv_color_hex(0x1C232B)
#define C_TEXT    lv_color_hex(0xE8EDF2)
#define C_SUB     lv_color_hex(0x8B97A3)
#define C_ACCENT  lv_color_hex(0x3D8BFF)
#define C_GREEN   lv_color_hex(0x3FB950)
#define C_AMBER   lv_color_hex(0xE3B341)
#define C_RED     lv_color_hex(0xE5484D)

// ---- 静态 UI 文案(同时维护于 _fontsrc/gen_symbols.py 的字体字符集) ----
#define T_TITLE        "沟通工牌"
#define T_START_HINT   "按 OK 开始录音"
#define T_NAV_HINT     "UP:任务   DOWN:配网"
#define T_SYNCED_FMT   "待同步 %d 条"
#define T_SAVED        "已保存"
#define T_DISCARD      "已丢弃"
#define T_REC_FAIL     "录音失败"
#define T_SYNCING      "正在同步..."
#define T_START_FAIL   "无法开始录音"
#define T_RECORDING    "正在录音"
#define T_STREAMING    "实时上传"
#define T_OFFLINE      "离线录音 稍后自动同步"
#define T_SAVING       "正在结束..."
#define T_REC_HINT     "OK 结束保存   UP 取消丢弃"
#define T_TASKS        "沟通任务"
#define T_TASKS_EMPTY  "暂无任务\n联网后自动同步"
#define T_LAST_SYNC    "上次同步: "
#define T_JUST_NOW     "刚刚"
#define T_MIN_AGO      " 分前"
#define T_HOUR_AGO     " 时前"
#define T_PROV_TITLE   "Wi-Fi 配网"
#define T_PROV_BACK    "长按 OK 返回"
#define T_CONN_FMT     "已连接\nSSID: %s\nIP: %s"
#define T_NO_CRED      "尚未配置 Wi-Fi\n请用微信小程序 EspBlufi\n搜索设备:\n"
#define T_CONNECTING   "正在连接 Wi-Fi..."
#define T_PROVISIONING "正在配网\n请用微信小程序 EspBlufi\n搜索设备:\n"
#define T_WIFI_ERROR   "网络异常,请重试"
#define T_WIFI_IDLE    "网络未启动"
#define T_PENDING_FMT  "%d 条待办"
#define T_PROV_FAIL    "配网启动失败"

// 中文字体(生成于 assets/fonts,fallback 到 montserrat 提供 LV_SYMBOL_* 图标)。
LV_FONT_DECLARE(badge_font_16);
LV_FONT_DECLARE(badge_font_20);
static lv_font_t s_f16;
static lv_font_t s_f20;

// ---- 页面与控件 ----
static lv_obj_t *s_screens[BADGE_PAGE_PROVISION + 1];
static lv_obj_t *s_wifi_icon[BADGE_PAGE_PROVISION + 1];   // 状态栏 Wi-Fi 图标
static lv_obj_t *s_batt[BADGE_PAGE_PROVISION + 1];        // 状态栏电量文本

static lv_obj_t *s_home_sync;        // 待机页"待同步 N 条"
static lv_obj_t *s_rec_dot;          // 录音红点
static lv_obj_t *s_rec_state;        // 录音状态(20px)
static lv_obj_t *s_rec_mode;         // 落点提示(16px)
static lv_obj_t *s_rec_time;         // 时长 MM:SS(20px)
static lv_obj_t *s_tasks_sub;        // "N 条待办"
static lv_obj_t *s_tasks_list;       // 任务行容器(可滚动)
static lv_obj_t *s_tasks_empty;      // 空态提示
static lv_obj_t *s_tasks_sync;       // "上次同步: xx"
static lv_obj_t *s_prov_info;        // 配网信息(多行)

static badge_model_t s_model;
static lv_timer_t *s_poller;

// ---- toast(各页一条,500ms 周期回调负责 2s 后隐藏) ----
static lv_obj_t *s_toast[BADGE_PAGE_PROVISION + 1];
static uint32_t s_toast_tick;

// ---- 刷新缓存(仅周期回调访问) ----
static int s_sig_batt = -1000;
static int s_sig_cached = -1;
static long long s_sig_fetch = -2;

// ---------------------------------------------------------------------------
// 基础构建 helpers
// ---------------------------------------------------------------------------
static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// 顶部状态栏:左 Wi-Fi 图标、右电量(所有页面一致)。
static void make_topbar(int idx)
{
    s_wifi_icon[idx] = make_label(s_screens[idx], &s_f16, C_SUB, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_icon[idx], LV_ALIGN_TOP_LEFT, 14, 12);
    s_batt[idx] = make_label(s_screens[idx], &s_f16, C_SUB, "--%");
    lv_obj_align(s_batt[idx], LV_ALIGN_TOP_RIGHT, -14, 12);
}

static void toast(int idx, const char *text)
{
    if (!s_toast[idx]) {
        s_toast[idx] = make_label(s_screens[idx], &s_f16, C_TEXT, text);
        lv_obj_set_style_bg_color(s_toast[idx], C_CARD, 0);
        lv_obj_set_style_bg_opa(s_toast[idx], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_toast[idx], 12, 0);
        lv_obj_set_style_pad_ver(s_toast[idx], 6, 0);
        lv_obj_set_style_radius(s_toast[idx], 10, 0);
        lv_obj_add_flag(s_toast[idx], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_toast[idx], text);
    lv_obj_align(s_toast[idx], LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_remove_flag(s_toast[idx], LV_OBJ_FLAG_HIDDEN);
    s_toast_tick = lv_tick_get();
}

// ---------------------------------------------------------------------------
// 页面构建
// ---------------------------------------------------------------------------
static void build_home(void)
{
    int i = BADGE_PAGE_HOME;
    s_screens[i] = make_screen();
    make_topbar(i);

    lv_obj_t *title = make_label(s_screens[i], &s_f20, C_TEXT, T_TITLE);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t *card = make_card(s_screens[i], 176, 138);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -14);

    // 录音圆钮:蓝底圆形 + 麦克风图标。
    lv_obj_t *knob = lv_obj_create(card);
    lv_obj_set_size(knob, 62, 62);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, C_ACCENT, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(knob, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t *mic = make_label(knob, &s_f20, C_TEXT, LV_SYMBOL_AUDIO);
    lv_obj_center(mic);

    lv_obj_t *hint = make_label(card, &s_f16, C_TEXT, T_START_HINT);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_home_sync = make_label(s_screens[i], &s_f16, C_AMBER, "");
    lv_obj_set_style_text_align(s_home_sync, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_home_sync, lv_pct(100));
    lv_obj_align(s_home_sync, LV_ALIGN_TOP_MID, 0, 214);
    lv_obj_add_flag(s_home_sync, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *nav = make_label(s_screens[i], &s_f16, C_SUB, T_NAV_HINT);
    lv_obj_set_style_text_align(nav, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(nav, lv_pct(100));
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, -18);
}

static void build_record(void)
{
    int i = BADGE_PAGE_RECORD;
    s_screens[i] = make_screen();
    make_topbar(i);

    // 红点(周期回调闪烁)。
    s_rec_dot = lv_obj_create(s_screens[i]);
    lv_obj_set_size(s_rec_dot, 20, 20);
    lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rec_dot, C_RED, 0);
    lv_obj_set_style_border_width(s_rec_dot, 0, 0);
    lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_rec_dot, LV_ALIGN_TOP_MID, 0, 52);

    s_rec_state = make_label(s_screens[i], &s_f20, C_TEXT, T_RECORDING);
    lv_obj_set_style_text_align(s_rec_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_rec_state, lv_pct(100));
    lv_obj_align(s_rec_state, LV_ALIGN_TOP_MID, 0, 88);

    s_rec_mode = make_label(s_screens[i], &s_f16, C_GREEN, T_STREAMING);
    lv_obj_set_style_text_align(s_rec_mode, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_rec_mode, lv_pct(100));
    lv_obj_align(s_rec_mode, LV_ALIGN_TOP_MID, 0, 122);

    s_rec_time = make_label(s_screens[i], &s_f20, C_TEXT, "00:00");
    lv_obj_set_style_text_align(s_rec_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_rec_time, lv_pct(100));
    lv_obj_align(s_rec_time, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *hint = make_label(s_screens[i], &s_f16, C_SUB, T_REC_HINT);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
}

// 单条任务行:第一行 [状态符号][标题],第二行 [负责人 · 截止]。
static void task_row(lv_obj_t *parent, const badge_task_t *t)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(row, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 2, 0);

    lv_obj_t *line = lv_obj_create(row);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(line, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, 6, 0);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    make_label(line, &s_f16, t->pending ? C_AMBER : C_GREEN,
               t->pending ? LV_SYMBOL_MINUS : LV_SYMBOL_OK);

    lv_obj_t *title = make_label(line, &s_f16, C_TEXT, t->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(title, 1);

    char meta[BADGE_TASK_ASSIGNEE_MAX + BADGE_TASK_DUE_MAX + 4];
    snprintf(meta, sizeof(meta), "%s %s", t->assignee, t->due);
    lv_obj_t *m = make_label(row, &s_f16, C_SUB, meta);
    lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
    lv_obj_set_width(m, lv_pct(100));
}

// 按当前快照重建任务列表(仅在快照签名变化或进页时调用)。
static void render_tasks(void)
{
    badge_task_t snap[BADGE_TASK_MAX];
    int n = badge_tasks_snapshot(snap, BADGE_TASK_MAX);
    int pending = badge_tasks_pending();

    char buf[32];
    snprintf(buf, sizeof(buf), T_PENDING_FMT, pending);
    lv_label_set_text(s_tasks_sub, buf);

    lv_obj_clean(s_tasks_list);
    if (n == 0) {
        lv_obj_add_flag(s_tasks_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_tasks_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_tasks_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tasks_empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < n; i++) {
        task_row(s_tasks_list, &snap[i]);
    }
}

static void render_sync_age(void)
{
    long long at = badge_tasks_fetched_at();
    if (at < 0) {
        lv_label_set_text(s_tasks_sync, T_LAST_SYNC "-");
        return;
    }
    long long elapsed = (long long)(lv_tick_get() / 1000) - at;
    if (elapsed < 0) {
        elapsed = 0;
    }
    char buf[24];
    if (elapsed < 60) {
        snprintf(buf, sizeof(buf), T_JUST_NOW);
    } else if (elapsed < 3600) {
        snprintf(buf, sizeof(buf), "%d" T_MIN_AGO, (int)(elapsed / 60));
    } else {
        snprintf(buf, sizeof(buf), "%d" T_HOUR_AGO, (int)(elapsed / 3600));
    }
    // 拼上前缀(T_LAST_SYNC 以冒号+空格结尾)。
    char out[40];
    snprintf(out, sizeof(out), T_LAST_SYNC "%s", buf);
    lv_label_set_text(s_tasks_sync, out);
}

static void build_tasks(void)
{
    int i = BADGE_PAGE_TASKS;
    s_screens[i] = make_screen();
    make_topbar(i);

    lv_obj_t *title = make_label(s_screens[i], &s_f20, C_TEXT, T_TASKS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    s_tasks_sub = make_label(s_screens[i], &s_f16, C_AMBER, "");
    lv_obj_set_style_text_align(s_tasks_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tasks_sub, lv_pct(100));
    lv_obj_align(s_tasks_sub, LV_ALIGN_TOP_MID, 0, 78);

    s_tasks_list = lv_obj_create(s_screens[i]);
    lv_obj_set_size(s_tasks_list, lv_pct(90), 178);
    lv_obj_align(s_tasks_list, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(s_tasks_list, C_CARD, 0);
    lv_obj_set_style_radius(s_tasks_list, 14, 0);
    lv_obj_set_style_border_width(s_tasks_list, 0, 0);
    lv_obj_set_style_pad_all(s_tasks_list, 8, 0);
    lv_obj_set_style_layout(s_tasks_list, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(s_tasks_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tasks_list, 6, 0);

    s_tasks_empty = make_label(s_screens[i], &s_f16, C_SUB, T_TASKS_EMPTY);
    lv_obj_set_style_text_align(s_tasks_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tasks_empty, lv_pct(90));
    lv_obj_align(s_tasks_empty, LV_ALIGN_TOP_MID, 0, 170);

    s_tasks_sync = make_label(s_screens[i], &s_f16, C_SUB, T_LAST_SYNC "-");
    lv_obj_set_style_text_align(s_tasks_sync, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tasks_sync, lv_pct(100));
    lv_obj_align(s_tasks_sync, LV_ALIGN_BOTTOM_MID, 0, -18);

    render_tasks();
    render_sync_age();
}

static void build_provision(void)
{
    int i = BADGE_PAGE_PROVISION;
    s_screens[i] = make_screen();
    make_topbar(i);

    lv_obj_t *title = make_label(s_screens[i], &s_f20, C_TEXT, T_PROV_TITLE);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    s_prov_info = make_label(s_screens[i], &s_f16, C_TEXT, "");
    lv_obj_set_style_text_align(s_prov_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_prov_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_prov_info, lv_pct(84));

    lv_obj_t *back = make_label(s_screens[i], &s_f16, C_SUB, T_PROV_BACK);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(back, lv_pct(100));
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -18);
}

// ---------------------------------------------------------------------------
// 状态栏与页面刷新
// ---------------------------------------------------------------------------
static void render_status_bar(int idx, bool up, int soc)
{
    lv_obj_set_style_text_color(s_wifi_icon[idx], up ? C_GREEN : C_SUB, 0);

    char buf[12];
    if (soc < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", soc);
    }
    lv_label_set_text(s_batt[idx], buf);
    lv_obj_set_style_text_color(s_batt[idx], (soc >= 0 && soc < 20) ? C_RED : C_SUB, 0);
}

static void render_record_live(void)
{
    // 以模型状态为准:SAVING 显示收尾;刚启动的瞬间采集任务可能尚未置位 active,
    // 此时按模型落点显示,避免闪现"正在结束"。
    if (s_model.rec == BADGE_REC_SAVING) {
        lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_rec_state, T_SAVING);
        lv_obj_set_style_text_color(s_rec_state, C_AMBER, 0);
        lv_label_set_text(s_rec_mode, "");
        return;
    }
    bool streaming = badge_record_active() ? badge_record_streaming()
                                           : (s_model.rec == BADGE_REC_STREAMING);
    lv_obj_set_style_text_color(s_rec_state, C_TEXT, 0);
    lv_label_set_text(s_rec_state, T_RECORDING);
    lv_label_set_text(s_rec_mode, streaming ? T_STREAMING : T_OFFLINE);
    lv_obj_set_style_text_color(s_rec_mode, streaming ? C_GREEN : C_AMBER, 0);

    char buf[8];
    badge_fmt_duration(badge_record_duration_ms(), buf, sizeof(buf));
    lv_label_set_text(s_rec_time, buf);
}

// 依据 badge_wifi_state 渲染配网信息(在 LVGL 上下文中调用)。
static void refresh_provision(void)
{
    char ssid[33];
    char ip[16];
    char name[40];
    char buf[160];
    lv_color_t color = C_TEXT;

    snprintf(name, sizeof(name), "%s", BADGE_BLE_NAME);

    switch (badge_wifi_state()) {
    case BADGE_WIFI_CONNECTED:
        badge_wifi_ssid(ssid, sizeof(ssid));
        badge_wifi_ip(ip, sizeof(ip));
        snprintf(buf, sizeof(buf), T_CONN_FMT, ssid, ip);
        color = C_GREEN;
        break;
    case BADGE_WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), T_CONNECTING);
        color = C_AMBER;
        break;
    case BADGE_WIFI_PROVISIONING:
        snprintf(buf, sizeof(buf), T_PROVISIONING "%s", name);
        break;
    case BADGE_WIFI_NO_CRED:
        snprintf(buf, sizeof(buf), T_NO_CRED "%s", name);
        color = C_AMBER;
        break;
    case BADGE_WIFI_ERROR:
        snprintf(buf, sizeof(buf), T_WIFI_ERROR);
        color = C_RED;
        break;
    default:
        snprintf(buf, sizeof(buf), T_WIFI_IDLE);
        color = C_SUB;
        break;
    }
    lv_obj_set_style_text_color(s_prov_info, color, 0);
    lv_label_set_text(s_prov_info, buf);
    lv_obj_align(s_prov_info, LV_ALIGN_TOP_MID, 0, 100);
}

// ---------------------------------------------------------------------------
// 周期刷新(运行于 LVGL 任务)
// ---------------------------------------------------------------------------
static void poll_cb(lv_timer_t *timer)
{
    (void)timer;

    // --- toast 自动隐藏 ---
    if (s_toast_tick && lv_tick_elaps(s_toast_tick) > 2000) {
        for (int i = 0; i <= BADGE_PAGE_PROVISION; i++) {
            if (s_toast[i]) {
                lv_obj_add_flag(s_toast[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_toast_tick = 0;
    }

    // --- 网络状态 ---
    bool up = badge_wifi_net_up();
    bool creds = badge_wifi_has_credentials();
    if (up != s_model.net_up) {
        badge_action_t act;
        badge_model_handle(&s_model, up ? BADGE_EV_NET_UP : BADGE_EV_NET_DOWN, &act);
        if (act == BADGE_ACT_SYNC_NOW) {
            badge_upload_request_sync();
        }
    }
    s_model.has_creds = creds;

    // --- 录音收尾检测(含录音意外提前结束的补救) ---
    bool act_now = badge_record_active();
    if (!act_now && s_model.rec != BADGE_REC_IDLE) {
        if (s_model.rec == BADGE_REC_STREAMING || s_model.rec == BADGE_REC_OFFLINE) {
            // 录音任务已自行结束(如音频初始化失败),补记为收尾。
            s_model.rec = BADGE_REC_SAVING;
        }
        if (s_model.rec == BADGE_REC_SAVING) {
            badge_action_t a;
            badge_model_handle(&s_model, BADGE_EV_REC_SAVED, &a);
            if (a == BADGE_ACT_SYNC_NOW) {
                badge_upload_request_sync();
            }
            toast(BADGE_PAGE_HOME,
                  badge_record_last_result() == ESP_OK ? T_SAVED : T_REC_FAIL);
            lv_screen_load(s_screens[s_model.page]);
            render_status_bar(s_model.page, badge_wifi_net_up(), bsp_battery_soc());
        }
    }

    // --- 电池(仅变化时更新文本) ---
    int soc = bsp_battery_soc();
    if (soc != s_sig_batt) {
        s_sig_batt = soc;
        for (int i = 0; i <= BADGE_PAGE_PROVISION; i++) {
            render_status_bar(i, badge_wifi_net_up(), soc);
        }
    }

    // --- Wi-Fi 图标 ---
    for (int i = 0; i <= BADGE_PAGE_PROVISION; i++) {
        lv_obj_set_style_text_color(s_wifi_icon[i], up ? C_GREEN : C_SUB, 0);
    }

    // --- 待机页:待同步条数 ---
    int cached = badge_store_count();
    if (cached != s_sig_cached) {
        s_sig_cached = cached;
        if (cached > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), T_SYNCED_FMT, cached);
            lv_label_set_text(s_home_sync, buf);
            lv_obj_remove_flag(s_home_sync, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_home_sync, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- 页面相关动态内容 ---
    badge_page_t page = s_model.page;
    if (page == BADGE_PAGE_RECORD) {
        // 红点闪烁(1Hz)。
        if (badge_record_active() && s_model.rec != BADGE_REC_SAVING) {
            if (lv_obj_has_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
            }
        }
        render_record_live();
    } else if (page == BADGE_PAGE_TASKS) {
        long long fetched = badge_tasks_fetched_at();
        if (fetched != s_sig_fetch) {
            s_sig_fetch = fetched;
            render_tasks();
        }
        render_sync_age();
    } else if (page == BADGE_PAGE_PROVISION) {
        refresh_provision();
    }
}

// ---------------------------------------------------------------------------
// 按键处理(main 输入分发任务调用)
// ---------------------------------------------------------------------------
static bool key_to_event(bsp_btn_t btn, bsp_btn_ev_t ev, badge_event_t *out)
{
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) {
        return false;
    }
    if (btn == BSP_BTN_OK) {
        *out = (ev == BSP_BTN_CLICK) ? BADGE_EV_KEY_OK_CLICK : BADGE_EV_KEY_OK_LONG;
        return true;
    }
    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        *out = BADGE_EV_KEY_UP_CLICK;
        return true;
    }
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        *out = BADGE_EV_KEY_DOWN_CLICK;
        return true;
    }
    return false;
}

// 页面切换与进入/离开钩子(须持 LVGL 锁调用;配网启停在锁外执行)。
static void enter_page(badge_page_t prev)
{
    if (prev == s_model.page) {
        return;
    }
    lv_screen_load(s_screens[s_model.page]);
    if (s_model.page == BADGE_PAGE_TASKS) {
        s_sig_fetch = -2;  // 强制下次刷新重建列表
    }
}

void badge_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    badge_event_t event;
    if (!key_to_event(btn, ev, &event)) {
        return;
    }

    badge_action_t action = BADGE_ACT_NONE;
    badge_page_t prev_page;
    bool changed;

    if (!bsp_lvgl_lock(1000)) {
        return;
    }
    prev_page = s_model.page;
    changed = badge_model_handle(&s_model, event, &action);
    if (changed) {
        if (prev_page != s_model.page) {
            enter_page(prev_page);
        }
        if (s_model.page == BADGE_PAGE_RECORD) {
            render_record_live();
        }
    }
    bsp_lvgl_unlock();

    if (!changed) {
        return;
    }

    // 页面进入/离开配网模式(BLE 启停较慢,在锁外执行)。
    if (s_model.page == BADGE_PAGE_PROVISION && prev_page != BADGE_PAGE_PROVISION) {
        if (badge_wifi_provisioning_start() != ESP_OK) {
            if (bsp_lvgl_lock(500)) {
                toast(BADGE_PAGE_PROVISION, T_PROV_FAIL);
                bsp_lvgl_unlock();
            }
        }
    } else if (s_model.page != BADGE_PAGE_PROVISION &&
               prev_page == BADGE_PAGE_PROVISION) {
        badge_wifi_provisioning_stop();
    }

    switch (action) {
    case BADGE_ACT_START_REC:
        if (badge_record_start(s_model.net_up) != ESP_OK) {
            // 启动失败:回退模型并提示(状态机已切到录音页,撤回待机)。
            if (bsp_lvgl_lock(1000)) {
                s_model.rec = BADGE_REC_IDLE;
                s_model.page = BADGE_PAGE_HOME;
                lv_screen_load(s_screens[s_model.page]);
                render_status_bar(BADGE_PAGE_HOME, badge_wifi_net_up(),
                                  bsp_battery_soc());
                toast(BADGE_PAGE_HOME, T_START_FAIL);
                bsp_lvgl_unlock();
            }
        }
        break;
    case BADGE_ACT_STOP_REC:
        // 阻塞至收尾完成;录音页已显示"正在结束...",后续转场由周期回调完成。
        badge_record_stop(false);
        break;
    case BADGE_ACT_CANCEL_REC:
        badge_record_stop(true);
        break;
    case BADGE_ACT_SYNC_NOW:
        badge_upload_request_sync();
        if (s_model.page == BADGE_PAGE_TASKS && bsp_lvgl_lock(500)) {
            toast(BADGE_PAGE_TASKS, T_SYNCING);
            bsp_lvgl_unlock();
        }
        break;
    default:
        break;
    }
}

void badge_ui_init(void)
{
    // 字体描述符:应用字体优先,montserrat 补充 LV_SYMBOL_* 图标。
    s_f16 = badge_font_16;
    s_f16.fallback = &lv_font_montserrat_16;
    s_f20 = badge_font_20;
    s_f20.fallback = &lv_font_montserrat_20;

    s_model.page = BADGE_PAGE_HOME;
    s_model.rec = BADGE_REC_IDLE;
    s_model.net_up = badge_wifi_net_up();
    s_model.has_creds = badge_wifi_has_credentials();

    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "LVGL 锁获取失败,UI 未创建");
        return;
    }
    build_home();
    build_record();
    build_tasks();
    build_provision();
    refresh_provision();
    lv_screen_load(s_screens[BADGE_PAGE_HOME]);
    s_poller = lv_timer_create(poll_cb, 500, NULL);
    bsp_lvgl_unlock();
}
