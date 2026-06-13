# =============================================================================
# Directory Structure
# =============================================================================
SRC_DIR       := src
OBJ_DIR       := obj
BIN_DIR       := bin

# =============================================================================
# Project Configuration
# =============================================================================
PROJECT_NAME  := firmware
TARGET        := $(BIN_DIR)/$(PROJECT_NAME)
GIT_HASH      := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_TIME    := $(shell date -u +'%Y-%m-%d_%H:%M_UTC')
BUILD_TAG     := $(shell date -u +'%Y%m%d_%H%M')

# =============================================================================
# Source Files
# =============================================================================
SRC := $(wildcard $(SRC_DIR)/*.c) \
       $(wildcard $(SRC_DIR)/driver/*.c) \
       $(wildcard $(SRC_DIR)/helper/*.c) \
       $(wildcard $(SRC_DIR)/ui/*.c) \
       $(wildcard $(SRC_DIR)/apps/*.c)

OBJS := $(OBJ_DIR)/start.o \
        $(OBJ_DIR)/init.o \
        $(OBJ_DIR)/external/printf/printf.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_adc.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_comp.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_crc.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_dac.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_dma.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_exti.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_gpio.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_i2c.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_lptim.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_pwr.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_rcc.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_rtc.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_spi.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_tim.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_usart.o \
        $(OBJ_DIR)/external/PY32F071_HAL_Driver/Src/py32f071_ll_utils.o \
        $(OBJ_DIR)/external/littlefs/lfs.o \
        $(OBJ_DIR)/external/littlefs/lfs_util.o \
        $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# =============================================================================
# Toolchain
# =============================================================================
TOOLCHAIN_PREFIX := arm-none-eabi-
AS       := $(TOOLCHAIN_PREFIX)gcc
CC       := $(TOOLCHAIN_PREFIX)gcc
LD       := $(TOOLCHAIN_PREFIX)gcc
OBJCOPY  := $(TOOLCHAIN_PREFIX)objcopy
SIZE     := $(TOOLCHAIN_PREFIX)size

# =============================================================================
# Compiler Flags
# =============================================================================
# Common flags for AS and CC
COMMON_FLAGS := -mcpu=cortex-m0plus -mthumb -mabi=aapcs
OPTIMIZATION := -Os -flto=auto -ffunction-sections -fdata-sections

# Assembler flags
ASFLAGS  := $(COMMON_FLAGS) -c

# Compiler flags
CFLAGS   := $(COMMON_FLAGS) $(OPTIMIZATION) \
            -std=c2x \
            -Wall -Wextra \
            -Wno-missing-field-initializers \
            -Wno-incompatible-pointer-types \
            -Wno-strict-aliasing \
            -Wno-unused-function -Wno-unused-variable \
            -fshort-enums \
            -Wno-unused-parameter \
            -fno-delete-null-pointer-checks \
            -fsingle-precision-constant \
            -finline-functions-called-once \
            -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -fno-math-errno -fno-stack-protector \
            -fmerge-all-constants -fconserve-stack \
            -MMD -MP

# Debug/Release specific flags
DEBUG_FLAGS   := -g3 -DDEBUG -Og
RELEASE_FLAGS := -g0 -DNDEBUG

# Defines
DEFINES  := -DPRINTF_INCLUDE_CONFIG_H \
            -DGIT_HASH=\"$(GIT_HASH)\" \
            -DTIME_STAMP=\"$(BUILD_TIME)\" \
            -DPY32F071xB \
            -DUSE_FULL_LL_DRIVER \
            -DTUD_RHPORT \
            -DLFS_NO_MALLOC \
            -DLFS_NO_ASSERT \
            -DLFS_NO_DEBUG \
            -DLFS_NO_WARN \
            -DLFS_FILE_MAX=64 \
            -DLFS_NO_ERROR

# Include paths
INC_DIRS := -I./src/config \
            -I./src/external/CMSIS/Device/PY32F071/Include \
            -I./src/external/CMSIS/Include \
            -I./src/external/littlefs \
            -I./src/external/PY32F071_HAL_Driver/Inc \
            -I./src/external/tinyusb/src

# =============================================================================
# Linker Flags
# =============================================================================

LDFLAGS  := $(COMMON_FLAGS) \
            -nostartfiles \
            -Tfirmware.ld \
            -lnosys -lm \
            -nostdlib \
            -ffreestanding \
            -Wl,--gc-sections \
            --specs=nano.specs \
            -Wl,--build-id=none \
            -Wl,--print-memory-usage \
            -Wl,-Map=$(OBJ_DIR)/output.map

# =============================================================================
# Build Configuration
# =============================================================================
# По умолчанию release сборка
BUILD_TYPE ?= release

ifeq ($(BUILD_TYPE),debug)
    CFLAGS += $(DEBUG_FLAGS)
    OPTIMIZATION := -Og
else
    CFLAGS += $(RELEASE_FLAGS)
endif

# =============================================================================
# Build Rules
# =============================================================================
.PHONY: all debug release clean help info flash

# Основная цель
all: $(TARGET).bin
	@echo "Build completed: $(TARGET).bin"

# Debug сборка
debug:
	@$(MAKE) BUILD_TYPE=debug all

# Release сборка с копированием
release:
	@$(MAKE) BUILD_TYPE=release all
	@cp $(TARGET).bin $(BIN_DIR)/k1-fw-alfa-by-fagci-$(BUILD_TAG).bin
	@echo "Release firmware: $(BIN_DIR)/k1-fw-alfa-by-fagci-$(BUILD_TAG).bin"

# Генерация бинарного файла
$(TARGET).bin: $(TARGET)
	@echo "Creating binary file..."
	$(OBJCOPY) -O binary $< $@

# Линковка
$(TARGET): $(OBJS) | $(BIN_DIR)
	@echo "Linking..."
	$(LD) $(LDFLAGS) $^ -o $@
	@echo ""
	$(SIZE) $@
	arm-none-eabi-nm -S --size-sort -r --radix=d $(BIN_DIR)/$(PROJECT_NAME) | head -30
	@echo ""

# Компиляция C файлов
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(@D)
	@echo "CC $<"
	@$(CC) $(CFLAGS) $(DEFINES) $(INC_DIRS) -c $< -o $@

# Компиляция ассемблерных файлов
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.s | $(OBJ_DIR)
	@mkdir -p $(@D)
	@echo "AS $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Создание директорий
$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $@

# =============================================================================
# Utility Targets
# =============================================================================

# Показать информацию о сборке
info:
	@echo "Project Information:"
	@echo "  Project:     $(PROJECT_NAME)"
	@echo "  Git Hash:    $(GIT_HASH)"
	@echo "  Build Time:  $(BUILD_TIME)"
	@echo "  Build Type:  $(BUILD_TYPE)"
	@echo ""
	@echo "Toolchain:"
	@echo "  CC:          $(CC)"
	@echo "  LD:          $(LD)"
	@echo ""
	@echo "Source Files: $(words $(SRC)) files"
	@echo "Object Files: $(words $(OBJS)) files"

# Очистка
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(TARGET) $(TARGET).* $(OBJ_DIR) $(BIN_DIR)/*.bin
	@echo "Clean completed"

# Очистка всего включая зависимости
distclean:
	@echo "Deep cleaning..."
	@rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Distclean completed"

# Помощь
help:
	@echo "Available targets:"
	@echo "  all      - Build firmware (default: release mode)"
	@echo "  debug    - Build firmware with debug symbols"
	@echo "  release  - Build release firmware with timestamp"
	@echo "  clean    - Remove build artifacts"
	@echo "  distclean- Remove all generated files"
	@echo "  info     - Show build configuration"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make              # Build release version"
	@echo "  make debug        # Build debug version"
	@echo "  make release      # Build and package release"
	@echo "  make BUILD_TYPE=debug  # Alternative debug build"

# =============================================================================
# Dependencies
# =============================================================================
DEPS := $(OBJS:.o=.d)
-include $(DEPS)
