// tests/test_badge_fmt.c —— 格式化工具 host 测试。
#include "badge_fmt.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_duration_basic(void)
{
    char buf[8];
    badge_fmt_duration(0, buf, sizeof(buf));
    assert(strcmp(buf, "00:00") == 0);
    badge_fmt_duration(59000, buf, sizeof(buf));
    assert(strcmp(buf, "00:59") == 0);
    badge_fmt_duration(61000, buf, sizeof(buf));
    assert(strcmp(buf, "01:01") == 0);
    badge_fmt_duration(100UL * 60UL * 1000UL, buf, sizeof(buf));
    assert(strcmp(buf, "100:00") != 0);          // 超界封顶,不是 100 分钟
    badge_fmt_duration(600UL * 60UL * 1000UL, buf, sizeof(buf));
    assert(strcmp(buf, "99:59") == 0);           // 封顶 99:59
    printf("ok: duration formatting\n");
}

static void test_clip_utf8_respects_boundaries(void)
{
    // "会议沟通" 每个汉字 3 字节;"A" 1 字节。
    char out[16];
    // 目标 9 字节:可容纳 "会议" (6 字节) + NUL,"通" 放不下(6+3+1>9)。
    badge_fmt_clip_utf8("会议沟通", out, 9);
    assert(strcmp(out, "会议") == 0);
    // 恰好放下:6+3+1 == 10。
    badge_fmt_clip_utf8("会议通", out, 10);
    assert(strcmp(out, "会议通") == 0);
    // ASCII 原样。
    badge_fmt_clip_utf8("call ABC 123", out, sizeof(out));
    assert(strcmp(out, "call ABC 123") == 0);
    // 空目标尺寸安全。
    badge_fmt_clip_utf8("会议", out, 1);
    assert(out[0] == '\0');
    // 截断后仍是合法 UTF-8:从首字节逐字符解码,每个序列都完整落在串内。
    badge_fmt_clip_utf8("跟进客户张三丰", out, 8);
    size_t len = strlen(out);
    size_t i = 0;
    while (i < len) {
        unsigned char lead = (unsigned char)out[i];
        size_t seq = 1;
        if ((lead & 0xE0) == 0xC0) seq = 2;
        else if ((lead & 0xF0) == 0xE0) seq = 3;
        else if ((lead & 0xF8) == 0xF0) seq = 4;
        assert(i + seq <= len);                  // 不含残缺的半截序列
        i += seq;
    }
    printf("ok: utf8 clipping\n");
}

int main(void)
{
    test_duration_basic();
    test_clip_utf8_respects_boundaries();
    printf("test_badge_fmt: ALL PASS\n");
    return 0;
}
