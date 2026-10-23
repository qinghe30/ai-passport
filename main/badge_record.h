// main/badge_record.h —— 录音采集工作流。
// 独立任务执行 I2S 采集;落点由启动时的网络状态决定:
//   在线  -> badge_upload 流式会话(实时上传);
//   离线  -> badge_store 文件(联网后补传)。
// 录音中网络断开:流式会话作废(服务器收到不完整数据),切换文件缓存续录。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// 初始化录音模块(创建内部互斥锁)。须在其他 badge_record_* 调用之前执行一次。
esp_err_t badge_record_init(void);

// 启动一次录音。streaming=true 且后台可达时实时上传,否则写离线缓存。
// 返回错误表示无法启动(音频初始化失败等)。
esp_err_t badge_record_start(bool streaming);

// 停止录音并收尾。discard=true 丢弃本次全部数据;false 保存/正常上传。
// 阻塞至收尾完成(含上传会话关闭),超时返回错误。
esp_err_t badge_record_stop(bool discard);

// 是否正在录音(含收尾阶段)。
bool badge_record_active(void);

// 当前已录时长(毫秒)。
uint32_t badge_record_duration_ms(void);

// 当前落点是否为实时上传。
bool badge_record_streaming(void);

// 最近一次录音的结果(停止后读取):0 成功保存/送达,其他为错误码。
esp_err_t badge_record_last_result(void);
