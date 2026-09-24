#ifndef ZIP_H
#define ZIP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include "printf.h"

/* 打包器候选路径（依次尝试）
 * 注意：原 firmware_create 打包器已改名为 zip */
__attribute__((unused)) static const char *creator_candidates[] = {
    "lzma/build/zip",               /* 在项目根目录执行 */
    "build/zip",                    /* 在 lzma 目录执行  */
    "zip",                          /* 当前目录或 PATH   */
    NULL,
};

int compressed_File(const char *input_path , const char *output_path, const char *version);

#endif
