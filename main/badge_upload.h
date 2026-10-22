// main/badge_upload.h —— HTTP 上传与同步服务。
// 三类职责,全部在 worker 侧执行,按键/UI 只发请求:
//   1. 流式会话:录音期间把 PCM 以 chunked POST 实时推送后台;
//   2. 补传:把离线缓存的录音文件逐个上传,成功后删除;
//   3. 任务拉取:GET 任务摘要写入 badge_tasks,供 UI 展示。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// 启动常驻同步 worker 任务(幂等)。需在 badge_wifi_init() 之后调用。
esp_err_t badge_upload_init(void);

// 请求执行一次同步(补传缓存 + 刷新任务)。忙时该请求被合并,不排队。
esp_err_t badge_upload_request_sync(void);

// 设备 ID(efuse MAC 十六进制大写,12 字符)。buf 至少 13 字节。
void badge_upload_device_id(char *buf, unsigned size);

// --- 流式会话(录音任务调用;同一时刻仅允许一个会话) ---

// 开始新的流式录音会话:建立 HTTP 连接并启动上传子任务。
esp_err_t badge_upload_stream_begin(void);

// 写入一段 PCM。内部经 StreamBuffer 解耦;缓冲持续写满(网络过慢)时返回
// 超时错误,调用方应结束会话并切换到离线缓存。
esp_err_t badge_upload_stream_write(const void *pcm, size_t bytes);

// 结束会话。discard=true 表示取消本次录音:子任务直接丢弃未发数据并断开;
// false 表示正常收尾:发完剩余数据后发送结束块,等待后台响应。
esp_err_t badge_upload_stream_end(bool discard);

// 最近一次流式会话是否完整送达(仅 end 成功后有意义)。
bool badge_upload_stream_delivered(void);
