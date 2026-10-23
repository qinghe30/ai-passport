// main/badge_fmt.c —— 纯文本格式化工具实现。
#include "badge_fmt.h"

#include <stdio.h>

void badge_fmt_duration(uint32_t ms, char *buf, size_t size)
{
    if (!buf || size < 6) return;
    uint32_t total_sec = ms / 1000;
    uint32_t minutes = total_sec / 60;
    uint32_t seconds = total_sec % 60;
    if (minutes > 99) {
        // 长会议录音需要显示真实时长:超过 99 分钟改用 H:MM:SS,不再封顶。
        snprintf(buf, size, "%u:%02u:%02u", (unsigned)(minutes / 60),
                 (unsigned)(minutes % 60), (unsigned)seconds);
        return;
    }
    buf[0] = (char)('0' + minutes / 10);
    buf[1] = (char)('0' + minutes % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + seconds / 10);
    buf[4] = (char)('0' + seconds % 10);
    buf[5] = '\0';
}

void badge_fmt_clip_utf8(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t used = 0;
    size_t i = 0;
    while (src[i] != '\0') {
        // UTF-8 首字节判定本字符长度(ASCII 1 字节;0xC0 起为 2~4 字节序列)。
        unsigned char c = (unsigned char)src[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        // 越界或放不下整个字符则停止;余量需含结尾 NUL。
        if (len == 1 && c > 0x7F) break;   // 非法续字节,视为坏数据直接截断
        if (used + len + 1 > dst_size) break;
        for (size_t k = 0; k < len; k++) {
            if (src[i + k] == '\0') return; // 序列被源串截断,保守结束
            dst[used + k] = src[i + k];
        }
        used += len;
        i += len;
    }
    dst[used] = '\0';
}
