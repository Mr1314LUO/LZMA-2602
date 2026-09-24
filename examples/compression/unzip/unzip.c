/*
 * unzip.c - 固件流式解压工具（基于 liblzma 直接实现）
 *
 * 功能：直接调用 liblzma 库，将 LZMA 固件更新包流式解压为原始固件二进制文件。
 *       打包格式：[FirmwareHeader (32 bytes)][LZMA Properties (5 bytes)][LZMA 压缩数据]
 *
 * 特点：
 *   1. 流式解压：4KB 输入/输出缓冲区，低内存占用
 *   2. 实时进度显示：每 10KB 刷新一次
 *   3. CRC32 校验：解压完成后验证数据完整性
 *
 * 用法：
 *   ./unzip <输入固件.lzma> [输出固件.bin]
 *
 * 示例：
 *   ./unzip firmware_v1.0.lzma                # 输出 firmware_v1.0.unpacked
 *   ./unzip firmware_v1.0.lzma firmware.bin   # 输出 firmware.bin
 *
 * 编译：gcc -Wall -Wextra -O2 -o build/unzip unzip.c -llzma
 */

#include "unzip.h"

/* 打印使用说明 */
static void print_usage(const char *program_name)
{
    printf("用法: %s <输入固件.lzma> [输出固件.bin]\n", program_name);
    printf("\n示例:\n");
    printf("  %s firmware_v1.0.lzma                # 输出 firmware_v1.0.unpacked\n", program_name);
    printf("  %s firmware_v1.0.lzma firmware.bin   # 输出 firmware.bin\n", program_name);
    printf("\n说明:\n");
    printf("  直接调用 liblzma 流式解压 LZMA 固件更新包 (4KB 缓冲区)\n");
}

/* 生成默认输出文件名：将 .lzma 扩展名替换为 .unpacked */
static void make_output_name(const char *in_path, char *out_buf, size_t buf_size)
{
    const char *dot = strrchr(in_path, '.');
    size_t base_len;

    if (dot && strcmp(dot, ".lzma") == 0) {
        base_len = (size_t)(dot - in_path);
    } else {
        base_len = strlen(in_path);
    }

    if (base_len + 10 > buf_size) {
        base_len = buf_size - 10;
    }

    strncpy(out_buf, in_path, base_len);
    out_buf[base_len] = '\0';
    strcat(out_buf, ".unpacked");
}

/*
 * 解码 LZMA SDK 的 5 字节属性头到 lzma_options_lzma
 *
 * LZMA 属性格式：
 *   Byte 0: (pb * 5 + lp) * 9 + lc
 *   Bytes 1-4: 字典大小 (32位小端)
 */
static void decode_lzma_props(const uint8_t props[5], lzma_options_lzma *opts)
{
    uint8_t byte0 = props[0];
    opts->lc   = byte0 % 9;
    opts->lp   = (byte0 / 9) % 5;
    opts->pb   = byte0 / 45;
    opts->dict_size = (uint32_t)props[1]
                    | ((uint32_t)props[2] << 8)
                    | ((uint32_t)props[3] << 16)
                    | ((uint32_t)props[4] << 24);

    /* 解码器不需要这些字段，但为安全起见赋默认值 */
    opts->mode           = LZMA_MODE_FAST;
    opts->nice_len       = 64;
    opts->mf             = LZMA_MF_HC3;
    opts->depth          = 0;
    opts->preset_dict    = NULL;
    opts->preset_dict_size = 0;
}

/* 主函数：流式解压实现 */
int uncompressed_File(const char *input_path, const char *output_path)
{
    char              out_path[1024];
    FILE             *fin  = NULL;
    FILE             *fout = NULL;
    FirmwareHeader_t  header;
    lzma_stream       strm = LZMA_STREAM_INIT;
    lzma_options_lzma lzma_opts;
    lzma_filter       filters[2];
    uint8_t           in_buf[INPUT_BUFFER_SIZE];
    uint8_t           out_buf[OUTPUT_BUFFER_SIZE];
    uint8_t           lzma_props[5];
    uint32_t          crc32_calculated = 0;
    int               ret = UNZIP_ERROR_DECOMPRESS;
    int               progress_10kb = 0;   /* 进度跟踪：上次打印时的 10KB 倍数 */

    if (!input_path) {
        print_usage("unzip");
        return UNZIP_ERROR_OPEN_INPUT;
    }

    /* 确定输出路径 */
    if (output_path) {
        strncpy(out_path, output_path, sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
    } else {
        make_output_name(input_path, out_path, sizeof(out_path));
    }

    /* ========== 打开输入文件 ========== */
    fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "错误：无法打开输入文件 '%s'\n", input_path);
        return UNZIP_ERROR_OPEN_INPUT;
    }

    /* ========== 读取并验证固件头部 ========== */
    if (fread(&header, sizeof(header), 1, fin) != 1) {
        fprintf(stderr, "错误：无法读取固件头部\n");
        ret = UNZIP_ERROR_READ_HEADER;
        goto cleanup;
    }

    if (header.magic != FW_MAGIC) {
        fprintf(stderr, "错误：固件魔数无效 (期望 0x%08X, 实际 0x%08X)\n",
                FW_MAGIC, header.magic);
        ret = UNZIP_ERROR_MAGIC;
        goto cleanup;
    }

    /* ========== 读取 LZMA 属性 (5字节) ========== */
    if (fread(lzma_props, 1, 5, fin) != 5) {
        fprintf(stderr, "错误：无法读取 LZMA 属性\n");
        ret = UNZIP_ERROR_READ_HEADER;
        goto cleanup;
    }

    /* ========== 初始化 LZMA 流式解码器 ========== */
    decode_lzma_props(lzma_props, &lzma_opts);

    filters[0].id      = LZMA_FILTER_LZMA1;
    filters[0].options = &lzma_opts;
    filters[1].id      = LZMA_VLI_UNKNOWN;

    if (lzma_raw_decoder(&strm, filters) != LZMA_OK) {
        fprintf(stderr, "错误：初始化 LZMA 解码器失败\n");
        ret = UNZIP_ERROR_INIT_DECODER;
        goto cleanup;
    }

    /* ========== 打开输出文件 ========== */
    fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "错误：无法创建输出文件 '%s'\n", out_path);
        ret = UNZIP_ERROR_OPEN_OUTPUT;
        goto cleanup;
    }

    /* ========== 打印信息 ========== */
    printf("========================================\n");
    printf("   固件解压缩工具 (liblzma 流式解压)\n");
    printf("========================================\n\n");
    printf("输入文件:      %s\n", input_path);
    printf("输出文件:      %s\n", out_path);
    printf("固件版本:      %u\n", header.version);
    printf("压缩大小:      %u bytes\n", header.compressed_size);
    printf("原始大小:      %u bytes\n", header.uncompressed_size);
    printf("缓冲区大小:    %d / %d bytes\n\n", INPUT_BUFFER_SIZE, OUTPUT_BUFFER_SIZE);

    /* ========== 流式解压主循环 ========== */
    {
        lzma_action action = LZMA_RUN;
        lzma_ret    lzma_ret_val;
        uint64_t    total_out = 0;
        int         input_done = 0;   /* 输入文件是否已读完 */

        strm.avail_in  = 0;
        strm.next_in   = NULL;

        while (1) {
            /* 输入缓冲区为空且文件未读完时，从文件读取下一块 */
            if (strm.avail_in == 0 && !input_done) {
                strm.avail_in = fread(in_buf, 1, INPUT_BUFFER_SIZE, fin);
                strm.next_in  = in_buf;

                if (feof(fin)) {
                    input_done = 1;
                    action = LZMA_FINISH;
                }
            }

            /* 准备输出缓冲区 */
            strm.avail_out = OUTPUT_BUFFER_SIZE;
            strm.next_out  = out_buf;

            /* 执行解压 */
            lzma_ret_val = lzma_code(&strm, action);

            /* 写入解压后的数据 */
            size_t write_size = OUTPUT_BUFFER_SIZE - strm.avail_out;
            /* 按头部声明的原始大小截断：丢弃 LZMA1 原始流在无结束标记时
               结尾可能多解出的字节（否则输出会比 uncompressed_size 多，
               导致 CRC32 校验失败） */
            if (total_out < header.uncompressed_size) {
                size_t remaining = (size_t)(header.uncompressed_size - total_out);
                if (write_size > remaining) {
                    write_size = remaining;
                }
            } else {
                write_size = 0;
            }
            if (write_size > 0) {
                if (fwrite(out_buf, 1, write_size, fout) != write_size) {
                    fprintf(stderr, "错误：写入输出文件失败\n");
                    ret = UNZIP_ERROR_WRITE_OUTPUT;
                    goto cleanup;
                }

                /* 累加计算 CRC32 */
                crc32_calculated = lzma_crc32(out_buf, write_size, crc32_calculated);
                total_out += write_size;

                /* 实时进度显示：每 10KB 刷新一次 */
                int current_10kb = (int)(total_out / 10240);
                if (current_10kb > progress_10kb) {
                    progress_10kb = current_10kb;
                    int percent = (int)(total_out * 100 / header.uncompressed_size);
                    printf("\r解压进度: %d%% (%lu KB / %u KB)",
                           percent,
                           (unsigned long)(total_out / 1024),
                           header.uncompressed_size / 1024);
                    fflush(stdout);
                }
            }

            /* 检查解压结果 */

            /* 情况1: 正常解压完成（LZMA_STREAM_END） */
            if (lzma_ret_val == LZMA_STREAM_END) {
                printf("\r解压进度: 100%% (%u / %u KB)\n\n",
                       header.uncompressed_size / 1024,
                       header.uncompressed_size / 1024);
                ret = UNZIP_SUCCESS;
                break;
            }

            /* 情况2: 数据已读完，且解压量达到预期大小 —— 压缩包可能不含结束标记 */
            if (lzma_ret_val == LZMA_OK && input_done && strm.avail_in == 0
                && total_out >= header.uncompressed_size) {
                printf("\r解压进度: 100%% (%u / %u KB)\n\n",
                       header.uncompressed_size / 1024,
                       header.uncompressed_size / 1024);
                ret = UNZIP_SUCCESS;
                break;
            }

            /* 情况3: 真正出错 */
            if (lzma_ret_val != LZMA_OK) {
                fprintf(stderr, "\n错误：LZMA 解压失败 (错误码: %d)\n", (int)lzma_ret_val);
                ret = UNZIP_ERROR_DECOMPRESS;
                goto cleanup;
            }
        }

        /* ========== CRC32 校验 ========== */
        if (crc32_calculated != header.crc32) {
            fprintf(stderr, "错误：CRC32 校验失败 (计算值: 0x%08X, 期望值: 0x%08X)\n",
                    crc32_calculated, header.crc32);
            ret = UNZIP_ERROR_CRC;
            goto cleanup;
        }

        printf("CRC32 校验成功: 0x%08X\n", crc32_calculated);
        printf("\n固件流式解压完成: %s\n", out_path);
    }

cleanup:
    /* ========== 清理资源 ========== */
    lzma_end(&strm);
    if (fin)  fclose(fin);
    if (fout) fclose(fout);

    if (ret != UNZIP_SUCCESS && output_path == NULL) {
        /* 解压失败时删除不完整的输出文件 */
        remove(out_path);
    }

    return ret;
}