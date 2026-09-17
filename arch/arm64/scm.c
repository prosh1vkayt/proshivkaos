/* arch/arm64/scm.c — разговор с TrustZone.
 *
 * ЗАЧЕМ. Радио Wi-Fi на этом телефоне — отдельный процессор внутри
 * кристалла (WCNSS, он же Pronto). Его прошивка подписана, и положить её в
 * память и отпустить процессор из сброса может только доверенная среда:
 * в дереве устройств у него compatible = "qcom,pil-tz-generic", pas-id 6.
 * Значит, первый шаг к Wi-Fi — не драйвер радио, а умение попросить
 * TrustZone. Этот файл и есть такое умение.
 *
 * КАК. Вызов SMC по соглашению ARMv8, которым пользуется ядро msm-4.9
 * (drivers/soc/qcom/scm.c, scm_call2):
 *
 *   x0  номер функции: 0x02000000 | служба << 8 | команда, плюс разряд 30
 *       (соглашение SMC64)
 *   x1  описание аргументов: младшие четыре разряда — их число
 *   x2..x5  сами аргументы
 *   x6  номер сессии, у нас всегда ноль
 *
 * Ответ: x0 — код (ноль — успех), x1..x3 — результаты. Код 1 означает
 * «прервано, повторите» — тогда тот же вызов повторяется.
 */
#include "arm64.h"

#define SCM_SIP_FNID(svc, cmd)  (0x02000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SMC64               0x40000000u
#define SCM_ARGS(n)             ((uint64_t)(n) & 0xF)
#define SCM_INTERRUPTED         1

#define SCM_SVC_PIL             0x02
#define PAS_IS_SUPPORTED_CMD    0x07
#define SCM_SVC_INFO            0x06
#define IS_CALL_AVAIL_CMD       0x01
#define TZ_INFO_GET_VERSION     0x03

#define PAS_ID_WCNSS            6       /* qcom,pas-id узла pronto@a21b000 */

/* Сам вызов — на ассемблере: SMC вправе испортить x4..x17, и доверять
   компилятору, что он ничего там не держит, нельзя. Результаты x1..x3
   складываются по указателю из седьмого аргумента. */
uint64_t scm_smc_raw(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
                     uint64_t x4, uint64_t x5, uint64_t *out3);
__asm__(
    ".text\n"
    ".global scm_smc_raw\n"
    ".type scm_smc_raw, %function\n"
    "scm_smc_raw:\n"
    "    stp x29, x30, [sp, #-32]!\n"
    "    mov x29, sp\n"
    "    str x6, [sp, #16]\n"
    "    mov x6, #0\n"
    "    smc #0\n"
    "    ldr x9, [sp, #16]\n"
    "    stp x1, x2, [x9]\n"
    "    str x3, [x9, #16]\n"
    "    ldp x29, x30, [sp], #32\n"
    "    ret\n"
);

/* Вызов с явным описанием аргументов (типы — по QCOM_SCM_ARGS ядра):
   x2..x4 — первые три аргумента. */
int64_t scm_call_args(uint32_t svc, uint32_t cmd, uint64_t arginfo,
                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *res0) {
    uint64_t out[3] = { 0, 0, 0 };
    uint64_t fn = SCM_SIP_FNID(svc, cmd) | SCM_SMC64;
    int64_t r;
    int tries = 0;
    do {
        r = (int64_t)scm_smc_raw(fn, arginfo, a0, a1, a2, 0, out);
    } while (r == SCM_INTERRUPTED && ++tries < 64);
    if (res0) *res0 = out[0];
    return r;
}

static int64_t scm_call(uint32_t svc, uint32_t cmd, int nargs,
                        uint64_t a0, uint64_t a1, uint64_t *res) {
    uint64_t out[3] = { 0, 0, 0 };
    uint64_t fn = SCM_SIP_FNID(svc, cmd) | SCM_SMC64;
    int64_t r;
    int tries = 0;
    do {
        r = (int64_t)scm_smc_raw(fn, SCM_ARGS(nargs), a0, a1, 0, 0, out);
    } while (r == SCM_INTERRUPTED && ++tries < 32);
    if (res) *res = out[0];
    return r;
}

static void say_call(const char *what, int64_t r, uint64_t v) {
    early_con_puts("TZ: ");
    early_con_puts(what);
    early_con_puts(" -> kod ");
    early_con_hex32((uint32_t)r);
    early_con_puts(" otvet ");
    early_con_hex32((uint32_t)v);
    early_con_puts("\n");
}

/* Первое знакомство: отвечает ли TrustZone вообще, какой она версии и
   готова ли запускать процессор Wi-Fi. Ничего не меняет — только спрашивает. */
void scm_probe_wifi(void) {
    uint64_t v = 0;
    int64_t r;

    r = scm_call(SCM_SVC_INFO, IS_CALL_AVAIL_CMD, 1,
                 SCM_SIP_FNID(SCM_SVC_INFO, TZ_INFO_GET_VERSION), 0, &v);
    say_call("est li vyzov versii", r, v);

    r = scm_call(SCM_SVC_INFO, TZ_INFO_GET_VERSION, 0, 0, 0, &v);
    say_call("versiya TrustZone", r, v);

    r = scm_call(SCM_SVC_INFO, IS_CALL_AVAIL_CMD, 1,
                 SCM_SIP_FNID(SCM_SVC_PIL, PAS_IS_SUPPORTED_CMD), 0, &v);
    say_call("est li proverka zapuska podsistem", r, v);

    r = scm_call(SCM_SVC_PIL, PAS_IS_SUPPORTED_CMD, 1, PAS_ID_WCNSS, 0, &v);
    say_call("zapusk processora Wi-Fi (pas-id 6) podderzhan", r, v);

    early_con_puts(r == 0 && v == 1
        ? "TZ: TrustZone gotova zapustit processor Wi-Fi — nuzhna tolko ego proshivka\n"
        : "TZ: TrustZone ne podtverdila zapusk processora Wi-Fi\n");
}
