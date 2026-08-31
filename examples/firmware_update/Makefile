# 固件更新解压缩示例 - Makefile
# 适用于 Linux/macOS 嵌入式开发环境

# 编译器设置
CC = gcc
CFLAGS = -Wall -Wextra -Os -I../../C
LDFLAGS =

BUILD_DIR = build

# 源文件
LZMA_SRC = ../../C/LzmaDec.c \
           ../../C/Alloc.c \
           ../../C/7zFile.c \
           ../../C/7zCrc.c \
           ../../C/7zCrcOpt.c \
           ../../C/7zAlloc.c

APP_SRC = firmware_update.c
CREATE_SRC = firmware_create.c

# 目标文件
TARGET = $(BUILD_DIR)/firmware_update
CREATE_TARGET = $(BUILD_DIR)/firmware_create

# 对象文件
OBJS := $(addprefix $(BUILD_DIR)/, $(notdir $(APP_SRC:.c=.o)))


# 编译规则
all: $(TARGET) $(CREATE_TARGET)

$(TARGET): $(APP_SRC) $(LZMA_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(APP_SRC) $(LZMA_SRC) $(LDFLAGS)
	@echo "编译完成: $(TARGET)"

$(CREATE_TARGET): $(CREATE_SRC) $(LZMA_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CREATE_SRC) $(LZMA_SRC) ../../C/LzmaEnc.c ../../C/LzFind.c ../../C/LzFindMt.c ../../C/LzFindOpt.c ../../C/Threads.c ../../C/CpuArch.c $(LDFLAGS) -lpthread
	@echo "编译完成: $(CREATE_TARGET)"

# 清理
clean:
	rm -f $(TARGET) *.o
	@echo "清理完成"

# 测试
test: $(TARGET) $(CREATE_TARGET)
	@echo "运行测试..."
	@echo "测试数据创建中..."
	@dd if=/dev/urandom of=test_firmware.bin bs=1024 count=100 2>/dev/null
	@echo "打包测试..."
	@./$(CREATE_TARGET) test_firmware.bin test_firmware.lzma 100
	@echo "解压测试..."
	@./$(TARGET) test_firmware.lzma test_output.bin
	@echo "验证文件..."
	@cmp test_firmware.bin test_output.bin && \
		echo "测试通过！" || \
		echo "测试失败！"
	@rm -f test_firmware.bin test_firmware.lzma test_output.bin

# 帮助
help:
	@echo "可用的目标:"
	@echo "  all     - 编译固件更新工具（默认）"
	@echo "  clean   - 清理编译产物"
	@echo "  test    - 运行测试"
	@echo "  help    - 显示此帮助信息"

.PHONY: all clean test help