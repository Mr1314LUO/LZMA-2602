/* 创建测试固件包 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* 固件头部结构 */
typedef struct {
    uint32_t magic;          /* 0x46575246 ("FRWF") */
    uint32_t version;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32;
    uint8_t  reserved[12];
} FirmwareHeader_t;

#define FIRMWARE_MAGIC 0x46575246

int main(void)
{
    /* 创建一个测试文件，包含简单的固件头部 */
    FILE *f = fopen("firmware_v1.0.lzma", "wb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    FirmwareHeader_t header;
    memset(&header, 0, sizeof(header));
    header.magic = FIRMWARE_MAGIC;
    header.version = 100;
    header.uncompressed_size = 10240; /* 10KB */
    header.crc32 = 0;

    /* 写入头部 */
    fwrite(&header, 1, sizeof(FirmwareHeader_t), f);

    /* 写入一些占位数据（这不是真正的 LZMA 压缩数据） */
    uint8_t data[512];
    memset(data, 0xAA, sizeof(data));
    /* LZMA 属性占位 */
    uint8_t props[5] = {0x05, 0x00, 0x00, 0x00, 0x00};
    fwrite(props, 1, 5, f);
    fwrite(data, 1, 512, f);

    fclose(f);

    printf("已创建测试固件包: firmware_v1.0.lzma\n");
    printf("魔数: 0x%08X\n", header.magic);
    return 0;
}