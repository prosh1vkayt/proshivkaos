/* arch/arm64/vectors.S — таблица векторов исключений AArch64.
 *
 * Прямой аналог IDT на x86 (которого у нас пока тоже нет — см.
 * docs/ARM64_PLAN.md п.4). Прерывания на этом этапе замаскированы, всё
 * железо опрашивается в цикле, поэтому обработчиков "по делу" тут нет.
 * Но таблица всё равно нужна: без неё VBAR_EL1 = 0, и ЛЮБАЯ ошибка
 * (разыменование мусора, невыровненный доступ, неизвестная инструкция)
 * уводит процессор исполнять случайные байты по нулевому адресу —
 * отладить такое невозможно. С таблицей вместо этого получаем нормальную
 * панику с указанием причины.
 *
 * Формат таблицы жёстко задан архитектурой: 16 записей по 128 байт,
 * начало выровнено на 2 КиБ. Четыре группы по четыре записи:
 * (Sync, IRQ, FIQ, SError) x (EL1t, EL1h, EL0 AArch64, EL0 AArch32).
 */

.macro VECTOR_ENTRY index
    .align 7                        /* каждая запись ровно 128 байт */
    mov     x0, #\index
    b       _exception_common
.endm

.section ".text", "ax"
.align 11                           /* VBAR_EL1 требует выравнивания 2 КиБ */
.global _vectors
_vectors:
    VECTOR_ENTRY 0                  /* EL1t  Synchronous */
    VECTOR_ENTRY 1                  /* EL1t  IRQ         */
    VECTOR_ENTRY 2                  /* EL1t  FIQ         */
    VECTOR_ENTRY 3                  /* EL1t  SError      */

    VECTOR_ENTRY 4                  /* EL1h  Synchronous */
    VECTOR_ENTRY 5                  /* EL1h  IRQ         */
    VECTOR_ENTRY 6                  /* EL1h  FIQ         */
    VECTOR_ENTRY 7                  /* EL1h  SError      */

    VECTOR_ENTRY 8                  /* EL0 AArch64 Sync  */
    VECTOR_ENTRY 9                  /* EL0 AArch64 IRQ   */
    VECTOR_ENTRY 10                 /* EL0 AArch64 FIQ   */
    VECTOR_ENTRY 11                 /* EL0 AArch64 SError */

    VECTOR_ENTRY 12                 /* EL0 AArch32 Sync  */
    VECTOR_ENTRY 13                 /* EL0 AArch32 IRQ   */
    VECTOR_ENTRY 14                 /* EL0 AArch32 FIQ   */
    VECTOR_ENTRY 15                 /* EL0 AArch32 SError */

_exception_common:
    /* Стек тут может быть уже испорчен, поэтому ничего не сохраняем —
       всё равно уходим в панику и не возвращаемся. Передаём в C номер
       вектора, ESR_EL1 (причина) и FAR_EL1 (адрес, вызвавший сбой). */
    mrs     x1, esr_el1
    mrs     x2, far_el1
    mrs     x3, elr_el1
    b       arm64_exception_handler
