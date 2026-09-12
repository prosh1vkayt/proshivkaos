# Makefile — proshivkaOS NEXT
#
# Две архитектуры и три профиля сборки:
#
#   ARCH=x86   (по умолчанию)          ARCH=arm64
#     text   — текстовый shell в VGA      text   — текстовый shell в UART
#     gui    — оконный интерфейс (XP)     —
#     touch  — тач-интерфейс (мышь        touch  — тач-интерфейс (тачскрин
#              изображает палец)                   через virtio-input)
#
# Быстрый старт:
#   make run                 текстовый shell на x86 в QEMU
#   make gui-run             оконный интерфейс на x86
#   make touch-run           тач-интерфейс на x86 (мышь вместо пальца)
#   make ARCH=arm64 run      текстовый shell на ARM64 через serial
#   make ARCH=arm64 touch-run  тач-интерфейс на ARM64 — основная цель проекта
#   make ARCH=arm64 image    сырой ARM64 Linux Image (build/arm64/proshivkaos.img)
#   make ARCH=arm64 bootimg  Android boot.img под fastboot flash boot
#
# Требования:
#   x86   : nasm + кросс-тулчейн i686-elf-* (Homebrew) или i386-elf-* (MacPorts)
#   arm64 : aarch64-elf-gcc + aarch64-elf-binutils (brew install aarch64-elf-gcc)
#   оба   : qemu-system-i386 / qemu-system-aarch64
#
# ВАЖНО про macOS: системный /usr/bin/ld — это Apple ld64, он НЕ понимает
# GNU-опцию -T для ELF-линкер-скриптов. Кросс-тулчейн обязателен, фолбэк на
# хостовой ld тут не работает ни для одной из архитектур.

ARCH ?= x86

# ============================================================================
#  Тулчейн
# ============================================================================
# У GNU Make есть встроенные значения CC=cc и LD=ld, выставленные ДО чтения
# этого файла, поэтому проверять "ifeq ($(CC),)" бессмысленно — переменная
# никогда не пустая. Спрашиваем $(origin ...): 'default' значит "встроенное
# значение, пользователь его не задавал", 'command line' — "передано явно
# через make CC=...", такое не трогаем.

ifeq ($(ARCH),x86)

  AS := nasm
  ifeq ($(origin CC),default)
    ifneq (, $(shell which i686-elf-gcc 2>/dev/null))
      CC := i686-elf-gcc
    else ifneq (, $(shell which i386-elf-gcc 2>/dev/null))
      CC := i386-elf-gcc
    else
      CC := gcc
      CFLAGS_HOST_FALLBACK := -m32
    endif
  endif
  ifeq ($(origin LD),default)
    ifneq (, $(shell which i686-elf-ld 2>/dev/null))
      LD := i686-elf-ld
    else ifneq (, $(shell which i386-elf-ld 2>/dev/null))
      LD := i386-elf-ld
    else
      LD := ld
    endif
  endif

  ARCH_CFLAGS  := $(CFLAGS_HOST_FALLBACK)
  LINKER_SCRIPT := linker.ld
  LDFLAGS      := -T $(LINKER_SCRIPT) -nostdlib -m elf_i386
  ASFLAGS      := -f elf32
  QEMU         := qemu-system-i386

else ifeq ($(ARCH),arm64)

  # Порядок предпочтения: bare-metal тулчейны (elf/none-elf) идут первыми —
  # они не тянут заголовки и стартовый код Linux-userspace, которых у нас нет.
  ifeq ($(origin CC),default)
    ifneq (, $(shell which aarch64-elf-gcc 2>/dev/null))
      CC := aarch64-elf-gcc
    else ifneq (, $(shell which aarch64-none-elf-gcc 2>/dev/null))
      CC := aarch64-none-elf-gcc
    else ifneq (, $(shell which aarch64-linux-gnu-gcc 2>/dev/null))
      CC := aarch64-linux-gnu-gcc
    else
      $(error Не найден кросс-компилятор под aarch64. Установите: brew install aarch64-elf-gcc)
    endif
  endif
  ifeq ($(origin LD),default)
    ifneq (, $(shell which aarch64-elf-ld 2>/dev/null))
      LD := aarch64-elf-ld
    else ifneq (, $(shell which aarch64-none-elf-ld 2>/dev/null))
      LD := aarch64-none-elf-ld
    else
      LD := aarch64-linux-gnu-ld
    endif
  endif
  OBJCOPY ?= $(patsubst %-ld,%-objcopy,$(LD))

  # -mgeneral-regs-only: запретить компилятору использовать регистры FP/SIMD.
  #   В ядре они не сохраняются при переключении контекста и вдобавок по
  #   умолчанию отключены в CPACR_EL1 — обращение к ним даёт исключение.
  # -mstrict-align: не генерировать невыровненные обращения к памяти.
  #   До включения MMU вся память трактуется как Device-nGnRnE, где
  #   невыровненный доступ — гарантированный abort.
  # -fpie + -pie: образ собирается позиционно-независимым. Загрузчик
  # телефона кладёт ядро не по тому адресу, на который оно слинковано, и без
  # этого все указатели, зашитые компоновщиком в данные, оказываются
  # недействительны. Компоновщик складывает их список в .rela.dyn, а
  # arch/arm64/boot.S проходит его на старте и правит.
  # -fvisibility=hidden убирает лишнюю косвенность через GOT: все символы
  # свои, экспортировать наружу нечего.
  ARCH_CFLAGS   := -mgeneral-regs-only -mstrict-align -fno-common \
                   -fpie -fvisibility=hidden
  LINKER_SCRIPT := arch/arm64/linker.ld
  # --no-warn-rwx-segments: компоновщик предупреждает, что весь образ лежит
  # в одном сегменте с правами на чтение, запись и исполнение сразу. Для
  # обычной программы это замечание по делу, а здесь иначе и не бывает:
  # разделение прав обеспечивает MMU по таблицам страниц, которых на входе
  # ещё нет — образ разворачивается до того, как включится трансляция.
  # Предупреждение относится к тому, чего в этом слое просто не существует.
  LDFLAGS       := -T $(LINKER_SCRIPT) -nostdlib -pie --no-dynamic-linker \
                   -z notext --no-warn-rwx-segments
  QEMU          := qemu-system-aarch64

  # Параметры упаковки в Android boot.img (fastboot). Значения по умолчанию —
  # типовые для Qualcomm; под конкретный аппарат берутся из его исходников
  # ядра или из распакованного стокового boot.img.
  # Значения по умолчанию совпадают с заводскими для Redmi Note 4 (mido) —
  # они сняты с его стокового boot.img, а не взяты из общих рекомендаций.
  # Для другого аппарата их надо переопределить: самый надёжный способ
  # узнать свои — распаковать стоковый образ (magiskboot unpack boot.img).
  BOOTIMG_BASE      ?= 0x80000000
  BOOTIMG_PAGESIZE  ?= 2048
  # Своя командная строка ядру не нужна: адреса берутся из device tree, а
  # не из cmdline. Оставляем пустой, чтобы ничего не обещать загрузчику.
  BOOTIMG_CMDLINE   ?=

else
  $(error Неизвестная ARCH=$(ARCH). Допустимо: x86, arm64)
endif

# Разрешение экрана тач-сборки — ОБЩЕЕ для обеих архитектур. 480x960 —
# портрет 2:1, как у современных телефонов. На ARM64 его получает ramfb,
# на x86 — VBE (интерфейс Bochs умеет произвольные разрешения, не только
# каноничные 640x480/800x600, поэтому портрет там тоже доступен).
# Верхняя граница — GFXFB_MAX_PIXELS (720x1440), см. gui/gfxfb.h.
# Игнорируется, если BOARD задаёт готовый фреймбуфер загрузчика (mido) —
# там разрешение диктует само железо, см. arch/arm64/boards/mido.h.
SCREEN_W ?= 480
SCREEN_H ?= 960

# Конкретная плата (имеет смысл только для ARCH=arm64). По умолчанию —
# эмулятор (arch/arm64/boards/qemu.h). BOARD=mido подключает
# arch/arm64/boards/mido.h: адреса реального Xiaomi Redmi Note 4/4X,
# взятые из мейнлайн device tree ядра Linux. См. docs/PORT_MIDO.md — там
# же честно перечислено, что уже проверено на бумаге, а что нет.
BOARD ?= qemu

# --- Конфигурация платы ------------------------------------------------
# Набор включённых возможностей лежит в configs/<плата>_defconfig, а их
# перечень с зависимостями описан в Kconfig (формат ядра Linux — если
# проект дорастёт до menuconfig, он заработает как есть).
#
# Каждая строка вида CONFIG_X=y превращается в макрос -DCONFIG_X и заодно
# решает, какие .c-файлы попадут в сборку. Держать одно в двух местах
# (список файлов в Makefile и #ifdef в коде) не пришлось: и то и другое
# выводится из одного файла.
BOARD_CONFIG := configs/$(BOARD)_defconfig

ifeq ($(ARCH),arm64)
  ifeq ($(wildcard $(BOARD_CONFIG)),)
    $(error Нет конфигурации для BOARD=$(BOARD). Ожидался файл $(BOARD_CONFIG))
  endif

  BOARD_CONFIGS := $(shell sed -n 's/^\(CONFIG_[A-Z0-9_]*\)=y$$/\1/p' $(BOARD_CONFIG))
  ARCH_CFLAGS   += $(addprefix -D,$(BOARD_CONFIGS))
endif

# Включена ли возможность: $(call cfg,TOUCH_FT5X06)
cfg = $(filter CONFIG_$(1),$(BOARD_CONFIGS))

BUILD  := build/$(ARCH)-$(BOARD)
OBJDIR := $(BUILD)/obj

# libgcc — вспомогательные функции самого компилятора. Своей libc у нас нет
# и не будет, но libgcc это не libc: там лежат вещи, которые компилятор
# подставляет САМ, когда у процессора нет подходящей инструкции. Например,
# деление 64-битного числа на 32-битном x86 (__udivdi3): аптайм в
# миллисекундах его использует, и без libgcc линковка падает.
LIBGCC := $(shell $(CC) $(ARCH_CFLAGS) -print-libgcc-file-name 2>/dev/null)

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie -nostdlib \
          -fno-builtin -Wall -Wextra -O2 $(ARCH_CFLAGS) \
          -DPROSHIVKA_SCREEN_W=$(SCREEN_W) -DPROSHIVKA_SCREEN_H=$(SCREEN_H) \
          -Ihal -Ifs -Ishell -Iapps -Igui -Igui/touch -Iarch/$(ARCH) \
          $(EXTRA_CFLAGS)

# ============================================================================
#  Списки исходников
# ============================================================================

# --- Общее ядро: не знает ни одной архитектурной подробности ---------------
CORE_SOURCES := \
    kernel/kstring.c \
    hal/hal_mem.c \
    hal/hal_thread.c \
    fs/ramfs.c

# --- Графика, общая для всех платформ --------------------------------------
GFX_COMMON_SOURCES := \
    gui/palette.c \
    gui/gfxfb.c \
    gui/hal_gfx.c \
    gui/font8x8.c

ifeq ($(ARCH),x86)

  ARCH_BASE_SOURCES := \
      arch/x86/cpu.c \
      arch/x86/rtc.c \
      arch/x86/pit.c \
      hal/hal_time_x86.c

  # Текстовый профиль
  TEXT_SOURCES := $(CORE_SOURCES) \
      kernel/kernel.c kernel/panic.c \
      hal/hal_console.c \
      arch/x86/vga.c arch/x86/keyboard.c arch/x86/cpu.c \
	  shell/shell.c apps/editor.c
  TEXT_ASM := boot/boot.asm

  # Графический бэкенд экрана и ввода
  ARCH_GFX_SOURCES := \
      arch/x86/vga13h.c arch/x86/pci.c arch/x86/vbe.c \
      arch/x86/hal_gfx_x86.c
  ARCH_INPUT_SOURCES := \
      arch/x86/keyboard.c arch/x86/mouse.c hal/hal_input_x86.c

  BOOT_ASM := boot/boot.asm

else

  ARCH_BASE_SOURCES := \
      arch/arm64/cpu.c \
      arch/arm64/fdt.c \
      arch/arm64/platform.c \
      arch/arm64/uart.c \
      arch/arm64/uart_pl011.c \
      arch/arm64/uart_msm.c \
      arch/arm64/mmu.c \
      arch/arm64/timer.c

  # Постоянный журнал есть не у всякой платы: он пишет в область ОЗУ,
  # которую устройство само исключило из общего пула. У QEMU такой нет.
  ifneq ($(call cfg,LOG_RAMOOPS),)
    ARCH_BASE_SOURCES += arch/arm64/ramoops.c arch/arm64/blackbox.c
  endif
  ifneq ($(call cfg,EARLY_FB_MARKS),)
    ARCH_BASE_SOURCES += arch/arm64/early_fb.c arch/arm64/early_con.c arch/arm64/boot_anim.c
  endif

  TEXT_SOURCES := $(CORE_SOURCES) $(ARCH_BASE_SOURCES) \
      kernel/kernel.c kernel/panic.c \
      hal/hal_console_arm64.c \
      shell/shell.c apps/editor.c
  TEXT_ASM :=

  ARCH_GFX_SOURCES := \
      arch/arm64/fwcfg.c arch/arm64/ramfb.c arch/arm64/hal_gfx_arm64.c
  ARCH_INPUT_SOURCES := \
      arch/arm64/virtio_input.c hal/hal_input_arm64.c

  # Драйверы реального телефона подключаются только там, где они есть.
  # В сборке под эмулятор их нет вовсе — вместо них работают слабые
  # заглушки в hal/hal_input_arm64.c, и слой ввода разницы не замечает.
  ifneq ($(call cfg,TLMM_GPIO),)
    ARCH_INPUT_SOURCES += arch/arm64/tlmm.c arch/arm64/keys_gpio.c arch/arm64/reboot_msm.c
  endif
  ifneq ($(call cfg,CLK_GCC_MSM8953),)
    ARCH_INPUT_SOURCES += arch/arm64/gcc_msm8953.c
  endif
  ifneq ($(call cfg,I2C_QUP),)
    ARCH_INPUT_SOURCES += arch/arm64/i2c_qup.c arch/arm64/i2c_bitbang.c
  endif
  ifneq ($(call cfg,TOUCH_FT5X06),)
    ARCH_INPUT_SOURCES += arch/arm64/touch_ft5x06.c
  endif
  ifneq ($(call cfg,PMIC_SPMI),)
    ARCH_BASE_SOURCES += arch/arm64/spmi_msm.c arch/arm64/pmic_pm8953.c
  endif
  ifneq ($(call cfg,RPM_SMD),)
    ARCH_BASE_SOURCES += arch/arm64/smem_msm.c arch/arm64/smd_rpm.c
  endif
  ifneq ($(call cfg,USB_DWC3),)
    ARCH_BASE_SOURCES += arch/arm64/usb_dwc3.c arch/arm64/usb_pos.c
  endif

  BOOT_ASM :=
  BOOT_S   := arch/arm64/boot.S arch/arm64/vectors.S

endif

# --- Оконный интерфейс (десктоп, только x86) -------------------------------
GUI_SOURCES := $(CORE_SOURCES) $(GFX_COMMON_SOURCES) $(ARCH_BASE_SOURCES) \
    $(ARCH_GFX_SOURCES) $(ARCH_INPUT_SOURCES) \
    kernel/kernel_gui.c kernel/panic_gui.c \
    gui/window.c gui/wallpaper.c \
    gui/wallpaper_320x200.c gui/wallpaper_640x480.c gui/wallpaper_800x600.c \
    gui/cursor.c gui/gconsole.c gui/terminal_app.c gui/settings_app.c gui/wm.c

# --- Тач-интерфейс (обе архитектуры) ---------------------------------------
# Запечённые обои (gui/wallpaper_*.c) сюда НЕ входят намеренно: они
# нарисованы под landscape-разрешения 4:3 и на портретном экране телефона
# бесполезны. Фон рисуется градиентом, см. gui/touch/touch_ui.c.
TOUCH_SOURCES := $(CORE_SOURCES) $(GFX_COMMON_SOURCES) $(ARCH_BASE_SOURCES) \
    $(ARCH_GFX_SOURCES) $(ARCH_INPUT_SOURCES) \
    kernel/kernel_touch.c kernel/panic_gui.c \
    gui/window.c gui/cursor.c gui/gconsole.c gui/terminal_app.c \
    gui/touch/touch_theme.c \
    gui/touch/osk.c \
    gui/touch/app_terminal.c \
    gui/touch/app_settings.c \
    gui/touch/app_files.c \
    gui/touch/app_about.c \
    gui/touch/touch_ui.c

# ============================================================================
#  Преобразование списков исходников в объектные файлы
# ============================================================================
# Объектники складываются в build/$(ARCH)-$(BOARD)/obj/, а не рядом с исходниками:
# иначе .o от x86-сборки и от arm64-сборки перезаписывали бы друг друга,
# и "make ARCH=arm64" после "make" собирал бы франкенштейна.
obj_of = $(patsubst %.c,$(OBJDIR)/%.o,$(patsubst %.asm,$(OBJDIR)/%.o,$(patsubst %.S,$(OBJDIR)/%.o,$(1))))

# Уникализация: некоторые файлы (например arch/x86/keyboard.c) попадают в
# список дважды — линкер на дубликаты объектников ругается.
uniq = $(if $(1),$(firstword $(1)) $(call uniq,$(filter-out $(firstword $(1)),$(1))))

TEXT_OBJ  := $(call uniq,$(call obj_of,$(TEXT_SOURCES) $(TEXT_ASM) $(BOOT_S)))
GUI_OBJ   := $(call uniq,$(call obj_of,$(GUI_SOURCES) $(BOOT_ASM) $(BOOT_S)))
TOUCH_OBJ := $(call uniq,$(call obj_of,$(TOUCH_SOURCES) $(BOOT_ASM) $(BOOT_S)))

KERNEL_ELF  := $(BUILD)/kernel.elf
GUI_ELF     := $(BUILD)/kernel_gui.elf
TOUCH_ELF   := $(BUILD)/kernel_touch.elf
RAW_IMAGE   := $(BUILD)/proshivkaos.img
BOOT_IMG    := $(BUILD)/proshivkaos_boot.img
BOARD_DTS   := arch/arm64/boards/$(BOARD).dts
BOARD_DTB   := $(BUILD)/$(BOARD).dtb
PACKED_KERN := $(BUILD)/Image.gz-dtb
PACKED_RAW  := $(BUILD)/Image-dtb-uncompressed
BOOT_IMG_RAW:= $(BUILD)/proshivkaos_boot_uncompressed.img
BOOT_IMG_SWAP:= $(BUILD)/proshivkaos_boot_swap.img
ISO         := $(BUILD)/proshivkaos.iso

.PHONY: all text gui touch clean run gui-run touch-run iso image bootimg help mido

all: text

# ============================================================================
#  Правила компиляции
# ============================================================================
# -MMD -MP: рядом с каждым объектным файлом компилятор кладёт список
# заголовков, от которых тот зависит. Без этого правки в заголовке не
# вызывают пересборку, и в образ попадают устаревшие объектники — ошибка
# тем неприятная, что выглядит как ошибка в коде.
#
# Мы на ней уже обожглись: boot.S берёт адрес области журнала из
# boards/mido.h, адрес поменялся, а объектник остался старый — и система
# писала журнал по адресу, которого в новой прошивке нет. Собиралось всё
# при этом без единого замечания.
# Объекты зависят и от файла настроек платы.
#
# Без этой строки правка configs/<плата>_defconfig не пересобирала
# ничего: она меняет только -D в командной строке, а make о них не знает.
# Признак включаешь, собираешь, запускаешь — и получаешь прежний образ
# без единого намёка на то, что он прежний. Один такой заход уже потерян.
$(OBJDIR)/%.o: %.c $(BOARD_CONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(shell find build -name '*.d' 2>/dev/null)

$(OBJDIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# ============================================================================
#  Цели сборки
# ============================================================================
text: $(KERNEL_ELF)
$(KERNEL_ELF): $(TEXT_OBJ) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(TEXT_OBJ) $(LIBGCC)
	@echo "собрано: $@"

gui: $(GUI_ELF)
$(GUI_ELF): $(GUI_OBJ) $(LINKER_SCRIPT)
ifneq ($(ARCH),x86)
	$(error Профиль gui существует только для ARCH=x86 — он завязан на VGA/VBE. \
	        Для ARM64 используйте профиль touch)
endif
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(GUI_OBJ) $(LIBGCC)
	@echo "собрано: $@"

touch: $(TOUCH_ELF)
$(TOUCH_ELF): $(TOUCH_OBJ) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(TOUCH_OBJ) $(LIBGCC)
	@echo "собрано: $@"

# ============================================================================
#  Образы для реального железа (только ARM64)
# ============================================================================
# Сырой ARM64 Linux Image: ELF-обвязка снимается, остаётся то, что грузчик
# кладёт в память как есть. Заголовок Image уже внутри — см. arch/arm64/boot.S.
image: $(TOUCH_ELF)
ifneq ($(ARCH),arm64)
	$(error Цель image имеет смысл только для ARCH=arm64)
endif
	$(OBJCOPY) -O binary $(TOUCH_ELF) $(RAW_IMAGE)
	@echo "собрано: $(RAW_IMAGE) ($$(wc -c < $(RAW_IMAGE)) байт)"

# Всё, что нужно для Redmi Note 4/4X, одной командой. Отдельная цель — не
# синтаксический сахар: под mido меняется и BOARD (константы платы), и
# каталог сборки, и путь к образу, и перепутать их между запусками легко.
mido:
	$(MAKE) ARCH=arm64 BOARD=mido bootimg
	$(MAKE) ARCH=arm64 BOARD=mido bootimg-raw
	@echo
	@echo "Готово: build/arm64-mido/proshivkaos_boot.img"
	@echo "Проверка БЕЗ записи в память устройства:"
	@echo "    fastboot boot build/arm64-mido/proshivkaos_boot.img"

# Android boot.img — то, что принимает fastboot. Собственный упаковщик в
# tools/mkbootimg.py: формат заголовка v0 простой, а тащить ради него
# зависимость из AOSP незачем.
bootimg: $(PACKED_KERN)
	python3 tools/mkbootimg.py \
	    --kernel $(PACKED_KERN) \
	    --base $(BOOTIMG_BASE) \
	    --pagesize $(BOOTIMG_PAGESIZE) \
	    --cmdline "$(BOOTIMG_CMDLINE)" \
	    --output $(BOOT_IMG)

# Дерево устройств платы.
# -p 4096: запас свободного места внутри дерева. Загрузчик дописывает в
# него своё — узел готового экрана, командную строку, границы initrd, — и
# без запаса ему пришлось бы дерево переразмещать. Такой путь у него есть,
# но он умеет отказать ("Failed to move/resize dtb buffer"), а четыре
# килобайта ничего не стоят.
$(BOARD_DTB): $(BOARD_DTS)
	@mkdir -p $(dir $@)
	dtc -I dts -O dtb -p 4096 -o $@ $<

# Ядро в том виде, в каком его принимает загрузчик телефона: сжатое, с
# приделанным следом деревом устройств.
#
# И то и другое обязательно, причём по одной и той же причине. LK ищет
# приделанное дерево ровно двумя способами (platform/msm_shared/dev_tree.c,
# функция dev_tree_appended):
#
#   - у СЖАТОГО ядра смещение возвращает распаковщик: там, где кончился
#     поток gzip, начинается дерево;
#   - у несжатого читается 32-битное слово по смещению 0x2C.
#
# Второе — соглашение 32-битного zImage. У ARM64 Image по смещению 0x2C
# лежит зарезервированное поле, то есть ноль, и загрузчик уходит искать
# дерево в самое начало образа, где вместо него заголовок Image. Разбор
# проваливается, и загрузка кончается отказом:
#
#     ERROR: Appended Device Tree Blob not found
#
# Именно поэтому образ из несжатого Image без дерева не грузился ничем,
# кроме подмены ядра в стоковом boot.img через magiskboot: там дерево
# доставалось от стокового ядра, а сжатие делал сам magiskboot.
$(PACKED_KERN): image $(BOARD_DTB)
	gzip -n -9 -c $(RAW_IMAGE) > $(BUILD)/Image.gz
	cat $(BUILD)/Image.gz $(BOARD_DTB) > $@
	@echo "упаковано: $@ ($$(wc -c < $@) байт) = gzip(ядро) + дерево"

# ЗАЧЕМ ЗДЕСЬ -n. Флаг запрещает gzip записывать в заголовок имя исходного
# файла и время. Сборка ядра Linux ставит его по той же причине, по которой
# он понадобился нам: имя файла удлиняет заголовок на свою длину, а
# распаковщик загрузчика возвращает смещение конца потока — то самое, по
# которому он потом ищет дерево. Разъехались на длину имени — и дерево
# ищется мимо, с исходом "dtb not found" и отказом грузиться.
#
# Проверить легко: у стокового ядра Xiaomi заголовок начинается с
# 1f 8b 08 00, у нашего до этой правки было 1f 8b 08 08 — бит FNAME.

# ---------------------------------------------------------------------------
# Запасной образ: то же ядро, но БЕЗ сжатия.
#
# Загрузчик умеет и такой формат, причём он надёжнее: смещение дерева не
# вычисляется распаковщиком, а написано в образе явно. Признак формата —
# метка "UNCOMPRESSED_IMG" в начале, следом 32-битное смещение дерева от
# начала этой метки, а за 20-байтным заголовком идёт обычный ARM64 Image.
# В коде загрузчика это PATCHED_KERNEL_MAGIC, ветка "Patched kernel
# detected".
#
# Держим оба варианта: если сжатый почему-то не примут, этот проверяет ту
# же систему по пути, где ошибиться просто негде.
$(PACKED_RAW): image $(BOARD_DTB)
	@python3 -c "import struct; \
	k=open('$(RAW_IMAGE)','rb').read(); d=open('$(BOARD_DTB)','rb').read(); \
	open('$@','wb').write(b'UNCOMPRESSED_IMG'+struct.pack('<I',20+len(k))+k+d)"
	@echo "упаковано: $@ ($$(wc -c < $@) байт) = метка + ядро + дерево"

# ---------------------------------------------------------------------------
# Образ методом подмены ядра в чужом boot.img.
#
# Самый надёжный путь на этом телефоне, и вот почему. Загрузчик разборчив к
# тому, как к ядру приделано дерево устройств, и договориться с ним "с
# нуля" оказалось трудно: смещение он вычисляет распаковщиком, а в
# fastboot-пути ветки для несжатого образа у него нет вовсе. Зато рядом
# лежит boot.img, который этот же загрузчик принимает каждый день, — тот,
# с которого телефон загружается. В нём дерево лежит ровно так, как
# загрузчику нравится.
#
# Поэтому мы не собираем образ, а берём рабочий и подменяем в нём ТОЛЬКО
# ядро. Дерево, его смещение, сжатие и все поля заголовка достаются нам
# уже согласованными.
#
# Дерево при этом достаётся от системы телефона — и это не потеря, а
# приобретение: оно описывает то же железо полнее нашего, а загрузчик
# добавляет в него узел готового экрана.
#
#     make mido-swap DONOR=~/mido/boot_lineage.img
#
# DONOR — boot.img, снятый с телефона:
#     adb shell su -c 'dd if=/dev/block/bootdevice/by-name/boot' > boot_lineage.img
mido-swap: image
	@test -n "$(DONOR)" || { echo "укажите DONOR=<путь к рабочему boot.img>"; exit 1; }
	@command -v magiskboot >/dev/null || { echo "magiskboot не найден в PATH"; exit 1; }
	rm -rf $(BUILD)/swap && mkdir -p $(BUILD)/swap
	cp "$(DONOR)" $(BUILD)/swap/donor.img
	cd $(BUILD)/swap && magiskboot unpack donor.img
	cp $(RAW_IMAGE) $(BUILD)/swap/kernel
	cd $(BUILD)/swap && magiskboot repack donor.img $(notdir $(BOOT_IMG_SWAP))
	cp $(BUILD)/swap/$(notdir $(BOOT_IMG_SWAP)) $(BOOT_IMG_SWAP)
	@echo
	@echo "собрано: $(BOOT_IMG_SWAP)"
	@echo "проверка без записи:  fastboot boot $(BOOT_IMG_SWAP)"

bootimg-raw: $(PACKED_RAW)
	python3 tools/mkbootimg.py \
	    --kernel $(PACKED_RAW) \
	    --base $(BOOTIMG_BASE) \
	    --pagesize $(BOOTIMG_PAGESIZE) \
	    --cmdline "$(BOOTIMG_CMDLINE)" \
	    --output $(BOOT_IMG_RAW)

# ============================================================================
#  Запуск в QEMU
# ============================================================================
ifeq ($(ARCH),x86)

run: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF)

run-big: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF) -display sdl

gui-run: $(GUI_ELF)
	$(QEMU) -kernel $(GUI_ELF)

gui-run-big: $(GUI_ELF)
	$(QEMU) -kernel $(GUI_ELF) -display sdl

# На x86 пальцем работает мышь: hal_input_x86.c переводит её относительные
# смещения в те же события "нажали/ведут/отпустили", что приходят с
# тачскрина на ARM64. Удобно для быстрой отладки интерфейса.
touch-run: $(TOUCH_ELF)
	$(QEMU) -kernel $(TOUCH_ELF) -display sdl

iso: $(KERNEL_ELF)
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL_ELF) $(BUILD)/isodir/boot/kernel.elf
	echo 'menuentry "proshivkaOS NEXT" {' >  $(BUILD)/isodir/boot/grub/grub.cfg
	echo '  multiboot /boot/kernel.elf'   >> $(BUILD)/isodir/boot/grub/grub.cfg
	echo '}'                              >> $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

run-iso: iso
	$(QEMU) -cdrom $(ISO)

else

# Текстовый профиль: экрана нет вообще, вся жизнь в последовательном порту.
run: $(KERNEL_ELF)
	$(QEMU) -M virt -cpu cortex-a53 -m 256 -nographic -kernel $(KERNEL_ELF)

# Тач-профиль:
#   -device ramfb                 экран (см. arch/arm64/ramfb.c)
#   -device virtio-tablet-device  тачскрин: абсолютные координаты
#   -device virtio-keyboard-device  клавиатура (экранная тоже есть, эта — для удобства)
#   -serial mon:stdio             отладочный вывод ядра в терминал
touch-run: $(TOUCH_ELF)
	$(QEMU) -M virt -cpu cortex-a53 -m 512 \
	    -kernel $(TOUCH_ELF) \
	    -device ramfb \
	    -device virtio-tablet-device \
	    -device virtio-keyboard-device \
	    -serial mon:stdio

# То же самое, но из сырого Image — проверка, что образ для реального
# железа собран правильно и грузится так же, как ELF.
image-run: image
	$(QEMU) -M virt -cpu cortex-a53 -m 512 \
	    -kernel $(RAW_IMAGE) \
	    -device ramfb \
	    -device virtio-tablet-device \
	    -device virtio-keyboard-device \
	    -serial mon:stdio

endif

# ============================================================================
clean:
	rm -rf build

help:
	@echo "proshivkaOS NEXT — цели сборки"
	@echo ""
	@echo "  make [ARCH=x86|arm64] text     текстовый shell"
	@echo "  make gui                       оконный интерфейс (только x86)"
	@echo "  make [ARCH=...] touch          тач-интерфейс"
	@echo "  make [ARCH=...] run            запустить текстовый профиль в QEMU"
	@echo "  make [ARCH=...] touch-run      запустить тач-интерфейс в QEMU"
	@echo "  make ARCH=arm64 image          сырой ARM64 Image для загрузчика"
	@echo "  make ARCH=arm64 bootimg        Android boot.img для fastboot"
	@echo "  make mido                      образ для Redmi Note 4/4X одной командой"
	@echo "  make clean                     удалить build/"
	@echo ""
	@echo "  Разрешение тач-сборки: make ARCH=arm64 touch SCREEN_W=720 SCREEN_H=1440"
