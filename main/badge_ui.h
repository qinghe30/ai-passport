// main/badge_ui.h —— 数字工牌中文 UI(四页面:待机/录音/任务/配网)。
// 页面由 LVGL 绘制;按键事件 -> badge_state 状态机 -> 动作副作用。
#pragma once

#include "bsp_button.h"

// 创建四个页面并启动周期刷新。须在 bsp_lvgl_init() 成功后、任何 badge_ui_key
// 之前调用一次;内部持 LVGL 锁,可在任意任务上下文调用。
void badge_ui_init(void);

// 按键事件入口(main 的输入分发任务调用)。单击/长按映射为状态机事件;
// 阻塞型副作用(停止录音收尾、配网启停)在此执行,UI 更新经 bsp_lvgl_lock 保护。
void badge_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
