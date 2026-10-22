// main/badge_tasks.c —— 任务摘要存储实现。
#include "badge_tasks.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static SemaphoreHandle_t s_lock;
static badge_task_t s_tasks[BADGE_TASK_MAX];
static int s_count;
static long long s_fetched_at = -1;

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void badge_tasks_store(const badge_task_t *tasks, int count)
{
    if (!tasks || count < 0) return;
    ensure_lock();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    int n = count > BADGE_TASK_MAX ? BADGE_TASK_MAX : count;
    for (int i = 0; i < n; i++) s_tasks[i] = tasks[i];
    s_count = n;
    s_fetched_at = (long long)(esp_timer_get_time() / 1000000LL);
    xSemaphoreGive(s_lock);
}

int badge_tasks_snapshot(badge_task_t *out, int max)
{
    if (!out || max <= 0) return 0;
    ensure_lock();
    int n = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        n = s_count < max ? s_count : max;
        for (int i = 0; i < n; i++) out[i] = s_tasks[i];
        xSemaphoreGive(s_lock);
    }
    return n;
}

int badge_tasks_pending(void)
{
    ensure_lock();
    int pending = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < s_count; i++) {
            if (s_tasks[i].pending) pending++;
        }
        xSemaphoreGive(s_lock);
    }
    return pending;
}

long long badge_tasks_fetched_at(void)
{
    ensure_lock();
    long long at;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        at = s_fetched_at;
        xSemaphoreGive(s_lock);
    } else {
        at = -1;
    }
    return at;
}
