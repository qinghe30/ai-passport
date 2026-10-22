// main/badge_store.h —— 离线录音缓存:storage 分区(FATFS)上的顺序 PCM 文件。
// 用途:Wi-Fi 离线时先把录音写入 Flash,联网后由上传任务逐个补传并删除。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// 顺序写入句柄(调用方按值持有,禁止跨任务并发使用;
// fp 由 badge_store_open/close 内部维护,调用方不要直接访问)。
typedef struct badge_store_writer {
    FILE *fp;
} badge_store_writer_t;

// 挂载 storage 分区。失败(含首次需要格式化)时自动尝试格式化后重挂一次;
// 仍失败则 badge_store_ready() 为 false,录音仅允许在线流式上传。
esp_err_t badge_store_init(void);

// 缓存是否可用。
bool badge_store_ready(void);

// 顺序写入器。open 生成新文件名(返回写入 buf,name 至少 16 字节);
// write/close 语义同 stdio。open 失败返回错误码,writer 置为无效。
esp_err_t badge_store_open(char *name, size_t name_size, badge_store_writer_t *w);
esp_err_t badge_store_write(badge_store_writer_t *w, const void *data, size_t bytes);
esp_err_t badge_store_close(badge_store_writer_t *w);

// 待补传文件数。
int badge_store_count(void);

// 按序号读取第 i 个待补传文件(0..count-1):名字写入 name,字节大小回填。
// 迭代期间目录可能变化(上传成功即删除),因此每次迭代重新从 0 取。
esp_err_t badge_store_entry(int index, char *name, size_t name_size, size_t *bytes);

// 打开待补传文件用于读(UPLOAD 任务用毕须 badge_store_read_close)。
esp_err_t badge_store_read_open(const char *name);
esp_err_t badge_store_read(void *data, size_t bytes, size_t *out_bytes);
void badge_store_read_close(void);

// 删除已补传成功的文件。
esp_err_t badge_store_delete(const char *name);
