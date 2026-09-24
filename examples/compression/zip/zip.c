/*
 * zip.c - 固件压缩工具（基于 firmware_create 打包器）
 *
 * 功能：调用已编译的 lzma/build/firmware_create 打包器，
 *       将原始固件二进制文件压缩为 LZMA 固件更新包。
 *       打包格式：[FirmwareHeader][LZMA Properties][LZMA 压缩数据]
 *
 * 用法：
 *   ./zip <输入固件.bin> <输出固件.lzma> [版本号]
 *
 * 示例：
 *   ./zip firmware.bin firmware_v1.0.lzma 100
 *
 * 说明：
 *   默认按以下顺序查找打包器：
 *     1. 环境变量 FIRMWARE_CREATE 指定的路径
 *     2. lzma/build/firmware_create（项目根目录下执行）
 *     3. build/firmware_create（lzma 目录下执行）
 *
 * 编译：gcc -Wall -Wextra -O2 -o build/zip zip.c
 */

#include "zip.h"

/* 打印使用说明 */
static void print_usage(const char *program_name)
{
    printf("\n用法: %s <输入固件.bin> <输出固件.lzma> [版本号]\n", program_name);
    printf("示例:\n");
    printf("  %s firmware.bin firmware_v1.0.lzma 100\n", program_name);
    printf("说明:\n");
    printf("  调用 lzma/build/firmware_create 将原始固件压缩为 LZMA 更新包\n");
    printf("  可通过环境变量 FIRMWARE_CREATE 指定其它打包器路径\n\n");
}

/* 解析打包器路径：环境变量优先，其次按候选列表依次探测 */
static const char *find_creator(void)
{
    const char *env = getenv("FIRMWARE_CREATE");
    if (env && *env) {
        if (access(env, X_OK) == 0) {
            return env;
        }
        fprintf(stderr, "警告：FIRMWARE_CREATE 指向的 '%s' 不存在或不可执行\n", env);
    }

    for (int i = 0; creator_candidates[i] != NULL; i++) {
        if (access(creator_candidates[i], X_OK) == 0) {
            return creator_candidates[i];
        }
    }
    return NULL;
}

/* 主函数 */
int compressed_File(const char *input_path , const char *output_path, const char *version)
{
    if (!input_path || !output_path || !version) {
        print_usage("zip");
        return 1;
    }

    /* 检查输入文件 */
    struct stat st;
    if (stat(input_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "错误：无法打开输入文件 '%s'\n", input_path);
        return 1;
    }

    /* 查找打包器 */
    const char *creator = find_creator();
    if (!creator) {
        fprintf(stderr, "错误：找不到打包器 firmware_create，请先编译 lzma 目录\n");
        return 1;
    }

    printf("=== 固件压缩工具 ===\n");
    printf("打包器: %s\n", creator);
    printf("输入:   %s\n", input_path);
    printf("输出:   %s\n", output_path);
    printf("版本:   %s\n", version);
    printf("\n");

    /* 子进程执行打包器 */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "错误：fork 失败 (%s)\n", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        execl(creator, creator, input_path, output_path, version, (char *)NULL);
        fprintf(stderr, "错误：执行 '%s' 失败 (%s)\n", creator, strerror(errno));
        _exit(127);
    }

    /* 父进程等待打包器结束 */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "错误：waitpid 失败 (%s)\n", strerror(errno));
        return 1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("\n固件压缩完成: %s\n", output_path);
        return 0;
    }

    fprintf(stderr, "固件压缩失败 (退出码: %d)\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 1;
}
