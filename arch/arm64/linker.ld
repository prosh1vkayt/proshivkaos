/* arch/arm64/linker.ld — размещение ядра на AArch64.
 *
 * Отличие от x86-шного linker.ld: там адрес 1 MiB — конвенция Multiboot.
 * Здесь 0x40080000 — это база ОЗУ QEMU virt (0x40000000) плюс text_offset
 * из заголовка Image (0x80000). Ровно туда же кладут образ LK/ABL на
 * реальных Qualcomm-устройствах (там база другая, но и она берётся из
 * заголовка Android boot.img — --base, см. Makefile цель bootimg).
 *
 * Секция .text.boot вынесена вперёд принудительно: заголовок ARM64 Image
 * обязан лежать в первых 64 байтах образа, иначе загрузчик его не найдёт.
 */
ENTRY(_start)

RAM_BASE    = 0x40000000;
TEXT_OFFSET = 0x00080000;
STACK_SIZE  = 0x00010000;   /* 64 KiB — против 16 KiB на x86: тут глубже
                                стек у отрисовки GUI и парсера fw_cfg */

SECTIONS
{
    . = RAM_BASE + TEXT_OFFSET;
    __image_start = .;

    .text : ALIGN(4K) {
        KEEP(*(.text.boot))     /* заголовок Image + точка входа — строго первыми */
        *(.text*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
    }

    /* Границы .bss выровнены на 8: boot.S обнуляет её инструкцией
       "str xzr, [x1], #8" — по 8 байт за раз. */
    . = ALIGN(8);
    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss*)
    }
    . = ALIGN(8);
    __bss_end = .;

    /* Стек ядра. Лежит за .bss и НЕ обнуляется (незачем) — но обязан
       попасть в __image_size, иначе загрузчик посчитает эту память
       свободной и может положить туда ramdisk/DTB прямо нам под ноги. */
    . = ALIGN(16);
    . = . + STACK_SIZE;
    __stack_top = .;

    __image_end = .;
    __image_size = __image_end - __image_start;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
