// main/badge_config.h —— 数字工牌应用编译期配置。
// 部署时按现场环境修改本文件后重新编译固件。
#pragma once

// 后台地址的出厂默认值(接收录音上传与提供任务摘要)。
// 实际值在运行时读取 NVS:配网页(热点 FoloBadge-XXXX 的 192.168.4.1)里填写后
// 覆盖本默认值,见 badge_settings.c。这里只在从未配置过时生效。
#define BADGE_SERVER_URL "http://192.168.1.100:8000"

// 配置热点名称前缀。完整热点名为 "前缀-XXXX"(XXXX 取 MAC 后两字节)。
#define BADGE_AP_PREFIX "FoloBadge"

// 设备蓝牙配网广播名(保留的 BluFi 配网实现使用;当前配网页走 SoftAP)。
#define BADGE_BLE_NAME "BLUFI_FoloBadge"

// 录音格式:16 kHz / 16 bit / 单声道(32 KB/s,与演示后台约定一致)。
#define BADGE_SAMPLE_RATE_HZ 16000
#define BADGE_SAMPLE_BITS    16
#define BADGE_SAMPLE_CHANNELS 1

// 录音采集块大小(采样数)。1 KB/块,采集任务与上传任务以此解耦。
#define BADGE_CHUNK_SAMPLES 512

// 实时上传缓冲(StreamBuffer 字节数)。约 250 毫秒 PCM,吸收网络抖动。
// 不能取太大:C3 无 PSRAM,录音期间音频(约 18 KB)已占掉大半堆,
// 缓冲留 32 KB 时流式期间只剩约 10 KB,lwIP/Wi-Fi 发不出报文,
// 表现为录音约 1 秒后 send 卡死 10 秒。
// 持续超时则说明网络带宽不足,录音任务会切换到离线缓存续录。
#define BADGE_STREAM_BUFFER_BYTES (8 * 1024)

// 离线缓存目录(FATFS)。分区表中的 storage 分区挂载点。
#define BADGE_STORE_MOUNT "/storage"
#define BADGE_STORE_DIR   "/storage/rec"

// 单条任务标题/摘要的最大显示字节(UTF-8,按字符边界截断)。
#define BADGE_TASK_TITLE_MAX_BYTES 96

// 待机页背光:亮屏亮度与空闲调暗亮度(百分比)。录音页保持亮屏。
#define BADGE_BACKLIGHT_ON  80
#define BADGE_BACKLIGHT_DIM 15
