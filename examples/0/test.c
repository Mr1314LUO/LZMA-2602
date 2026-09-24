/*
 * test.c - 固件更新完整示例
 *
 * 本示例演示了完整的 OTA 固件更新流程：
 * 1. 读取压缩的固件更新包 (.lzma)
 * 2. 验证固件头部（魔数、版本、CRC32）
 * 3. 使用 LZMA 流式解码器解压固件数据（4KB 输入/输出缓冲）
 * 4. 将解压后的固件写入模拟 Flash
 * 5. 验证 CRC32 校验和
 *
 * 编译：gcc -o test test.c LzmaDec.c ../../C/Alloc.c ../../C/7zCrc.c \
 *           ../../C/7zCrcOpt.c ../../C/7zAlloc.c -I../../C -Os -lm
 * 使用：./test <firmware.lzma>
 *
 * 快速测试：
 *   # 先创建固件包: ./build/firmware_create test_firmware.bin test_firmware.lzma 1
 *   # 再运行本示例:  ./test test_firmware.lzma
 */

#include "unzip_streame.h"

/* ================================================================
 * 模拟 Flash 操作函数
 * ================================================================ */

/* 模拟 Flash 擦除（将区域置为 0xFF） */
static int flash_erase(uint32_t address, size_t size)
{
    if (!g_flash_base) {
        fprintf(stderr, "错误：Flash 未初始化\n");
        return -1;
    }
    if (address + size > FLASH_SIZE) {
        fprintf(stderr, "错误：擦除超出 Flash 范围 (addr=0x%X, size=%zu)\n",
                address, size);
        return -2;
    }

    memset(g_flash_base + address, FLASH_ERASE_VALUE, size);
    return 0;
}

/* 模拟 Flash 写入 */
static int flash_write(uint32_t address, const uint8_t *data, size_t size)
{
    if (!g_flash_base) {
        fprintf(stderr, "错误：Flash 未初始化\n");
        return -1;
    }
    if (address + size > FLASH_SIZE) {
        fprintf(stderr, "错误：写入超出 Flash 范围 (addr=0x%X, size=%zu)\n",
                address, size);
        return -2;
    }

    memcpy(g_flash_base + address, data, size);
    g_flash_offset = address + size;
    return 0;
}

/* 模拟 Flash 读取 */
static int flash_read(uint32_t address, uint8_t *buffer, size_t size)
{
    if (!g_flash_base) {
        fprintf(stderr, "错误：Flash 未初始化\n");
        return -1;
    }
    if (address + size > FLASH_SIZE) {
        fprintf(stderr, "错误：读取超出 Flash 范围\n");
        return -2;
    }

    memcpy(buffer, g_flash_base + address, size);
    return 0;
}

/* ================================================================
 * 进度回调函数
 * ================================================================ */

/* 用户自定义进度回调 */
static int my_progress_callback(uint64_t in, uint64_t out, void *user_data)
{
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
 * 流式解压：LZMA -> Flash，同时增量计算 CRC32
 * ================================================================ */
static int decompress_firmware_to_flash(
    FILE *fp,
    const FirmwareHeader_t *header,
    uint32_t *out_crc32)
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
    uint32_t      flash_addr = 0;
    UInt32        crc = CRC_INIT_VAL;

    CrcGenerateTable();

    if (fread(lzma_props, 1, LZMA_PROPS_SIZE, fp) != LZMA_PROPS_SIZE) {
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
            in_size = fread(in_buf, 1, sizeof(in_buf), fp);
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
            if (flash_write(flash_addr, out_buf, out_pos) != 0) {
                lzma_res = SZ_ERROR_WRITE;
                break;
            }
            crc = CrcUpdate(crc, out_buf, out_pos);
            flash_addr += (uint32_t)out_pos;
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

    if (out_crc32) {
        *out_crc32 = CRC_GET_DIGEST(crc);
    }

    return 0;
}

/* ================================================================
 * 核心：执行固件更新
 *
 * 输入：firmware_path - 压缩的固件包路径
 * 功能：
 *   1. 读取并验证固件头部
 *   2. 解压 LZMA 数据到模拟 Flash
 *   3. 验证 CRC32
 *   4. 输出更新结果
 * ================================================================ */
int perform_firmware_update(const char *firmware_path)
{
    FILE           *fp           = NULL;
    FirmwareHeader_t header;
    uint32_t        calc_crc     = 0;
    int             ret          = 0;
    long            file_size;

    printf("\n========================================\n");
    printf("    OTA 固件更新演示\n");
    printf("========================================\n\n");

    /* ---- 步骤 1: 分配模拟 Flash ---- */
    printf("[1/5] 分配模拟 Flash (%u MB)...\n", FLASH_SIZE / (1024 * 1024));
    g_flash_base = (uint8_t *)malloc(FLASH_SIZE);
    if (!g_flash_base) {
        fprintf(stderr, "错误：无法分配 Flash 内存\n");
        return -1;
    }
    /* 初始化 Flash 为擦除状态 */
    memset(g_flash_base, FLASH_ERASE_VALUE, FLASH_SIZE);

    /* ---- 步骤 2: 打开并读取固件包 ---- */
    printf("[2/5] 打开固件包: %s\n", firmware_path);

    fp = fopen(firmware_path, "rb");
    if (!fp) {
        fprintf(stderr, "错误：无法打开文件 '%s'\n", firmware_path);
        free(g_flash_base);
        g_flash_base = NULL;
        return -2;
    }

    /* 获取文件大小 */
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* 读取固件头部 */
    if (fread(&header, 1, sizeof(FirmwareHeader_t), fp) != sizeof(FirmwareHeader_t)) {
        fprintf(stderr, "错误：无法读取固件头部\n");
        fclose(fp);
        free(g_flash_base);
        g_flash_base = NULL;
        return -3;
    }

    /* ---- 步骤 3: 验证固件头部 ---- */
    printf("[3/5] 验证固件头部...\n");

    if (header.magic != FIRMWARE_MAGIC) {
        fprintf(stderr, "错误：无效的固件魔数 (期望: 0x%08X, 实际: 0x%08X)\n",
                FIRMWARE_MAGIC, header.magic);
        fclose(fp);
        free(g_flash_base);
        g_flash_base = NULL;
        return -4;
    }

    if (header.compressed_size == 0 || header.uncompressed_size == 0) {
        fprintf(stderr, "错误：固件大小无效 (压缩: %u, 未压缩: %u)\n",
                header.compressed_size, header.uncompressed_size);
        fclose(fp);
        free(g_flash_base);
        g_flash_base = NULL;
        return -5;
    }

    if (header.uncompressed_size > FLASH_SIZE) {
        fprintf(stderr, "错误：固件过大 (需 %u bytes, 但 Flash 仅 %u bytes)\n",
                header.uncompressed_size, FLASH_SIZE);
        fclose(fp);
        free(g_flash_base);
        g_flash_base = NULL;
        return -6;
    }

    /* 保存固件版本 */
    g_firmware_version = header.version;

    printf("  固件版本: %u\n", header.version);
    printf("  文件大小: %ld bytes\n", file_size);
    printf("  压缩大小: %u bytes\n", header.compressed_size);
    printf("  未压缩大小: %u bytes\n", header.uncompressed_size);
    printf("  CRC32:    0x%08X\n", header.crc32);
    printf("  压缩率:   %.2f%%\n",
           (1.0 - (double)header.compressed_size / header.uncompressed_size) * 100.0);

    /* ---- 步骤 4: 流式 LZMA 解压到模拟 Flash ---- */
    printf("[4/5] 开始流式解压固件到 Flash (输入 %u KB, 输出 %u KB)...\n\n",
           INPUT_BUFFER_SIZE / 1024, OUTPUT_BUFFER_SIZE / 1024);

    ret = decompress_firmware_to_flash(fp, &header, &calc_crc);
    fclose(fp);
    if (ret != 0) {
        free(g_flash_base);
        g_flash_base = NULL;
        return ret;
    }

    /* ---- 步骤 5: 验证 CRC32 ---- */
    printf("[5/5] 验证 CRC32...\n");

    if (calc_crc == header.crc32) {
        printf("  CRC32 验证通过: 0x%08X ✓\n", calc_crc);
    } else {
        fprintf(stderr, "  CRC32 验证失败！\n");
        fprintf(stderr, "    期望: 0x%08X\n", header.crc32);
        fprintf(stderr, "    实际: 0x%08X\n", calc_crc);
        free(g_flash_base);
        g_flash_base = NULL;
        return -12;
    }

    /* ---- 完成 ---- */
    printf("\n========================================\n");
    printf("  固件更新成功完成！\n");
    printf("========================================\n");
    printf("  固件版本: %u\n", g_firmware_version);
    printf("  Flash 写入: %zu / %u bytes\n",
           g_flash_offset, header.uncompressed_size);
    printf("  Flash 占用: %.2f%%\n",
           (double)g_flash_offset / FLASH_SIZE * 100.0);
    printf("========================================\n");

    free(g_flash_base);
    g_flash_base = NULL;

    return 0;
}

/* ================================================================
 * 打印使用说明
 * ================================================================ */
static void print_usage(const char *program_name)
{
    printf("用法: %s <固件包.lzma>\n", program_name);
    printf("\n示例:\n");
    printf("  # 1. 创建测试固件 (100KB 随机数据)\n");
    printf("  dd if=/dev/urandom of=test_firmware.bin bs=1024 count=100\n\n");
    printf("  # 2. 打包固件\n");
    printf("  ./build/firmware_create test_firmware.bin test_firmware.lzma 1\n\n");
    printf("  # 3. 运行本示例进行固件更新演示\n");
    printf("  %s test_firmware.lzma\n", program_name);
    printf("\n说明:\n");
    printf("  本程序模拟嵌入式 OTA 固件更新流程:\n");
    printf("  - 在内存中模拟 Flash 存储（4MB）\n");
    printf("  - 使用 LZMA 流式解压固件数据（低内存）\n");
    printf("  - 验证 CRC32 校验和\n");
    printf("  - 显示实时解压进度\n");
}

/* ================================================================
 * 主函数
 * ================================================================ */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *firmware_path = argv[1];
    int ret = perform_firmware_update(firmware_path);

    if (ret != 0) {
        fprintf(stderr, "\n固件更新失败 (错误代码: %d)\n", ret);
        return 1;
    }

    return 0;
}