# 固件更新解压缩示例

这个示例展示了如何使用 LZMA SDK 在嵌入式系统中实现固件更新功能。

## 项目结构

```
firmware_update/
├── firmware_update.c    # 固件解压工具（设备端）
├── firmware_create.c    # 固件打包工具（服务器端）
├── Makefile             # 编译脚本
└── README.md            # 说明文档
```

## 功能说明

### 1. firmware_create.c - 固件打包工具

用于在服务器端将原始固件二进制文件打包为 LZMA 压缩的固件更新包。

**固件包格式：**
```
[FirmwareHeader (32 bytes)] [LZMA Properties (5 bytes)] [LZMA Compressed Data]
```

**头部结构：**
```c
typedef struct {
    uint32_t magic;              // 0x46575246 ("FRWF")
    uint32_t version;            // 固件版本号
    uint32_t compressed_size;    // 压缩数据大小
    uint32_t uncompressed_size;  // 未压缩数据大小
    uint32_t crc32;              // CRC32 校验
    uint8_t  reserved[12];       // 保留字段
} FirmwareHeader_t;
```

**编译：**
```bash
make create
```

**使用：**
```bash
./firmware_create firmware.bin firmware_v1.0.lzma 100
```

### 2. firmware_update.c - 固件解压工具

用于在嵌入式设备端解压固件更新包。

**特性：**
- 流式解压（低内存占用）
- 进度回调
- CRC32 校验
- 错误处理

**编译：**
```bash
make
```

**使用：**
```bash
./firmware_update firmware_v1.0.lzma firmware_v1.0.bin
```

## 编译

### 编译解压工具（设备端）

```bash
make
```

### 编译打包工具（服务器端）

```bash
make create
```

### 运行测试

```bash
make test
```

### 清理

```bash
make clean
```

## 嵌入式系统适配

### 内存优化

对于资源受限的嵌入式系统，可以调整缓冲区大小：

```c
/* 在 firmware_update.c 中修改 */
#define INPUT_BUFFER_SIZE  (1 * 1024)   /* 1KB 输入缓冲区 */
#define OUTPUT_BUFFER_SIZE (1 * 1024)   /* 1KB 输出缓冲区 */
```

### 静态内存分配

避免使用动态内存分配：

```c
/* 自定义分配器使用静态缓冲区 */
static uint8_t g_static_buffer[256 * 1024];
static size_t g_buffer_offset = 0;

static void *StaticAlloc(void *p, size_t size)
{
    p = p;
    if (g_buffer_offset + size > sizeof(g_static_buffer))
        return NULL;
    void *ptr = g_static_buffer + g_buffer_offset;
    g_buffer_offset += size;
    return ptr;
}

static void StaticFree(void *p, void *address)
{
    p = p;
    /* 静态内存无需释放 */
}
```

### 编译选项优化

```bash
# 代码大小优化
gcc -Os -o firmware_update firmware_update.c ...

# 禁用未使用的功能
gcc -DZ7_LZMA_SIZE_OPT -o firmware_update firmware_update.c ...
```

## 集成到嵌入式项目

### 1. 复制必要文件

```bash
cp firmware_update.c your_project/
cp ../../C/LzmaDec.c your_project/
cp ../../C/7zTypes.h your_project/
```

### 2. 修改回调函数

根据你的嵌入式系统修改以下回调：

```c
/* 文件系统操作 */
static int flash_write(uint32_t address, const uint8_t *data, size_t size)
{
    /* 写入 Flash */
    return your_flash_write(address, data, size);
}

/* 进度回调 */
static int my_progress_callback(uint64_t in, uint64_t out, void *user_data)
{
    /* 更新进度条 */
    update_progress_bar(out);
    return 0;
}
```

### 3. 集成到固件更新流程

```c
int perform_firmware_update(const char *firmware_path)
{
    /* 1. 验证固件包 */
    /* 2. 解压到临时缓冲区 */
    /* 3. 验证 CRC32 */
    /* 4. 写入 Flash */
    /* 5. 重启系统 */
}
```

## 错误代码

| 代码 | 说明 |
|------|------|
| 0 | 成功 |
| -1 | 无法打开输入文件 |
| -2 | 无法读取固件头部 |
| -3 | 固件头部验证失败 |
| -4 | 无法创建输出文件 |
| -5 | CRC32 校验失败 |
| -6 | 写入头部失败 |
| -7 | 无法重新打开输入文件 |
| -8 | 创建编码器失败 |
| -9 | 设置编码参数失败 |
| -10 | 写入属性失败 |
| -11 | 写入 LZMA 属性失败 |
| -12 | 压缩失败 |

## 许可证

本示例代码基于 LZMA SDK（公共领域），可以自由使用、修改和分发。

## 参考

- [LZMA SDK 文档](../../DOC/lzma-sdk.txt)
- [LZMA 解码器说明](../../DOC/lzma.txt)
- [7z ANSI-C 解码器说明](../../DOC/7zC.txt)