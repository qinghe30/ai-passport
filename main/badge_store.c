// main/badge_store.c —— FATFS 离线缓存实现。
// 挂载点 BADGE_STORE_MOUNT,录音文件固定放在其下 rec/ 子目录,文件名为
// 8.3 短名 R%06u.PCM(不启用 LFN,省 RAM/Flash);序号取目录内最大序号 + 1。
#include "badge_store.h"

#include "badge_config.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "badge_store";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_ready;
static uint32_t s_next_seq = 1;
// 读取句柄单槽:同一时刻只有上传任务在读一个补传文件。
static FILE *s_read_fp;

// 8.3 文件名:R + 6 位序号 + .PCM,无需长文件名支持。
static void seq_to_name(uint32_t seq, char *buf, size_t size)
{
    snprintf(buf, size, "R%06lu.PCM", (unsigned long)seq);
}

static void full_path(const char *name, char *buf, size_t size)
{
    snprintf(buf, size, BADGE_STORE_DIR "/%s", name);
}

esp_err_t badge_store_init(void)
{
    if (s_ready) return ESP_OK;

    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 2,
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        BADGE_STORE_MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage 分区挂载失败: %s", esp_err_to_name(err));
        return err;
    }

    // rec 目录不存在则创建(FATFS mkdir 对已存在目录返回错误,忽略即可)。
    mkdir(BADGE_STORE_DIR, 0775);

    // 扫描现有文件,把序号推进到最大值 + 1,避免补传文件被覆盖。
    DIR *dir = opendir(BADGE_STORE_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            unsigned seq = 0;
            if (sscanf(ent->d_name, "R%u.PCM", &seq) == 1 && seq >= s_next_seq) {
                s_next_seq = seq + 1;
            }
        }
        closedir(dir);
    }
    s_ready = true;
    ESP_LOGI(TAG, "storage 已挂载,下一个文件序号 %lu", (unsigned long)s_next_seq);
    return ESP_OK;
}

bool badge_store_ready(void)
{
    return s_ready;
}

// struct badge_store_writer 定义在 badge_store.h 中(调用方需按值持有句柄)。

esp_err_t badge_store_open(char *name, size_t name_size, badge_store_writer_t *w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    w->fp = NULL;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (name && name_size) name[0] = '\0';
    char fname[24];
    seq_to_name(s_next_seq, fname, sizeof(fname));
    char path[64];
    full_path(fname, path, sizeof(path));
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "打开缓存文件失败: %s", path);
        return ESP_FAIL;
    }
    s_next_seq++;
    w->fp = fp;
    if (name && name_size) snprintf(name, name_size, "%s", fname);
    return ESP_OK;
}

esp_err_t badge_store_write(badge_store_writer_t *w, const void *data, size_t bytes)
{
    if (!w || !w->fp) return ESP_ERR_INVALID_STATE;
    size_t n = fwrite(data, 1, bytes, w->fp);
    if (n != bytes) {
        ESP_LOGE(TAG, "缓存写入不完整: %u/%u", (unsigned)n, (unsigned)bytes);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t badge_store_close(badge_store_writer_t *w)
{
    if (!w || !w->fp) return ESP_ERR_INVALID_STATE;
    // fclose 内部 flush;失败意味着最后的录音数据未落盘,如实报错。
    int rc = fclose(w->fp);
    w->fp = NULL;
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

int badge_store_count(void)
{
    if (!s_ready) return 0;
    int count = 0;
    DIR *dir = opendir(BADGE_STORE_DIR);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        unsigned seq = 0;
        if (sscanf(ent->d_name, "R%u.PCM", &seq) == 1) count++;
    }
    closedir(dir);
    return count;
}

esp_err_t badge_store_entry(int index, char *name, size_t name_size, size_t *bytes)
{
    if (!s_ready || index < 0) return ESP_ERR_INVALID_ARG;
    DIR *dir = opendir(BADGE_STORE_DIR);
    if (!dir) return ESP_ERR_INVALID_STATE;
    int seen = 0;
    struct dirent *ent;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    while ((ent = readdir(dir)) != NULL) {
        unsigned seq = 0;
        if (sscanf(ent->d_name, "R%u.PCM", &seq) != 1) continue;
        if (seen++ < index) continue;
        if (name && name_size) snprintf(name, name_size, "%s", ent->d_name);
        if (bytes) {
            char path[64];
            full_path(ent->d_name, path, sizeof(path));
            *bytes = 0;
            FILE *fp = fopen(path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                fclose(fp);
                if (sz > 0) *bytes = (size_t)sz;
            }
        }
        result = ESP_OK;
        break;
    }
    closedir(dir);
    return result;
}

esp_err_t badge_store_read_open(const char *name)
{
    if (!s_ready || !name) return ESP_ERR_INVALID_ARG;
    char path[64];
    full_path(name, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;
    s_read_fp = fp;
    return ESP_OK;
}

esp_err_t badge_store_read(void *data, size_t bytes, size_t *out_bytes)
{
    if (!s_read_fp) return ESP_ERR_INVALID_STATE;
    size_t n = fread(data, 1, bytes, s_read_fp);
    if (out_bytes) *out_bytes = n;
    if (n == 0) return ESP_FAIL;
    return ESP_OK;
}

void badge_store_read_close(void)
{
    if (s_read_fp) {
        fclose(s_read_fp);
        s_read_fp = NULL;
    }
}

esp_err_t badge_store_delete(const char *name)
{
    if (!s_ready || !name) return ESP_ERR_INVALID_ARG;
    char path[64];
    full_path(name, path, sizeof(path));
    if (remove(path) != 0) return ESP_FAIL;
    return ESP_OK;
}
