// main/badge_blufi_security.h —— BLUFI 官方安全协商(DHM+AES+MD5/CRC)。
// 提取自 demo/blufi-provisioning 参考实现,函数前缀改为 badge_,算法保持一致。
#pragma once

#include <stdbool.h>
#include <stdint.h>

void badge_blufi_negotiate(uint8_t *data, int len, uint8_t **output_data,
                           int *output_len, bool *need_free);
int badge_blufi_encrypt(uint8_t iv8, uint8_t *data, int len);
int badge_blufi_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t badge_blufi_checksum(uint8_t iv8, uint8_t *data, int len);
int badge_blufi_security_init(void);
void badge_blufi_security_deinit(void);
