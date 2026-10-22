// main/badge_state.c —— 数字工牌状态机实现。规则集中于此,便于单测覆盖。
#include "badge_state.h"

bool badge_model_handle(badge_model_t *m, badge_event_t ev, badge_action_t *out)
{
    badge_action_t action = BADGE_ACT_NONE;
    bool changed = false;

    switch (ev) {
    case BADGE_EV_KEY_OK_CLICK:
        if (m->page == BADGE_PAGE_HOME && m->rec == BADGE_REC_IDLE) {
            // 快捷发起录音:在线则实时上传,离线则写 Flash 缓存。
            m->rec = m->net_up ? BADGE_REC_STREAMING : BADGE_REC_OFFLINE;
            m->page = BADGE_PAGE_RECORD;
            m->discard_pending = false;
            action = BADGE_ACT_START_REC;
            changed = true;
        } else if (m->page == BADGE_PAGE_RECORD &&
                   (m->rec == BADGE_REC_STREAMING || m->rec == BADGE_REC_OFFLINE)) {
            // 停止录音,进入收尾。
            m->rec = BADGE_REC_SAVING;
            action = BADGE_ACT_STOP_REC;
            changed = true;
        } else if (m->page == BADGE_PAGE_TASKS) {
            // 任务页确定键 = 手动刷新(补传 + 拉取任务)。
            action = BADGE_ACT_SYNC_NOW;
            changed = true;
        }
        break;

    case BADGE_EV_KEY_OK_LONG:
        // 录音页长按不响应,避免沟通中误触中断;其余页长按回待机。
        if (m->rec == BADGE_REC_IDLE &&
            (m->page == BADGE_PAGE_TASKS || m->page == BADGE_PAGE_PROVISION ||
             m->page == BADGE_PAGE_RECORD)) {
            m->page = BADGE_PAGE_HOME;
            changed = true;
        }
        break;

    case BADGE_EV_KEY_UP_CLICK:
        if (m->page == BADGE_PAGE_HOME && m->rec == BADGE_REC_IDLE) {
            m->page = BADGE_PAGE_TASKS;
            changed = true;
        } else if (m->page == BADGE_PAGE_RECORD &&
                   (m->rec == BADGE_REC_STREAMING || m->rec == BADGE_REC_OFFLINE)) {
            // 取消录音:丢弃本次数据并收尾。
            m->rec = BADGE_REC_SAVING;
            m->discard_pending = true;
            action = BADGE_ACT_CANCEL_REC;
            changed = true;
        }
        break;

    case BADGE_EV_KEY_DOWN_CLICK:
        if (m->page == BADGE_PAGE_HOME && m->rec == BADGE_REC_IDLE) {
            // 配网页任何时候都可进入,便于更换 Wi-Fi 环境。
            m->page = BADGE_PAGE_PROVISION;
            changed = true;
        }
        break;

    case BADGE_EV_NET_UP:
        if (!m->net_up) {
            m->net_up = true;
            changed = true;
            // 录音途中网络恢复不打断离线写入(避免文件碎片),录完统一补传。
            if (m->rec == BADGE_REC_IDLE) {
                action = BADGE_ACT_SYNC_NOW;
            }
        }
        break;

    case BADGE_EV_NET_DOWN:
        if (m->net_up) {
            m->net_up = false;
            changed = true;
            if (m->rec == BADGE_REC_STREAMING) {
                // 实时上传中断:改写离线缓存,尽力保住已录内容。
                m->rec = BADGE_REC_OFFLINE;
            }
        }
        break;

    case BADGE_EV_REC_SAVED:
        if (m->rec == BADGE_REC_SAVING) {
            m->rec = BADGE_REC_IDLE;
            m->page = BADGE_PAGE_HOME;
            changed = true;
            // 丢弃的录音不触发同步;正常保存且在线时才补传/刷新。
            if (m->net_up && !m->discard_pending) {
                action = BADGE_ACT_SYNC_NOW;
            }
            m->discard_pending = false;
        }
        break;

    default:
        break;
    }

    if (out) *out = action;
    return changed;
}
