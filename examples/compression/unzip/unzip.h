#ifndef UNZIP_H
#define UNZIP_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <lzma.h>

/* 固件头部魔数: "FRWF" = 0x46575246 */
#define FW_MAGIC 0x46575246

/* 流式解压缓冲区大小 (4KB) - 低内存占用 */
#define INPUT_BUFFER_SIZE  (4 * 1024)
#define OUTPUT_BUFFER_SIZE (4 * 1024)

/* 固件头部结构 */
/* 与 lzma/flow-unzip/unzip_stream.h 共存：该头已定义 FirmwareHeader_t 时跳过 */
#ifndef __UNZIP_STREAM_H__
typedef struct {
    uint32_t magic;              /* 魔数 0x46575246 ("FRWF") */
    uint32_t version;            /* 固件版本号 */
    uint32_t compressed_size;    /* 压缩数据大小 */
    uint32_t uncompressed_size;  /* 未压缩数据大小 */
    uint32_t crc32;              /* CRC32 校验 */
    uint8_t  reserved[12];       /* 保留字段 */
} FirmwareHeader_t;
#endif /* __UNZIP_STREAM_H__ */

/* 错误码 */
enum {
    UNZIP_SUCCESS            =  0,
    UNZIP_ERROR_OPEN_INPUT   = -1,
    UNZIP_ERROR_READ_HEADER  = -2,
    UNZIP_ERROR_MAGIC        = -3,
    UNZIP_ERROR_OPEN_OUTPUT  = -4,
    UNZIP_ERROR_INIT_DECODER = -5,
    UNZIP_ERROR_DECOMPRESS   = -6,
    UNZIP_ERROR_WRITE_OUTPUT = -7,
    UNZIP_ERROR_CRC          = -8,
};

int uncompressed_File(const char *input_path, const char *output_path);

#endif