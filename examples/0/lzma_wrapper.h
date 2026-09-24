#ifndef LZMA_WRAPPER_H
#define LZMA_WRAPPER_H

#include <stdint.h>

/* 流式解压缓冲区大小 */
#define LZMA_WRAPPER_BUF_SIZE (4 * 1024)

/**
 * @brief 将 LZMA1 裸流文件解压为普通二进制文件
 * @param input_path  压缩文件路径 (.lzma)
 * @param output_path 输出文件路径
 * @param uncompressed_size 可选输出参数，返回解压后总字节数（可为 NULL）
 * @return 0 成功，-1 失败
 */
int lzma_decompress_file(const char *input_path, const char *output_path,
                         uint64_t *uncompressed_size);

#endif /* LZMA_WRAPPER_H */
