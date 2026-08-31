/*
 * firmware_create.c - 创建固件更新包工具
 *
 * 功能：将原始固件二进制文件打包为 LZMA 压缩的固件更新包
 * 格式：[FirmwareHeader][LZMA压缩数据]
 *
 * 编译：gcc -o firmware_create firmware_create.c \
 *           ../../C/LzmaEnc.c ../../C/LzFind.c ../../C/LzFindMt.c \
 *           ../../C/LzmaDec.c ../../C/Alloc.c ../../C/7zFile.c ../../C/7zCrc.c \
 *           ../../C/Threads.c -I../../C -Os -lpthread
 *
 * 使用：./firmware_create <input_firmware.bin> <output_firmware.lzma> [version]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "LzmaEnc.h"
#include "7zFile.h"
#include "7zCrc.h"
#include "7zAlloc.h"
#include "Alloc.h"

/* 固件头部结构（与 firmware_update.c 一致） */
typedef struct {
    uint32_t magic;          /* 魔数：0x46575246 ("FRWF") */
    uint32_t version;        /* 固件版本 */
    uint32_t compressed_size;/* 压缩数据大小 */
    uint32_t uncompressed_size; /* 未压缩数据大小 */
    uint32_t crc32;          /* 未压缩数据的 CRC32 */
    uint8_t  reserved[12];   /* 保留字段 */
} FirmwareHeader_t;

#define FIRMWARE_MAGIC 0x46575246

/* 全局内存分配器 */
static const ISzAlloc g_FirmwareAlloc = { SzAlloc, SzFree };

/* 文件读取回调（使用标准 FILE*） */
typedef struct {
    ISeqInStream vt;
    FILE *file;
} CStdioFileInStream;

static SRes FileInStream_Read(ISeqInStreamPtr p, void *buf, size_t *size)
{
    CStdioFileInStream *s = (CStdioFileInStream *)p;
    size_t read_size = fread(buf, 1, *size, s->file);
    *size = read_size;
    return SZ_OK;
}

/* 文件写入回调（使用标准 FILE*） */
typedef struct {
    ISeqOutStream vt;
    FILE *file;
} CFileOutStream2;

static size_t FileOutStream2_Write(ISeqOutStreamPtr p, const void *buf, size_t size)
{
    CFileOutStream2 *s = (CFileOutStream2 *)p;
    return fwrite(buf, 1, size, s->file);
}

/* 创建固件更新包 */
int create_firmware_package(
    const char *input_path,
    const char *output_path,
    uint32_t version)
{
    FILE *input_file = NULL;
    FILE *output_file = NULL;
    FirmwareHeader_t header;
    uint8_t *input_data = NULL;
    long file_size;
    uint32_t crc32;

    printf("=== 固件打包工具 ===\n\n");

    /* 打开输入文件 */
    input_file = fopen(input_path, "rb");
    if (!input_file) {
        fprintf(stderr, "错误：无法打开输入文件 '%s'\n", input_path);
        return -1;
    }

    /* 获取文件大小 */
    fseek(input_file, 0, SEEK_END);
    file_size = ftell(input_file);
    fseek(input_file, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "错误：输入文件为空\n");
        fclose(input_file);
        return -2;
    }

    printf("输入文件: %s\n", input_path);
    printf("文件大小: %ld bytes\n", file_size);

    /* 读取整个文件 */
    input_data = (uint8_t *)malloc(file_size);
    if (!input_data) {
        fprintf(stderr, "错误：内存分配失败\n");
        fclose(input_file);
        return -3;
    }

    if (fread(input_data, 1, file_size, input_file) != (size_t)file_size) {
        fprintf(stderr, "错误：读取文件失败\n");
        free(input_data);
        fclose(input_file);
        return -4;
    }

    fclose(input_file);

    /* 计算 CRC32 */
    CrcGenerateTable();
    crc32 = CrcCalc(input_data, file_size);
    printf("CRC32: 0x%08X\n", crc32);

    /* 打开输出文件 */
    output_file = fopen(output_path, "wb");
    if (!output_file) {
        fprintf(stderr, "错误：无法创建输出文件 '%s'\n", output_path);
        free(input_data);
        return -5;
    }

    /* 初始化头部（先占位） */
    memset(&header, 0, sizeof(FirmwareHeader_t));
    header.magic = FIRMWARE_MAGIC;
    header.version = version;
    header.uncompressed_size = (uint32_t)file_size;
    header.crc32 = crc32;

    /* 写入占位头部 */
    if (fwrite(&header, 1, sizeof(FirmwareHeader_t), output_file) != sizeof(FirmwareHeader_t)) {
        fprintf(stderr, "错误：写入头部失败\n");
        free(input_data);
        fclose(output_file);
        return -6;
    }

    /* 压缩数据 */
    printf("开始压缩...\n");

    /* 准备输入输出流 */
    CStdioFileInStream inStream;
    CFileOutStream2 outStream;

    inStream.vt.Read = FileInStream_Read;
    inStream.file = NULL;  /* 将在重新打开文件后设置 */

    outStream.vt.Write = FileOutStream2_Write;
    outStream.file = output_file;

    /* 重新打开输入文件用于压缩 */
    input_file = fopen(input_path, "rb");
    if (!input_file) {
        fprintf(stderr, "错误：无法重新打开输入文件\n");
        free(input_data);
        fclose(output_file);
        return -7;
    }
    inStream.file = input_file;
    /* 创建 LZMA 编码器 */
    CLzmaEncHandle enc = LzmaEnc_Create(&g_FirmwareAlloc);
    if (!enc) {
        fprintf(stderr, "错误：创建编码器失败\n");
        free(input_data);
        fclose(input_file);
        fclose(output_file);
        return -8;
    }

    /* 初始化编码参数 */
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    props.dictSize = 1 << 20;  /* 1MB 字典 */
    props.level = 5;           /* 中等压缩级别 */
    props.numThreads = 1;      /* 单线程（嵌入式友好） */

    SRes res = LzmaEnc_SetProps(enc, &props);
    if (res != SZ_OK) {
        fprintf(stderr, "错误：设置编码参数失败 (代码: %d)\n", res);
        LzmaEnc_Destroy(enc, &g_FirmwareAlloc, &g_AlignedAlloc);
        free(input_data);
        fclose(input_file);
        fclose(output_file);
        return -9;
    }

    /* 写入 LZMA 属性 */
    uint8_t lzma_props[LZMA_PROPS_SIZE];
    size_t props_size = LZMA_PROPS_SIZE;
    res = LzmaEnc_WriteProperties(enc, lzma_props, &props_size);
    if (res != SZ_OK) {
        fprintf(stderr, "错误：写入属性失败\n");
        LzmaEnc_Destroy(enc, &g_FirmwareAlloc, &g_AlignedAlloc);
        free(input_data);
        fclose(input_file);
        fclose(output_file);
        return -10;
    }

    if (fwrite(lzma_props, 1, props_size, output_file) != props_size) {
        fprintf(stderr, "错误：写入 LZMA 属性失败\n");
        LzmaEnc_Destroy(enc, &g_FirmwareAlloc, &g_AlignedAlloc);
        free(input_data);
        fclose(input_file);
        fclose(output_file);
        return -11;
    }

    /* 执行压缩 */
    res = LzmaEnc_Encode(enc, &outStream.vt, &inStream.vt, NULL, &g_FirmwareAlloc, &g_AlignedAlloc);

    /* 获取压缩后大小 */
    long compressed_size = ftell(output_file) - sizeof(FirmwareHeader_t);

    /* 清理 */
    LzmaEnc_Destroy(enc, &g_FirmwareAlloc, &g_AlignedAlloc);
    fclose(input_file);
    fclose(output_file);
    free(input_data);

    if (res != SZ_OK) {
        fprintf(stderr, "错误：压缩失败 (代码: %d)\n", res);
        remove(output_path);
        return -12;
    }

    /* 更新头部中的压缩大小 */
    header.compressed_size = (uint32_t)compressed_size;

    output_file = fopen(output_path, "r+b");
    if (output_file) {
        fwrite(&header, 1, sizeof(FirmwareHeader_t), output_file);
        fclose(output_file);
    }

    printf("压缩完成: %ld bytes -> %ld bytes\n", file_size, compressed_size);
    printf("压缩率: %.2f%%\n", (1.0 - (double)compressed_size / file_size) * 100.0);
    printf("输出文件: %s\n", output_path);

    return 0;
}

/* 打印使用说明 */
static void print_usage(const char *program_name)
{
    printf("用法: %s <输入文件.bin> <输出文件.lzma> [版本号]\n", program_name);
    printf("\n示例:\n");
    printf("  %s firmware.bin firmware_v1.0.lzma 100\n", program_name);
    printf("\n说明:\n");
    printf("  输入文件为原始固件二进制文件\n");
    printf("  输出文件为 LZMA 压缩的固件更新包\n");
    printf("  版本号为可选参数，默认为 1\n");
}

/* 主函数 */
int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];
    uint32_t version = 1;

    if (argc == 4) {
        version = (uint32_t)atoi(argv[3]);
    }

    int ret = create_firmware_package(input_path, output_path, version);

    if (ret != 0) {
        fprintf(stderr, "\n固件打包失败 (错误代码: %d)\n", ret);
        return 1;
    }

    return 0;
}