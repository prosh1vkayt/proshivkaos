/* arch/arm64/boot.S — точка входа ядра на AArch64 (GNU as, не NASM:
 * NASM физически не умеет ARM, поэтому x86-шный boot/boot.asm тут не
 * переиспользуется — это его прямой аналог, см. docs/ARM64_PLAN.md п.1).
 *
 * Кто нас грузит:
 *   - QEMU  -M virt -kernel ...   (кладёт образ по RAM_BASE + text_offset)
 *   - LK / ABL / U-Boot на реальном телефоне из boot.img
 * Все трое понимают один и тот же формат — "ARM64 Linux Image", т.е.
 * сырой бинарник с 64-байтным заголовком в самом начале. Поэтому первая
 * инструкция обязана быть валидной веткой (загрузчик может просто прыгнуть
 * на начало образа, не разбирая заголовок), а сразу за ней идут поля
 * заголовка — их загрузчик читает как данные.
 *
 * Контракт ARM64 Boot Protocol на входе:
 *   x0 = физический адрес FDT (device tree blob)
 *   x1..x3 = 0 (зарезервированы)
 *   MMU выключен, D-кэш выключен, прерывания замаскированы,
 *   EL — обычно EL2 (QEMU virt с virtualization=on, реальные телефоны)
 *   или EL1 (QEMU virt по умолчанию). Обрабатываем оба случая.
 */

.section ".text.boot", "ax"
.global _start

_start:
    /* ---------------- ARM64 Linux Image header (64 байта) ------------- */
    b       _boot                   /* code0: исполняемая ветка          */
    .long   0                       /* code1: зарезервировано            */
    .quad   0x80000                 /* text_offset: смещение от базы RAM */
    .quad   __image_size            /* image_size: полный размер образа  */
    .quad   0xA                     /* flags: LE + страницы 4 КиБ        */
    .quad   0                       /* res2                              */
    .quad   0                       /* res3                              */
    .quad   0                       /* res4                              */
    .long   0x644d5241              /* magic "ARM\x64"                   */
    .long   0                       /* res5: смещение PE/COFF-заголовка  */

_boot:
    /* x0 = DTB. Дальше x0 НЕ трогаем до самого вызова kmain. */

    /* Многоядерность: на этом этапе планировщика нет, поэтому все ядра
       кроме нулевого просто паркуем в wfi. MPIDR_EL1[7:0] = Aff0. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbnz    x1, _park

    /* ---------------- EL2 -> EL1 (если стартовали в EL2) -------------- */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    _at_el1

    mov     x2, #1
    lsl     x2, x2, #31             /* HCR_EL2.RW = 1: EL1 работает в    */
    msr     hcr_el2, x2             /* AArch64, а не в AArch32           */

    mrs     x2, cnthctl_el2         /* EL1PCTEN|EL1PCEN: разрешить EL1   */
    orr     x2, x2, #3              /* читать системный счётчик — на нём */
    msr     cnthctl_el2, x2         /* держится весь hal_time_ms()       */
    msr     cntvoff_el2, xzr

    mov     x2, #0x0800             /* SCTLR_EL1 в состояние "после      */
    movk    x2, #0x30d0, lsl #16    /* сброса": MMU и кэши выключены     */
    msr     sctlr_el1, x2

    mov     x2, #0x3c5              /* SPSR_EL2 = EL1h, DAIF замаскирован */
    msr     spsr_el2, x2
    adr     x2, _at_el1
    msr     elr_el2, x2
    eret                            /* спуск в EL1, продолжаем с _at_el1 */

_at_el1:
    /* ---------------- Стек ------------------------------------------- */
    /* Аналог "mov esp, stack_top" из x86-шного boot.asm. adrp+add вместо
       ldr =symbol: не нужен литерал-пул и образ остаётся позиционно
       корректным при загрузке по адресу из линкер-скрипта. */
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1

    /* ---------------- Обнуление .bss ---------------------------------- */
    /* На x86 это делал GRUB (Multiboot гарантирует обнулённый .bss).
       Здесь никто за нас этого не делает: загрузчик кладёт в память ровно
       столько байт, сколько занимает файл образа, а .bss в файле не
       хранится вообще. Без обнуления hal_mem, RamFS и вся статика ядра
       стартуют с мусором. */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:

    /* ---------------- Таблица векторов исключений --------------------- */
    adrp    x1, _vectors
    add     x1, x1, :lo12:_vectors
    msr     vbar_el1, x1
    isb

    /* ---------------- kmain(magic, boot_info) ------------------------- */
    /* Сигнатура kmain() общая с x86, менять её не нужно: там, где на x86
       лежит указатель на multiboot_info, здесь лежит указатель на DTB.
       magic передаём нулевой — ядро его и так игнорирует. */
    mov     x1, x0                  /* arg1 = DTB   */
    mov     x0, #0                  /* arg0 = magic */
    bl      kmain

_park:
    wfi
    b       _park
