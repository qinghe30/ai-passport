// main/badge_tasks.h —— 后台同步下来的任务摘要存储。
// 上传/同步任务写入,UI 任务读取;互斥锁保护,快照式访问避免持锁渲染。
#pragma once

#include "badge_config.h"
#include <stdbool.h>

#define BADGE_TASK_MAX 8
#define BADGE_TASK_ASSIGNEE_MAX 32
#define BADGE_TASK_DUE_MAX 16

typedef struct {
    int id;                                  // 后台任务 ID
    char title[BADGE_TASK_TITLE_MAX_BYTES];  // 标题(UTF-8,已按边界截断)
    char assignee[BADGE_TASK_ASSIGNEE_MAX];  // 负责人
    char due[BADGE_TASK_DUE_MAX];            // 截止日期 YYYY-MM-DD,可为空
    bool pending;                            // true=待办 false=已完成
} badge_task_t;

// 用后台最新数据整体替换本地快照(深拷贝)。count>BADGE_TASK_MAX 时截断。
void badge_tasks_store(const badge_task_t *tasks, int count);

// 拷出当前快照,返回实际条数(最多 max 条)。
int badge_tasks_snapshot(badge_task_t *out, int max);

// 待办条数(pending=true 的数量)。
int badge_tasks_pending(void);

// 上次成功拉取任务的时间(秒,esp_timer 计数);从未成功返回 -1。
long long badge_tasks_fetched_at(void);
