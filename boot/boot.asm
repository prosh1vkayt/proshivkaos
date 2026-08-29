; boot/boot.asm
; proshivkaOS NEXT — x86 Multiboot entry point (NASM syntax)
; Собирается GRUB'ом как обычный Multiboot-совместимый kernel.elf

bits 32

; ---- Multiboot header (спецификация Multiboot 1) ----
MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x0            ; без доп. флагов (align/meminfo не запрашиваем на 1-м этапе)
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

; ---- Стек ядра ----
section .bss
align 16
stack_bottom:
    resb 16384                  ; 16 KiB стека — достаточно для первого этапа
stack_top:

; ---- Точка входа ----
section .text
global _start
extern kmain                    ; определена в kernel/kernel.c

_start:
    cli                         ; прерывания выключены, IDT ещё не настроен
    mov esp, stack_top          ; поднимаем стек

    push ebx                    ; ebx = указатель на multiboot_info (пригодится на след. этапе)
    push eax                    ; eax = magic 0x2BADB002 (можно проверить в kmain)

    call kmain

.hang:
    cli
    hlt
    jmp .hang
