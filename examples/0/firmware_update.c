/*
 * firmware_update.c - 固件更新解压缩示例
 *
 * 功能：使用 LZMA SDK 解压固件更新包
 * 适用：嵌入式系统、IoT 设备固件升级
 *
 * 编译：gcc -o firmware_update firmware_update.c \
 *           ../../C/LzmaDec.c ../../C/Alloc.c ../../C/7zFile.c ../../C/7zCrc.c \
 *           -I../../C -Os
 *
 * 使用：./firmware_update <compressed_firmware.lzma> <output_firmware.bin>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "LzmaDec.h"
#include "7zFile.h"
#include "7zCrc.h"
#include "7zAlloc.h"
#include "Alloc.h"

/* 缓冲区大小配置（可根据嵌入式系统内存调整） */
#define INPUT_BUFFER_SIZE  (4 * 1024)   /* 4KB 输入缓冲区 */
#define OUTPUT_BUFFER_SIZE (4 * 1024)   /* 4KB 输出缓冲区 */

/* 固件头部结构（自定义格式） */
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

/* 进度回调函数 */
typedef struct {
    uint64_t total_in;
    uint64_t total_out;
    int (*callback)(uint64_t in, uint64_t out, void *user_data);
    void *user_data;
} ProgressContext_t;

/* 打印进度信息 */
static int progress_callback(uint64_t in, uint64_t out, void *user_data)
{
    ProgressContext_t *ctx = (ProgressContext_t *)user_data;

    if (ctx && ctx->callback) {
        return ctx->callback(in, out, ctx->user_data);
    }

    /* 默认回调：每 10KB 打印一次 */
    if (out % (10 * 1024) == 0) {
        printf("\r已解压: %lu bytes", (unsigned long)out);
        fflush(stdout);
    }

    return 0; /* 返回 0 表示继续 */
}

/* 验证固件头部 */
static int verify_firmware_header(const FirmwareHeader_t *header)
{
    if (header->magic != FIRMWARE_MAGIC) {
        fprintf(stderr, "错误：无效的固件魔数 (0x%08X)\n", header->magic);
        return -1;
    }

    if (header->compressed_size == 0 || header->uncompressed_size == 0) {
        fprintf(stderr, "错误：固件大小无效\n");
        return -2;
    }

    printf("固件版本: %u\n", header->version);
    printf("压缩大小: %u bytes\n", header->compressed_size);
    printf("未压缩大小: %u bytes\n", header->uncompressed_size);

    return 0;
}

/* 计算 CRC32 */
static uint32_t calculate_crc32(const uint8_t *data, size_t length)
{
    CrcGenerateTable();
    return CrcCalc(data, length);
}

/* 流式解压固件数据 */
static int decompress_firmware_stream(
    FILE *input_file,
    FILE *output_file,
    uint64_t uncompressed_size,
    ProgressContext_t *progress)
{
    CLzmaDec state;
    SRes res;
    uint8_t in_buf[INPUT_BUFFER_SIZE];
    uint8_t out_buf[OUTPUT_BUFFER_SIZE];
    size_t in_pos = 0, in_size = 0, out_pos = 0;
    uint64_t remaining_size = uncompressed_size;
    uint64_t total_in = 0, total_out = 0;

    /* 初始化解码器状态 */
    LzmaDec_Construct(&state);

    /* 读取 LZMA 属性（5 字节） */
    uint8_t props[LZMA_PROPS_SIZE];
    if (fread(props, 1, LZMA_PROPS_SIZE, input_file) != LZMA_PROPS_SIZE) {
        fprintf(stderr, "错误：无法读取 LZMA 属性\n");
        return -1;
    }

    /* 分配解码器资源 */
    res = LzmaDec_Allocate(&state, props, LZMA_PROPS_SIZE, &g_FirmwareAlloc);
    if (res != SZ_OK) {
        fprintf(stderr, "错误：分配解码器失败 (代码: %d)\n", res);
        return -2;
    }

    /* 初始化解码器 */
    LzmaDec_Init(&state);

    printf("开始解压...\n");

    /* 主解压循环 */
    while (1) {
        /* 读取输入数据 */
        if (in_pos == in_size) {
            in_size = fread(in_buf, 1, INPUT_BUFFER_SIZE, input_file);
            if (in_size == 0) {
                if (remaining_size > 0) {
                    fprintf(stderr, "\n错误：输入数据不完整\n");
                    res = SZ_ERROR_DATA;
                }
                break;
            }
            in_pos = 0;
        }

        /* 准备解码参数 */
        SizeT in_processed = in_size - in_pos;
        SizeT out_processed = OUTPUT_BUFFER_SIZE - out_pos;
        ELzmaFinishMode finish_mode = LZMA_FINISH_ANY;
        ELzmaStatus status;

        /* 如果接近输出大小限制，使用结束模式 */
        if (out_processed > remaining_size) {
            out_processed = (SizeT)remaining_size;
            finish_mode = LZMA_FINISH_END;
        }

        /* 执行解码 */
        res = LzmaDec_DecodeToBuf(
            &state,
            out_buf + out_pos,
            &out_processed,
            in_buf + in_pos,
            &in_processed,
            finish_mode,
            &status
        );

        /* 更新位置 */
        in_pos += in_processed;
        out_pos += out_processed;
        remaining_size -= out_processed;
        total_in += in_processed;
        total_out += out_processed;

        /* 写入输出数据 */
        if (out_pos > 0) {
            size_t written = fwrite(out_buf, 1, out_pos, output_file);
            if (written != out_pos) {
                fprintf(stderr, "\n错误：写入输出文件失败\n");
                res = SZ_ERROR_WRITE;
                break;
            }
            out_pos = 0;
        }

        /* 调用进度回调 */
        if (progress) {
            if (progress_callback(total_in, total_out, progress) != 0) {
                printf("\n用户取消解压\n");
                res = SZ_ERROR_FAIL;
                break;
            }
        }

        /* 检查是否完成 */
        if (res != SZ_OK) {
            break;
        }

        if (remaining_size == 0) {
            break;
        }

        /* 检查是否停滞（无输入无输出） */
        if (in_processed == 0 && out_processed == 0) {
            fprintf(stderr, "\n错误：解码停滞\n");
            res = SZ_ERROR_DATA;
            break;
        }
    }

    /* 释放解码器资源 */
    LzmaDec_Free(&state, &g_FirmwareAlloc);

    if (res == SZ_OK) {
        printf("\r解压完成: %lu bytes\n", (unsigned long)total_out);
        return 0;
    } else {
        fprintf(stderr, "\n解压失败 (代码: %d)\n", res);
        return -3;
    }
}

/* 完整的固件更新流程 */
int update_firmware(const char *input_path, const char *output_path)
{
    FILE *input_file = NULL;
    FILE *output_file = NULL;
    FirmwareHeader_t header;
    int ret = 0;

    printf("=== 固件更新工具 ===\n\n");

    /* 打开输入文件 */
    input_file = fopen(input_path, "rb");
    if (!input_file) {
        fprintf(stderr, "错误：无法打开输入文件 '%s'\n", input_path);
        return -1;
    }

    /* 读取固件头部 */
    if (fread(&header, 1, sizeof(FirmwareHeader_t), input_file) != sizeof(FirmwareHeader_t)) {
        fprintf(stderr, "错误：无法读取固件头部\n");
        fclose(input_file);
        return -2;
    }

    /* 验证头部 */
    if (verify_firmware_header(&header) != 0) {
        fclose(input_file);
        return -3;
    }

    /* 打开输出文件 */
    output_file = fopen(output_path, "wb");
    if (!output_file) {
        fprintf(stderr, "错误：无法创建输出文件 '%s'\n", output_path);
        fclose(input_file);
        return -4;
    }

    /* 准备进度上下文 */
    ProgressContext_t progress = {
        .total_in = 0,
        .total_out = 0,
        .callback = NULL,
        .user_data = NULL
    };

    /* 解压固件数据 */
    ret = decompress_firmware_stream(
        input_file,
        output_file,
        header.uncompressed_size,
        &progress
    );

    if (ret != 0) {
        fclose(input_file);
        fclose(output_file);
        remove(output_path); /* 删除不完整的输出文件 */
        return ret;
    }

    /* 验证 CRC32（可选） */
    printf("验证 CRC32...\n");
    fclose(output_file);

    output_file = fopen(output_path, "rb");
    if (output_file) {
        /* 读取整个文件计算 CRC */
        fseek(output_file, 0, SEEK_END);
        long file_size = ftell(output_file);
        fseek(output_file, 0, SEEK_SET);

        uint8_t *verify_buf = (uint8_t *)malloc(file_size);
        if (verify_buf) {
            size_t read_size = fread(verify_buf, 1, file_size, output_file);
            uint32_t calc_crc = calculate_crc32(verify_buf, read_size);

            if (calc_crc == header.crc32) {
                printf("CRC32 验证通过: 0x%08X\n", calc_crc);
            } else {
                fprintf(stderr, "警告：CRC32 不匹配！\n");
                fprintf(stderr, "  期望: 0x%08X\n", header.crc32);
                fprintf(stderr, "  实际: 0x%08X\n", calc_crc);
                ret = -5;
            }

            free(verify_buf);
        }

        fclose(output_file);
    }

    fclose(input_file);

    if (ret == 0) {
        printf("\n固件更新包解压成功！\n");
        printf("输出文件: %s\n", output_path);
    }

    return ret;
}

/* 打印使用说明 */
static void print_usage(const char *program_name)
{
    printf("用法: %s <输入文件.lzma> <输出文件.bin>\n", program_name);
    printf("\n示例:\n");
    printf("  %s firmware_v1.0.lzma firmware_v1.0.bin\n", program_name);
    printf("\n说明:\n");
    printf("  输入文件应为 LZMA 压缩的固件包\n");
    printf("  输出文件为解压后的固件二进制文件\n");
}

/* 主函数 */
int main(int argc, char *argv[])
{
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];

    int ret = update_firmware(input_path, output_path);

    if (ret != 0) {
        fprintf(stderr, "\n固件更新失败 (错误代码: %d)\n", ret);
        return 1;
    }

    return 0;
}