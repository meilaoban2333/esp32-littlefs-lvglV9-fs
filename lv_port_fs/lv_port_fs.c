/**
 * @file lv_port_fs.c
 * @brief LVGL v9 文件系统接口 (ESP32 + LittleFS)
 * 
 * 本文件实现了 LVGL 文件系统接口的移植层，使用 ESP32 的 LittleFS 作为底层存储。
 * 支持文件和目录的打开、读写、寻址、关闭等操作。
 */

#include "lv_port_fs.h"
#include "lvgl.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_timer.h"  
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_littlefs.h"

#define TAG "lv_fs"


typedef FILE * file_t;  // 文件句柄类型
typedef DIR * dir_t;    // 目录句柄类型

/* forward declaration */
static void fs_init(void);  // 文件系统初始化函数

/* LVGL FS 回调函数声明 */
static void * fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode);
static lv_fs_res_t fs_close(lv_fs_drv_t * drv, void * file_p);
static lv_fs_res_t fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br);
static lv_fs_res_t fs_write(lv_fs_drv_t * drv, void * file_p, const void * buf, uint32_t btw, uint32_t * bw);
static lv_fs_res_t fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence);
static lv_fs_res_t fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p);

static void * fs_dir_open(lv_fs_drv_t * drv, const char * path);
static lv_fs_res_t fs_dir_read(lv_fs_drv_t * drv, void * rddir_p, char * fn, uint32_t fn_len);
static lv_fs_res_t fs_dir_close(lv_fs_drv_t * drv, void * rddir_p);

/**
 * @brief 初始化 LVGL 文件系统并注册驱动
 * 
 * @inputs 无
 * @outputs 无
 */
void lv_port_fs_init(void)
{
    fs_init();  // 初始化 LittleFS

    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);  // 初始化 LVGL 文件系统驱动结构体

    fs_drv.letter = 'S';      // 设置 LVGL 中访问的盘符，如 S:/path/to/file
    fs_drv.open_cb = fs_open;
    fs_drv.close_cb = fs_close;
    fs_drv.read_cb = fs_read;
    fs_drv.write_cb = fs_write;
    fs_drv.seek_cb = fs_seek;
    fs_drv.tell_cb = fs_tell;

    fs_drv.dir_open_cb = fs_dir_open;
    fs_drv.dir_read_cb = fs_dir_read;
    fs_drv.dir_close_cb = fs_dir_close;

    lv_fs_drv_register(&fs_drv);  // 注册驱动到 LVGL
}

/**
 * @brief LittleFS 初始化
 * 
 * @inputs 无
 * @outputs 无
 */
static void fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LV_FS_PATH,            // 挂载点
        .partition_label = NULL,            // 使用默认分区
        .format_if_mount_failed = true,     // 挂载失败时格式化
        .dont_mount = false                 // 自动挂载
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS (%s)", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS Partition size: total: %d, used: %d", total, used);
    }
}

/* 文件操作实现 */

/**
 * @brief 打开文件
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - path: 文件路径，支持 LVGL 风格盘符（如 S:/file.txt）
 *  - mode: 打开模式 (LV_FS_MODE_RD 或 LV_FS_MODE_WR)
 * @outputs
 *  - 返回 FILE* 文件句柄，失败返回 NULL
 */
static void * fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode)
{
    LV_UNUSED(drv);

    const char *p = path;
    if (p[1] == ':') p += 2;   // 去掉 LVGL 的盘符，如 "S:"

    char filepath[256];
    if (*p == '/') {
        snprintf(filepath, sizeof(filepath), LV_FS_PATH "%s", p);
    } else {
        snprintf(filepath, sizeof(filepath), LV_FS_PATH "/%s", p);
    }

    const char *mode_str = (mode & LV_FS_MODE_WR) ? "wb" : "rb";  // 写模式或读模式
    ESP_LOGI(TAG, "open %s (%s)", filepath, mode_str);

    FILE *f = fopen(filepath, mode_str);
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d)", filepath, errno);
    }
    return f;
}

/**
 * @brief 关闭文件
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - file_p: 文件句柄
 * @outputs
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_close(lv_fs_drv_t * drv, void * file_p)
{
    LV_UNUSED(drv);
    if (!file_p) return LV_FS_RES_UNKNOWN;
    fclose((FILE *)file_p);
    return LV_FS_RES_OK;
}

/**
 * @brief 读取文件
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - file_p: 文件句柄
 *  - buf: 数据缓冲区
 *  - btr: 期望读取字节数
 * @outputs
 *  - br: 实际读取字节数
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_read(lv_fs_drv_t * drv, void * file_p, void * buf,
                           uint32_t btr, uint32_t * br)
{
    LV_UNUSED(drv);
    if (!file_p) return LV_FS_RES_UNKNOWN;

    size_t n = fread(buf, 1, btr, (FILE *)file_p);
    *br = (uint32_t)n;

    if (n == 0) {
        if (feof((FILE *)file_p)) return LV_FS_RES_OK;       // 文件结尾
        if (ferror((FILE *)file_p)) return LV_FS_RES_UNKNOWN; // 读取错误
    }
    return LV_FS_RES_OK;
}

/**
 * @brief 写入文件
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - file_p: 文件句柄
 *  - buf: 数据缓冲区
 *  - btw: 期望写入字节数
 * @outputs
 *  - bw: 实际写入字节数
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_write(lv_fs_drv_t * drv, void * file_p, const void * buf,
                            uint32_t btw, uint32_t * bw)
{
    LV_UNUSED(drv);
    if (!file_p) return LV_FS_RES_UNKNOWN;

    size_t n = fwrite(buf, 1, btw, (FILE *)file_p);
    *bw = (uint32_t)n;

    if (n < btw && ferror((FILE *)file_p)) return LV_FS_RES_UNKNOWN;
    return LV_FS_RES_OK;
}

/**
 * @brief 文件定位
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - file_p: 文件句柄
 *  - pos: 相对位置或绝对位置
 *  - whence: 定位方式 (LV_FS_SEEK_SET/CUR/END)
 * @outputs
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_seek(lv_fs_drv_t * drv, void * file_p,
                           uint32_t pos, lv_fs_whence_t whence)
{
    LV_UNUSED(drv);
    if (!file_p) return LV_FS_RES_UNKNOWN;

    int origin = SEEK_SET;
    switch (whence) {
        case LV_FS_SEEK_SET: origin = SEEK_SET; break;
        case LV_FS_SEEK_CUR: origin = SEEK_CUR; break;
        case LV_FS_SEEK_END: origin = SEEK_END; break;
    }
    return fseek((FILE *)file_p, pos, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

/**
 * @brief 获取文件当前位置
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - file_p: 文件句柄
 * @outputs
 *  - pos_p: 当前文件位置
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p)
{
    LV_UNUSED(drv);
    if (!file_p || !pos_p) return LV_FS_RES_UNKNOWN;

    long pos = ftell((FILE *)file_p);
    if (pos < 0) return LV_FS_RES_UNKNOWN;
    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

/* 目录操作实现 */

/**
 * @brief 打开目录
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - path: 目录路径
 * @outputs
 *  - 返回 DIR* 目录句柄，失败返回 NULL
 */
static void * fs_dir_open(lv_fs_drv_t * drv, const char * path)
{
    LV_UNUSED(drv);

    char dirpath[256];
    if (path[0] == '/') snprintf(dirpath, sizeof(dirpath), LV_FS_PATH "%s", path);
    else snprintf(dirpath, sizeof(dirpath), LV_FS_PATH "/%s", path);

    return opendir(dirpath);
}

/**
 * @brief 读取目录
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - rddir_p: 目录句柄
 *  - fn_len: 文件名缓冲区长度
 * @outputs
 *  - fn: 读取到的文件名，目录读完返回空字符串
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_dir_read(lv_fs_drv_t * drv, void * rddir_p,
                               char * fn, uint32_t fn_len)
{
    LV_UNUSED(drv);
    if (!rddir_p) return LV_FS_RES_UNKNOWN;

    struct dirent *entry = readdir((DIR *)rddir_p);
    if (!entry) {
        fn[0] = '\0';  // LVGL 要求读完返回空字符串
        return LV_FS_RES_OK;
    }
    strncpy(fn, entry->d_name, fn_len - 1);
    fn[fn_len - 1] = '\0';
    return LV_FS_RES_OK;
}

/**
 * @brief 关闭目录
 * 
 * @inputs
 *  - drv: LVGL 文件系统驱动指针
 *  - rddir_p: 目录句柄
 * @outputs
 *  - 返回 LV_FS_RES_OK 或 LV_FS_RES_UNKNOWN
 */
static lv_fs_res_t fs_dir_close(lv_fs_drv_t * drv, void * rddir_p)
{
    LV_UNUSED(drv);
    if (!rddir_p) return LV_FS_RES_UNKNOWN;
    closedir((DIR *)rddir_p);
    return LV_FS_RES_OK;
}
