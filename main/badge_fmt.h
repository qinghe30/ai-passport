// main/badge_fmt.h —— 纯文本格式化工具(可 host 测试,无 ESP-IDF 依赖)。
#pragma once

#include <stddef.h>
#include <stdint.h>

// 毫秒转时长文本:100 分钟以内为 "MM:SS",超过后为 "H:MM:SS"(不封顶)。
// buf 至少 6 字节;要完整显示带小时的格式需要 10 字节以上。
void badge_fmt_duration(uint32_t ms, char *buf, size_t size);

// 按 UTF-8 字符边界把 src 截断到 max_bytes(含结尾 NUL)以内写入 dst。
// 不会在多字节序列中间截断;放不下的整字符直接丢弃。始终 NUL 结尾。
void badge_fmt_clip_utf8(const char *src, char *dst, size_t dst_size);
