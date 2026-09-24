/*
 * flow-unzip.c - 固件流式解压工具
 *
 * 本程序直接从 LZMA 固件包解压出原始固件文件：
 * 1. 读取压缩的固件更新包 (.lzma)
 * 2. 验证固件头部（魔数、版本、CRC32）
 * 3. 使用 LZMA 流式解码器解压固件数据（4KB 输入/输出缓冲）
 * 4. 将解压后的固件写入输出文件
 *
 * 编译：gcc -o flow-unzip unzip_stream.c LzmaDec.c ../sdk/Alloc.c ../sdk/7zCrc.c \
 *           ../sdk/7zCrcOpt.c ../sdk/7zAlloc.c -I../sdk -Os -lm
 * 使用：./flow-unzip <输入固件.lzma> [输出固件.bin]
 *
 * 快速测试：
 *   # 先创建固件包: ./build/firmware_create test_firmware.bin test_firmware.lzma 1
 *   # 再运行本示例:  ./flow-unzip test_firmware.lzma output.bin
 */

#include "unzip_stream.h"

/* ================================================================
 * 文件输出操作函数
 * ================================================================ */

/* 将解压数据写入输出文件（fw_output_write_fn 回调适配器） */
static int file_write_cb(void *write_user, const uint8_t *data, size_t size)
{
    FILE *fp = (FILE *)write_user;
    if (fwrite(data, 1, size, fp) != size) {
        fprintf(stderr, "错误：写入输出文件失败\n");
        return -1;
    }
    return 0;
}

/* ================================================================
 * 进度回调函数
 * ================================================================ */

/* 用户自定义进度回调 */
static int my_progress_callback(uint64_t in, uint64_t out, void *user_data)
{
    (void)in; /* 输入字节数暂不用于进度计算 */
    uint64_t total_uncompressed = *(uint64_t *)user_data;
    int percent = 0;

    if (total_uncompressed > 0) {
        percent = (int)(out * 100 / total_uncompressed);
    }

    /* 仅在百分比变化时更新显示，避免刷屏 */
    if (percent != g_last_percent) {
        printf("\r  解压进度: %3d%%  (%lu / %lu bytes)",
               percent, (unsigned long)out, (unsigned long)total_uncompressed);
        fflush(stdout);
        g_last_percent = percent;
    }

    return 0; /* 返回 0 表示继续，返回非 0 可取消操作 */
}

/* ================================================================
 * 流式解压：LZMA -> write_fn 回调（文件/Flash 等任意输出），
 * 同时增量计算 CRC32
 * ================================================================ */
static int decompress_firmware_stream(
    FILE *in_fp,
    fw_output_write_fn write_fn, void *write_user,
    const FirmwareHeader_t *header,
    uint32_t *out_crc32, uint64_t *out_total)
{
    CLzmaDec      lzma_state;
    SRes          lzma_res = SZ_OK;
    uint8_t       in_buf[INPUT_BUFFER_SIZE];
    uint8_t       out_buf[OUTPUT_BUFFER_SIZE];
    uint8_t       lzma_props[LZMA_PROPS_SIZE];
    size_t        in_pos = 0;
    size_t        in_size = 0;
    size_t        out_pos = 0;
    uint64_t      remaining = header->uncompressed_size;
    uint64_t      total_in = 0;
    uint64_t      total_out = 0;
    UInt32        crc = CRC_INIT_VAL;

    CrcGenerateTable();

    if (fread(lzma_props, 1, LZMA_PROPS_SIZE, in_fp) != LZMA_PROPS_SIZE) {
        fprintf(stderr, "错误：无法读取 LZMA 压缩属性\n");
        return -8;
    }

    LzmaDec_Construct(&lzma_state);
    lzma_res = LzmaDec_Allocate(&lzma_state, lzma_props, LZMA_PROPS_SIZE,
                                &g_FirmwareAlloc);
    if (lzma_res != SZ_OK) {
        fprintf(stderr, "错误：LZMA 解码器初始化失败 (代码: %d)\n", lzma_res);
        return -9;
    }

    LzmaDec_Init(&lzma_state);

    while (remaining > 0) {
        if (in_pos == in_size) {
            in_size = fread(in_buf, 1, sizeof(in_buf), in_fp);
            if (in_size == 0) {
                fprintf(stderr, "\n错误：输入数据不完整\n");
                lzma_res = SZ_ERROR_DATA;
                break;
            }
            in_pos = 0;
        }

        SizeT src_processed = in_size - in_pos;
        SizeT dst_processed = OUTPUT_BUFFER_SIZE - out_pos;
        ELzmaFinishMode finish = LZMA_FINISH_ANY;
        ELzmaStatus status;

        if (dst_processed > remaining) {
            dst_processed = (SizeT)remaining;
            finish = LZMA_FINISH_END;
        }

        lzma_res = LzmaDec_DecodeToBuf(
            &lzma_state,
            out_buf + out_pos, &dst_processed,
            in_buf + in_pos, &src_processed,
            finish, &status
        );

        in_pos += src_processed;
        out_pos += dst_processed;
        remaining -= dst_processed;
        total_in += src_processed;
        total_out += dst_processed;

        if (out_pos > 0) {
            if (write_fn(write_user, out_buf, out_pos) != 0) {
                lzma_res = SZ_ERROR_WRITE;
                break;
            }
            crc = CrcUpdate(crc, out_buf, out_pos);
            out_pos = 0;
        }

        {
            uint64_t total = header->uncompressed_size;
            my_progress_callback(total_in, total_out, &total);
        }

        if (lzma_res != SZ_OK) {
            if (lzma_res == SZ_ERROR_DATA) {
                fprintf(stderr, "\n错误：LZMA 数据损坏\n");
            }
            break;
        }

        if (src_processed == 0 && dst_processed == 0) {
            fprintf(stderr, "\n错误：解码停滞\n");
            lzma_res = SZ_ERROR_DATA;
            break;
        }
    }

    printf("\n\n");
    LzmaDec_Free(&lzma_state, &g_FirmwareAlloc);

    if (lzma_res != SZ_OK) {
        fprintf(stderr, "错误：解压失败 (代码: %d)\n", lzma_res);
        return -10;
    }

    if (total_out != header->uncompressed_size) {
        fprintf(stderr, "错误：解压大小不匹配 (期望 %u, 实际 %lu)\n",
                header->uncompressed_size, (unsigned long)total_out);
        return -11;
    }

    g_total_uncompressed = total_out;

    if (out_crc32) {
        *out_crc32 = CRC_GET_DIGEST(crc);
    }
    if (out_total) {
        *out_total = total_out;
    }

    return 0;
}

/* ================================================================
 * 核心：执行固件流式解压（输出到 write_fn 回调，如直接写 Flash）
 *
 * 输入：firmware_path - 压缩的固件包路径
 *       write_fn      - 解压输出回调（返回非 0 中止）
 *       write_user    - 回调用户上下文
 * 功能：
 *   1. 读取并验证固件头部
 *   2. 解压 LZMA 数据逐块写入 write_fn（不落盘）
 *   3. 验证 CRC32
 * ================================================================ */
int perform_firmware_update_stream(const char *firmware_path,
                                   fw_output_write_fn write_fn, void *write_user,
                                   uint32_t *out_crc32, uint64_t *out_total)
{
    FILE           *in_fp         = NULL;
    FirmwareHeader_t header;
    uint32_t        calc_crc     = 0;
    uint64_t        total_out    = 0;
    int             ret          = 0;
    long            file_size;

    printf("\n========================================\n");
    printf("    固件流式解压工具\n");
    printf("========================================\n\n");

    /* ---- 步骤 1: 打开输入固件包 ---- */
    printf("[1/4] 打开固件包: %s\n", firmware_path);

    in_fp = fopen(firmware_path, "rb");
    if (!in_fp) {
        fprintf(stderr, "错误：无法打开输入文件 '%s'\n", firmware_path);
        return -2;
    }

    /* 获取文件大小 */
    fseek(in_fp, 0, SEEK_END);
    file_size = ftell(in_fp);
    fseek(in_fp, 0, SEEK_SET);

    /* 读取固件头部 */
    if (fread(&header, 1, sizeof(FirmwareHeader_t), in_fp) != sizeof(FirmwareHeader_t)) {
        fprintf(stderr, "错误：无法读取固件头部\n");
        fclose(in_fp);
        return -3;
    }

    /* ---- 步骤 2: 验证固件头部 ---- */
    printf("[2/4] 验证固件头部...\n");

    if (header.magic != FIRMWARE_MAGIC) {
        fprintf(stderr, "错误：无效的固件魔数 (期望: 0x%08X, 实际: 0x%08X)\n",
                FIRMWARE_MAGIC, header.magic);
        fclose(in_fp);
        return -4;
    }

    if (header.compressed_size == 0 || header.uncompressed_size == 0) {
        fprintf(stderr, "错误：固件大小无效 (压缩: %u, 未压缩: %u)\n",
                header.compressed_size, header.uncompressed_size);
        fclose(in_fp);
        return -5;
    }

    g_total_uncompressed = header.uncompressed_size;

    printf("  固件版本: %u\n", header.version);
    printf("  文件大小: %ld bytes\n", file_size);
    printf("  压缩大小: %u bytes\n", header.compressed_size);
    printf("  未压缩大小: %u bytes\n", header.uncompressed_size);
    printf("  CRC32:    0x%08X\n", header.crc32);
    printf("  压缩率:   %.2f%%\n",
           (1.0 - (double)header.compressed_size / header.uncompressed_size) * 100.0);

    /* ---- 步骤 3: 流式解压，逐块写入 write_fn ---- */
    printf("[3/4] 开始流式解压 (输入 %u KB, 输出 %u KB)...\n\n",
           INPUT_BUFFER_SIZE / 1024, OUTPUT_BUFFER_SIZE / 1024);

    ret = decompress_firmware_stream(in_fp, write_fn, write_user,
                                     &header, &calc_crc, &total_out);
    fclose(in_fp);
    if (ret != 0) {
        return ret;
    }

    /* ---- 步骤 4: 验证 CRC32 ---- */
    printf("[4/4] 验证 CRC32...\n");

    if (calc_crc == header.crc32) {
        printf("  CRC32 验证通过: 0x%08X ✓\n", calc_crc);
    } else {
        fprintf(stderr, "  CRC32 验证失败！\n");
        fprintf(stderr, "    期望: 0x%08X\n", header.crc32);
        fprintf(stderr, "    实际: 0x%08X\n", calc_crc);
        return -12;
    }

    /* ---- 完成 ---- */
    printf("\n========================================\n");
    printf("  固件流式解压成功完成！\n");
    printf("========================================\n");
    printf("  固件版本: %u\n", header.version);
    printf("  解压大小: %lu bytes\n", (unsigned long)total_out);
    printf("========================================\n");

    if (out_crc32) {
        *out_crc32 = calc_crc;
    }
    if (out_total) {
        *out_total = total_out;
    }

    return 0;
}

/* ================================================================
 * 文件输出封装：解压 .lzma 包到输出文件（独立 CLI 工具使用）
 * ================================================================ */
int perform_firmware_update(const char *firmware_path, const char *output_path)
{
    FILE     *out_fp  = NULL;
    uint32_t  calc_crc = 0;
    uint64_t  total_out = 0;
    int       ret;

    out_fp = fopen(output_path, "wb");
    if (!out_fp) {
        fprintf(stderr, "错误：无法创建输出文件 '%s'\n", output_path);
        return -6;
    }

    ret = perform_firmware_update_stream(firmware_path,
                                         file_write_cb, out_fp,
                                         &calc_crc, &total_out);
    fclose(out_fp);
    if (ret != 0) {
        return ret;
    }

    printf("  输出文件: %s\n", output_path);
    return 0;
}

/* ================================================================
 * 命令行工具外壳
 * 默认作为库链接进 ota_main（不编译 main，避免入口冲突）；
 * 独立编译工具时加 -DFLOW_UNZIP_STANDALONE
 * ================================================================ */
#ifdef FLOW_UNZIP_STANDALONE

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

/* ================================================================
 * 打印使用说明
 * ================================================================ */
static void print_usage(const char *program_name)
{
    printf("用法: %s <输入固件.lzma> [输出固件.bin]\n", program_name);
    printf("\n示例:\n");
    printf("  # 1. 创建测试固件 (100KB 随机数据)\n");
    printf("  dd if=/dev/urandom of=test_firmware.bin bs=1024 count=100\n\n");
    printf("  # 2. 打包固件\n");
    printf("  ./build/firmware_create test_firmware.bin test_firmware.lzma 1\n\n");
    printf("  # 3. 运行本程序解压固件\n");
    printf("  %s test_firmware.lzma\n", program_name);
    printf("  或: %s test_firmware.lzma output.bin\n", program_name);
    printf("\n说明:\n");
    printf("  本程序将 LZMA 压缩固件包解压为原始固件文件:\n");
    printf("  - 使用 LZMA 流式解压（低内存占用，4KB 输入/输出缓冲）\n");
    printf("  - 验证 CRC32 校验和\n");
    printf("  - 显示实时解压进度\n");
    printf("  - 如果不指定输出文件名，自动生成 .unpacked 后缀\n");
}

/* ================================================================
 * 主函数
 * ================================================================ */
int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    char default_output[1024];

    const char *output_path = (argc == 3) ? argv[2] : NULL;

    /* 如果用户没有指定输出文件名，自动生成 */
    if (!output_path) {
        make_output_name(input_path, default_output, sizeof(default_output));
        output_path = default_output;
    }

    int ret = perform_firmware_update(input_path, output_path);

    if (ret != 0) {
        fprintf(stderr, "\n固件解压失败 (错误代码: %d)\n", ret);
        return 1;
    }

    return 0;
}

#endif /* FLOW_UNZIP_STANDALONE */