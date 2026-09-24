#ifndef __UNZIP_STREAM_H__
#define __UNZIP_STREAM_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "LzmaDec.h"
#include "7zCrc.h"
#include "7zAlloc.h"
#include "Alloc.h"

/* ================================================================
 * 固件头部定义（与 firmware_create.c / firmware_update.c 一致）
 * ================================================================ */
typedef struct {
    uint32_t magic;              /* 魔数：0x46575246 ("FRWF") */
    uint32_t version;            /* 固件版本 */
    uint32_t compressed_size;    /* 压缩数据大小（含 LZMA 属性头） */
    uint32_t uncompressed_size;  /* 未压缩数据大小 */
    uint32_t crc32;              /* 未压缩数据的 CRC32 */
    uint8_t  reserved[12];       /* 保留字段 */
} FirmwareHeader_t;

#define FIRMWARE_MAGIC 0x46575246

/* 流式解压缓冲区（可按嵌入式内存调整） */
#define INPUT_BUFFER_SIZE  (4 * 1024)
#define OUTPUT_BUFFER_SIZE (4 * 1024)

/* ================================================================
 * 模拟 Flash 配置
 * ================================================================ */
#define FLASH_SIZE          (4 * 1024 * 1024)  /* 4MB 模拟 Flash */
#define FLASH_ERASE_VALUE   0xFF               /* Flash 擦除后的默认值 */

/* 全局内存分配器（LZMA SDK 使用） */
static const ISzAlloc g_FirmwareAlloc = { SzAlloc, SzFree };

/* 模拟 Flash 存储 */
static uint8_t  *g_flash_base   = NULL;
static size_t    g_flash_offset = 0;   /* 当前写入偏移 */
static uint32_t  g_firmware_version = 0;

/* 进度显示 */
static int g_last_percent = -1;

#endif