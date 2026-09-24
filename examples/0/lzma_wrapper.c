/*
 * lzma_wrapper.c - 基于 liblzma (xz) 的固件解压缩封装
 *
 * 说明：
 *   本项目 workspace 中缺少 LZMA SDK 的完整源码（../../C 目录不存在），
 *   因此这里使用系统自带的 liblzma（xz-utils）提供 LZMA 解码能力。
 *   上层仍通过统一接口 lzma_decompress_file() 调用，
 *   若以后引入完整 LZMA SDK，只需替换本文件的实现即可。
 *
 *   注意：liblzma 的裸 LZMA1 解码依赖 header 参数（liblzma >= 5.1.0alpha）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <lzma.h>

#include "lzma_wrapper.h"

int lzma_decompress_file(const char *input_path, const char *output_path,
                         uint64_t *uncompressed_size)
{
    FILE *in_file = NULL;
    FILE *out_file = NULL;
    lzma_stream strm = LZMA_STREAM_INIT;
    uint8_t in_buf[LZMA_WRAPPER_BUF_SIZE];
    uint8_t out_buf[LZMA_WRAPPER_BUF_SIZE];
    lzma_action action;
    uint64_t total_out = 0;
    int ret = -1;

    in_file = fopen(input_path, "rb");
    if (!in_file) {
        fprintf(stderr, "lzma: 无法打开输入文件 '%s'\n", input_path);
        return -1;
    }

    out_file = fopen(output_path, "wb");
    if (!out_file) {
        fprintf(stderr, "lzma: 无法创建输出文件 '%s'\n", output_path);
        fclose(in_file);
        return -1;
    }

    /*
     * 使用裸 LZMA1 解码器。
     * 参数: lc=3, lp=0, pb=2（LZMA SDK 与 xz 的默认参数）
     * 与 lzma 命令行工具 `lzma -d` 生成的裸流兼容。
     */
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) {
        fprintf(stderr, "lzma: 初始化解码器失败\n");
        goto cleanup;
    }

    printf("开始解压: %s -> %s ...\n", input_path, output_path);

    for (;;) {
        if (strm.avail_in == 0 && !feof(in_file)) {
            strm.avail_in = fread(in_buf, 1, sizeof(in_buf), in_file);
            strm.next_in = in_buf;
            if (ferror(in_file)) {
                fprintf(stderr, "lzma: 读取输入文件失败\n");
                goto cleanup;
            }
        }

        action = feof(in_file) ? LZMA_FINISH : LZMA_RUN;

        strm.next_out = out_buf;
        strm.avail_out = sizeof(out_buf);

        lzma_ret lzret = lzma_code(&strm, action);

        if (lzret != LZMA_OK && lzret != LZMA_STREAM_END) {
            fprintf(stderr, "lzma: 解压失败 (错误码: %d)\n", (int)lzret);
            goto cleanup;
        }

        size_t written = sizeof(out_buf) - strm.avail_out;
        if (written > 0) {
            if (fwrite(out_buf, 1, written, out_file) != written) {
                fprintf(stderr, "lzma: 写入输出文件失败\n");
                goto cleanup;
            }
            total_out += written;
        }

        if (lzret == LZMA_STREAM_END) {
            break;
        }

        /* 输入耗尽且解码器仍需要数据 -> 数据不完整 */
        if (feof(in_file) && strm.avail_in == 0 && lzret != LZMA_STREAM_END) {
            fprintf(stderr, "lzma: 输入数据不完整\n");
            goto cleanup;
        }
    }

    printf("解压完成: %lu bytes\n", (unsigned long)total_out);
    if (uncompressed_size) {
        *uncompressed_size = total_out;
    }
    ret = 0;

cleanup:
    lzma_end(&strm);
    fclose(in_file);
    if (out_file) {
        fclose(out_file);
    }
    if (ret != 0) {
        remove(output_path);
    }
    return ret;
}
