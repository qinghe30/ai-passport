// tests/test_badge_state.c —— 数字工牌状态机 host 测试。
#include "badge_state.h"

#include <assert.h>
#include <stdio.h>

static badge_model_t base_model(void)
{
    badge_model_t m = {
        .page = BADGE_PAGE_HOME,
        .rec = BADGE_REC_IDLE,
        .net_up = false,
        .has_creds = true,
    };
    return m;
}

static void test_home_ok_starts_streaming_when_online(void)
{
    badge_model_t m = base_model();
    m.net_up = true;
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_KEY_OK_CLICK, &act));
    assert(m.page == BADGE_PAGE_RECORD);
    assert(m.rec == BADGE_REC_STREAMING);
    assert(act == BADGE_ACT_START_REC);
    printf("ok: home+OK online -> streaming record\n");
}

static void test_home_ok_starts_offline_when_no_network(void)
{
    badge_model_t m = base_model();
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_KEY_OK_CLICK, &act));
    assert(m.rec == BADGE_REC_OFFLINE);
    assert(m.page == BADGE_PAGE_RECORD);
    assert(act == BADGE_ACT_START_REC);
    printf("ok: home+OK offline -> offline record\n");
}

static void test_recording_ok_stops_and_save_returns_home(void)
{
    badge_model_t m = base_model();
    m.net_up = true;
    m.page = BADGE_PAGE_RECORD;
    m.rec = BADGE_REC_STREAMING;
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_KEY_OK_CLICK, &act));
    assert(m.rec == BADGE_REC_SAVING);
    assert(act == BADGE_ACT_STOP_REC);
    assert(badge_model_handle(&m, BADGE_EV_REC_SAVED, &act));
    assert(m.rec == BADGE_REC_IDLE);
    assert(m.page == BADGE_PAGE_HOME);
    assert(act == BADGE_ACT_SYNC_NOW);   // 在线收尾后触发补传/刷新
    printf("ok: record OK stop -> saving -> home + sync\n");
}

static void test_net_loss_switches_streaming_to_offline(void)
{
    badge_model_t m = base_model();
    m.net_up = true;
    m.page = BADGE_PAGE_RECORD;
    m.rec = BADGE_REC_STREAMING;
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_NET_DOWN, &act));
    assert(m.rec == BADGE_REC_OFFLINE);
    assert(!m.net_up);
    assert(act == BADGE_ACT_NONE);       // 不打断录音,仅切换落点
    printf("ok: net down keeps recording into cache\n");
}

static void test_cancel_discards_and_does_not_sync(void)
{
    badge_model_t m = base_model();
    m.net_up = true;
    m.page = BADGE_PAGE_RECORD;
    m.rec = BADGE_REC_STREAMING;
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_KEY_UP_CLICK, &act));
    assert(m.rec == BADGE_REC_SAVING);
    assert(act == BADGE_ACT_CANCEL_REC);
    assert(badge_model_handle(&m, BADGE_EV_REC_SAVED, &act));
    assert(m.rec == BADGE_REC_IDLE);
    assert(m.page == BADGE_PAGE_HOME);
    assert(act == BADGE_ACT_NONE);       // 丢弃后无需同步
    printf("ok: cancel discards recording\n");
}

static void test_navigation_rules(void)
{
    badge_model_t m = base_model();
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_KEY_UP_CLICK, &act));
    assert(m.page == BADGE_PAGE_TASKS);
    assert(badge_model_handle(&m, BADGE_EV_KEY_OK_CLICK, &act));
    assert(act == BADGE_ACT_SYNC_NOW);   // 任务页 OK = 手动刷新
    assert(badge_model_handle(&m, BADGE_EV_KEY_OK_LONG, &act));
    assert(m.page == BADGE_PAGE_HOME);
    assert(badge_model_handle(&m, BADGE_EV_KEY_DOWN_CLICK, &act));
    assert(m.page == BADGE_PAGE_PROVISION);
    assert(badge_model_handle(&m, BADGE_EV_KEY_OK_LONG, &act));
    assert(m.page == BADGE_PAGE_HOME);
    printf("ok: navigation transitions\n");
}

static void test_recording_ignores_navigation_and_long_press(void)
{
    badge_model_t m = base_model();
    m.net_up = true;
    m.page = BADGE_PAGE_RECORD;
    m.rec = BADGE_REC_STREAMING;
    badge_action_t act = BADGE_ACT_NONE;
    assert(!badge_model_handle(&m, BADGE_EV_KEY_OK_LONG, &act));
    assert(m.page == BADGE_PAGE_RECORD);          // 长按不误退
    assert(badge_model_handle(&m, BADGE_EV_KEY_UP_CLICK, &act));
    assert(act == BADGE_ACT_CANCEL_REC);          // UP 在录音页 = 取消
    printf("ok: recording page guards navigation\n");
}

static void test_net_up_triggers_sync_when_idle_only(void)
{
    badge_model_t m = base_model();
    badge_action_t act = BADGE_ACT_NONE;
    assert(badge_model_handle(&m, BADGE_EV_NET_UP, &act));
    assert(m.net_up);
    assert(act == BADGE_ACT_SYNC_NOW);   // 空闲联网 -> 立即补传缓存
    m.page = BADGE_PAGE_TASKS;
    badge_model_t snapshot = m;
    assert(badge_model_handle(&m, BADGE_EV_NET_UP, &act) == false);
    assert(snapshot.page == m.page);     // 重复联网事件无动作
    printf("ok: net up sync trigger\n");
}

int main(void)
{
    test_home_ok_starts_streaming_when_online();
    test_home_ok_starts_offline_when_no_network();
    test_recording_ok_stops_and_save_returns_home();
    test_net_loss_switches_streaming_to_offline();
    test_cancel_discards_and_does_not_sync();
    test_navigation_rules();
    test_recording_ignores_navigation_and_long_press();
    test_net_up_triggers_sync_when_idle_only();
    printf("test_badge_state: ALL PASS\n");
    return 0;
}
