CC      := gcc
LD      := ld
AS      := nasm
QEMU    := qemu-system-x86_64
OBJCOPY := objcopy

# Флаги компилятора
# !!! ДОБАВЛЕНЫ ФЛАГИ -MMD -MP ДЛЯ АВТОМАТИЧЕСКОЙ ГЕНЕРАЦИИ ЗАВИСИМОСТЕЙ ОТ .h ФАЙЛОВ !!!
BASE_CFLAGS := -g -mcmodel=kernel -m64 -fno-merge-constants -fno-ident -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-omit-frame-pointer -fno-builtin -ffunction-sections -fdata-sections -IInclude -Oz -ffreestanding -fstack-protector-strong -fcf-protection=full -mno-red-zone -fno-pie -fno-allow-store-data-races -fno-delete-null-pointer-checks -fno-optimize-sibling-calls -fno-common -fno-strict-aliasing -Wno-address-of-packed-member -Wno-unused-parameter -fno-asynchronous-unwind-tables -freorder-blocks-algorithm=stc -fcf-protection=none -mindirect-branch=thunk-extern -fpartial-inlining -MMD -MP
DEBUG_CFLAGS := -m64 -g -O0 -DDEBUG -fno-lto -fno-omit-frame-pointer -fno-stack-protector -MMD -MP

# Флаги линковки
LDFLAGS := -m elf_x86_64 -nostdlib -static -T Link.ld --gc-sections --no-eh-frame-hdr -z max-page-size=0x1000 -z common-page-size=0x1000

# Флаги ассемблера
ASMFLAGS       := -f elf64 -O2
ASMFLAGS_DEBUG := -f elf64 -g -F dwarf

# Директории для исключения (не сканируем)
EXCLUDE_DIRS :=

# Функция для фильтрации файлов
filter_sources = $(filter-out $(foreach dir,$(EXCLUDE_DIRS),./$(dir)/%),$(1))

# Автоматический поиск всех исходников (с исключениями)
SRCS_ASM := $(call filter_sources,$(shell find . -type f -name '*.asm' ! -path './Bin/*'))
SRCS_C   := $(call filter_sources,$(shell find . -type f -name '*.c' ! -path './Bin/*'))
SRCS_S   := $(call filter_sources,$(shell find . -type f -name '*.S' ! -path './Bin/*'))

# Объекты (сохраняем структуру директорий)
ASM_OBJS := $(patsubst ./%.asm,Bin/%.asm.o,$(SRCS_ASM))
C_OBJS   := $(patsubst ./%.c,Bin/%.c.o,$(SRCS_C))
S_OBJS   := $(patsubst ./%.S,Bin/%.S.o,$(SRCS_S))
OBJECTS  := $(S_OBJS) $(ASM_OBJS) $(C_OBJS)

# !!! ГЕНЕРИРУЕМ СПИСОК .d ФАЙЛОВ НА ОСНОВЕ НАЙДЕННЫХ .c И .S ОБЪЕКТОВ !!!
DEPS     := $(patsubst Bin/%.c.o,Bin/%.c.d,$(C_OBJS)) $(patsubst Bin/%.S.o,Bin/%.S.d,$(S_OBJS))

BUILD_KERNEL := Bin/kernel
QEMU_OPTS ?=

.PHONY: all clean builddir run debug list exclude-list clean-orphans

# ============================================================================
# ОСНОВНАЯ СБОРКА
# ============================================================================

all: builddir clean-orphans $(OBJECTS) $(BUILD_KERNEL)

# ============================================================================
# ПРОВЕРКА ORPHAN-ОБЪЕКТНИКОВ (простая версия)
# ============================================================================

clean-orphans:
	@echo "=== Checking for orphan object files ==="
	@for obj in $(OBJECTS); do \
		if [ -f "$$obj" ]; then \
			src=""; \
			case "$$obj" in \
				*.c.o)   src=$$(echo "$$obj" | sed 's|^Bin/||' | sed 's|\.c\.o$$|.c|') ;; \
				*.asm.o) src=$$(echo "$$obj" | sed 's|^Bin/||' | sed 's|\.asm\.o$$|.asm|') ;; \
				*.S.o)   src=$$(echo "$$obj" | sed 's|^Bin/||' | sed 's|\.S\.o$$|.S|') ;; \
			esac; \
			if [ ! -f "$$src" ]; then \
				echo "  Removing orphan: $$obj"; \
				rm -f "$$obj"; \
			fi; \
		fi; \
	done

# ============================================================================
# ОСТАЛЬНЫЕ ПРАВИЛА
# ============================================================================

exclude-list:
	@echo "=== Excluded directories ==="
	@printf '%s\n' $(EXCLUDE_DIRS)

list:
	@echo "=== ASM files ==="
	@echo "$(SRCS_ASM)" | tr ' ' '\n'
	@echo "\n=== C files ==="
	@echo "$(SRCS_C)" | tr ' ' '\n'
	@echo "\n=== S files ==="
	@echo "$(SRCS_S)" | tr ' ' '\n'
	@echo "\n=== Total: $$(echo $(SRCS_ASM) $(SRCS_C) $(SRCS_S) | wc -w) files"

builddir:
	@mkdir -p Bin

# Правило для создания директорий
define create_dir
	@mkdir -p $(dir $@)
endef

# Правила для asm
Bin/%.asm.o: %.asm
	$(call create_dir)
	$(AS) $(ASMFLAGS) $< -o $@

# Правила для c
Bin/%.c.o: %.c
	$(call create_dir)
	$(CC) $(BASE_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

# Правила для .S (GNU as)
# !!! ДОБАВЛЕНЫ ФЛАГИ BASE_CFLAGS ДЛЯ СБОРКИ .S, ЧТОБЫ ТАМ ТОЖЕ СЛЕДИЛИ ЗА ИНКЛЮДАМИ !!!
Bin/%.S.o: %.S
	$(call create_dir)
	$(CC) $(BASE_CFLAGS) $(EXTRA_CFLAGS) -c -x assembler-with-cpp $< -o $@

# Сборка ядра
$(BUILD_KERNEL): $(OBJECTS) Link.ld
	$(call create_dir)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

debug: EXTRA_CFLAGS=$(DEBUG_CFLAGS)
debug: ASMFLAGS=$(ASMFLAGS_DEBUG)
debug: LDFLAGS = -m elf_x86_64 -nostdlib -static -T Link.ld --no-eh-frame-hdr
debug: clean all
	@echo "=== Debug build ready. Run: qemu-system-x86_64 -kernel $(BUILD_KERNEL) -s -S ==="

run-gdb: all
	$(QEMU) -kernel $(BUILD_KERNEL) -s -S $(QEMU_OPTS)

run: all
	$(QEMU) -kernel $(BUILD_KERNEL) $(QEMU_OPTS)

clean:
	rm -rf Bin

# ============================================================================
# !!! ПОДКЛЮЧЕНИЕ СГЕНЕРИРОВАННЫХ ФАЙЛОВ ЗАВИСИМОСТЕЙ !!!
# ============================================================================
-include $(DEPS)
