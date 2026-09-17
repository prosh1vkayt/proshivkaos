/* arch/arm64/pil.c — запуск сопроцессоров через TrustZone (PIL).
 *
 * Процессор Wi-Fi (Pronto) и защищённый режим GPU (zap-шейдер) запускаются
 * одинаково: прошивка подписана, и проверить подпись, положить образ в
 * защищённую память и отпустить процессор из сброса может только доверенная
 * среда. Мы готовим ей всё и просим. Порядок — как в ядре Linux
 * (drivers/soc/qcom/mdt_loader.c и qcom_scm.c):
 *
 *   1. метаданные: ELF-заголовок и хэш-сегмент из файла .mdt кладутся в
 *      память, которую TrustZone читает сама, -> PAS_INIT_IMAGE;
 *   2. если образ перемещаемый — PAS_MEM_SETUP на отведённую ему область;
 *   3. загружаемые сегменты из файлов .bNN — в эту область;
 *   4. PAS_AUTH_AND_RESET — TrustZone проверяет образ и запускает
 *      процессор. С этого мгновения область закрыта: любое наше обращение
 *      туда кончится перезагрузкой. Поэтому до этого вызова мы убираем её
 *      из карты памяти совсем.
 *
 * Подпись TrustZone проверяет на крипто-ядре кристалла — его такты
 * включаются перед вызовами. */
#include "arm64.h"
#include "hal_time.h"
#include "boards/board.h"

const uint8_t *fw_find(const char *name, uint32_t *size);
int64_t scm_call_args(uint32_t svc, uint32_t cmd, uint64_t arginfo,
                      uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *res0);
void mmu_map_ram_window(uint64_t pa, uint64_t size, int on);
int rpm_vote_smpa_level(uint32_t id, uint32_t level);
int rpm_vote_kv(uint32_t res_type, uint32_t res_id, uint32_t key, uint32_t value);

#define SVC_PIL              2
#define PAS_INIT_IMAGE       1
#define PAS_MEM_SETUP        2
#define PAS_AUTH_AND_RESET   5
#define PAS_SHUTDOWN         6
#define ARGS(n)              ((uint64_t)(n))
#define ARGS_VAL_RW          (2u | (2u << 6))       /* два аргумента: значение, буфер */

#define PT_LOAD              1
#define MDT_TYPE_MASK        (7u << 24)
#define MDT_TYPE_HASH        (2u << 24)
#define MDT_RELOCATABLE      (1u << 27)

/* Буфер метаданных — в некэшируемой области обмена с устройствами,
   подальше от буферов USB в её начале. */
#define META_OFFSET          0x100000
#define META_MAX             0x40000

typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} phdr32_t;

static void say(const char *s) { early_con_puts(s); }
static void say_hex(const char *s, uint64_t v) {
    early_con_puts(s); early_con_hex32((uint32_t)v); early_con_puts("\n");
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void copy_bytes(volatile uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i = 0;
    for (; i + 4 <= n && (((uintptr_t)dst + i) & 3) == 0; i += 4)
        *(volatile uint32_t *)(dst + i) = rd32(src + i);
    for (; i < n; i++) dst[i] = src[i];
}

/* Такты крипто-ядра: голоса за три ветви и корень 80 МГц от GPLL0. */
static void crypto_clocks_on(void) {
    uint64_t gcc = BOARD_GCC_BASE;
    mmio_write32(gcc + 0x45004, mmio_read32(gcc + 0x45004) | 7u);
    uint32_t cfg = mmio_read32(gcc + 0x16008);
    uint32_t want = (1u << 8) | 19u;                /* GPLL0, деление 10 */
    if ((cfg & 0x71F) != want) {
        mmio_write32(gcc + 0x16008, (cfg & ~0x71Fu) | want);
        mmio_write32(gcc + 0x16004, mmio_read32(gcc + 0x16004) | 1u);
        for (int i = 0; i < 1000 && (mmio_read32(gcc + 0x16004) & 1u); i++)
            hal_time_delay_us(10);
    }
    dsb_sy();
    say_hex("PIL: takty kripto, golosa ", mmio_read32(gcc + 0x45004));
}

static char g_name[64];

static const uint8_t *fw_part(const char *base, const char *ext, uint32_t *size) {
    int n = 0;
    while (base[n] && n < 50) { g_name[n] = base[n]; n++; }
    g_name[n++] = '.';
    for (int i = 0; ext[i] && n < 63; i++) g_name[n++] = ext[i];
    g_name[n] = 0;
    return fw_find(g_name, size);
}

/* Загрузить и запустить. mem_phys/mem_size — область, отведённая образу
   в дереве устройств. Возвращает 0 при успехе. */
int pil_boot(const char *fw, uint32_t pas_id, uint64_t mem_phys, uint64_t mem_size) {
    uint32_t mdt_size = 0;
    const uint8_t *mdt = fw_part(fw, "mdt", &mdt_size);
    if (!mdt) { say("PIL: net fayla .mdt v obraze\n"); return -1; }
    if (mdt_size < 52 || mdt[0] != 0x7F || mdt[1] != 'E' || mdt[4] != 1) {
        say("PIL: .mdt ne ELF32\n"); return -2;
    }

    uint32_t phoff = rd32(mdt + 28);
    uint16_t phnum = rd16(mdt + 44);
    if (phnum < 2 || phoff + (uint32_t)phnum * 32 > mdt_size) { say("PIL: zagolovki ne v fayle\n"); return -3; }
    const phdr32_t *ph = (const phdr32_t *)(mdt + phoff);

    /* Хэш-сегмент и границы загружаемых. */
    int hash = -1, reloc = 0;
    uint64_t min_addr = ~0ull, max_addr = 0;
    for (int i = 0; i < phnum; i++) {
        if (i > 0 && hash < 0 && (ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) hash = i;
        if (ph[i].p_type != PT_LOAD || (ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH ||
            !ph[i].p_memsz) continue;
        if (ph[i].p_flags & MDT_RELOCATABLE) reloc = 1;
        if (ph[i].p_paddr < min_addr) min_addr = ph[i].p_paddr;
        uint64_t end = ((uint64_t)ph[i].p_paddr + ph[i].p_memsz + 0xFFF) & ~0xFFFull;
        if (end > max_addr) max_addr = end;
    }
    if (hash < 0) { say("PIL: net hesh-segmenta\n"); return -4; }
    if (max_addr - min_addr > mem_size) { say("PIL: obraz bolshe otvedyonnoy oblasti\n"); return -5; }

    /* 1. Метаданные. */
    extern char __dma_start[];
    volatile uint8_t *meta = (volatile uint8_t *)((uintptr_t)__dma_start + META_OFFSET);
    uint32_t ehdr_size = ph[0].p_filesz, hash_size = ph[hash].p_filesz;
    if (ehdr_size + hash_size > META_MAX) { say("PIL: metadannye slishkom veliki\n"); return -6; }
    copy_bytes(meta, mdt, ehdr_size);
    if (ehdr_size + hash_size == mdt_size) {
        copy_bytes(meta + ehdr_size, mdt + ehdr_size, hash_size);
    } else if ((uint64_t)ph[hash].p_offset + hash_size <= mdt_size) {
        copy_bytes(meta + ehdr_size, mdt + ph[hash].p_offset, hash_size);
    } else {
        char ext[4] = { 'b', (char)('0' + hash / 10), (char)('0' + hash % 10), 0 };
        uint32_t sz = 0;
        const uint8_t *seg = fw_part(fw, ext, &sz);
        if (!seg || sz != hash_size) { say("PIL: net fayla hesh-segmenta\n"); return -7; }
        copy_bytes(meta + ehdr_size, seg, hash_size);
    }
    dsb_sy();

    crypto_clocks_on();

    uint64_t res = 0;
    int64_t r = scm_call_args(SVC_PIL, PAS_INIT_IMAGE, ARGS_VAL_RW, pas_id,
                              (uint64_t)(uintptr_t)meta, 0, &res);
    say_hex("PIL: PAS_INIT_IMAGE kod ", (uint64_t)r);
    say_hex("PIL:   rezultat ", res);
    if (r != 0 || res != 0) return -10;

    /* 2. Область. */
    if (reloc) {
        r = scm_call_args(SVC_PIL, PAS_MEM_SETUP, ARGS(3), pas_id, mem_phys,
                          max_addr - min_addr, &res);
        say_hex("PIL: PAS_MEM_SETUP kod ", (uint64_t)r);
        say_hex("PIL:   rezultat ", res);
        if (r != 0 || res != 0) return -11;
    }

    /* 3. Сегменты. */
    mmu_map_ram_window(mem_phys, mem_size, 1);
    uint64_t reloc_base = reloc ? min_addr : mem_phys;
    int loaded = 0;
    for (int i = 0; i < phnum; i++) {
        const phdr32_t *p = &ph[i];
        if (p->p_type != PT_LOAD || (p->p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH ||
            !p->p_memsz) continue;
        uint64_t off = (uint64_t)p->p_paddr - reloc_base;
        if (off + p->p_memsz > mem_size || p->p_filesz > p->p_memsz) {
            say_hex("PIL: segment vne oblasti, nomer ", (uint64_t)i);
            mmu_map_ram_window(mem_phys, mem_size, 0);
            return -12;
        }
        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)(mem_phys + off);
        if (p->p_filesz) {
            const uint8_t *data;
            uint32_t sz = 0;
            if ((uint64_t)p->p_offset + p->p_filesz <= mdt_size) {
                data = mdt + p->p_offset;
                sz = p->p_filesz;
            } else {
                char ext[4] = { 'b', (char)('0' + i / 10), (char)('0' + i % 10), 0 };
                data = fw_part(fw, ext, &sz);
            }
            if (!data || sz != p->p_filesz) {
                say_hex("PIL: net fayla segmenta ", (uint64_t)i);
                mmu_map_ram_window(mem_phys, mem_size, 0);
                return -13;
            }
            copy_bytes(dst, data, p->p_filesz);
        }
        for (uint32_t k = p->p_filesz; k < p->p_memsz; k++) dst[k] = 0;
        loaded++;
    }
    dsb_sy();
    /* Область — вон из карты: после проверки она закрыта для нас. */
    mmu_map_ram_window(mem_phys, mem_size, 0);
    say_hex("PIL: segmentov zagruzheno ", (uint64_t)loaded);

    /* 4. Проверка и запуск. */
    r = scm_call_args(SVC_PIL, PAS_AUTH_AND_RESET, ARGS(1), pas_id, 0, 0, &res);
    say_hex("PIL: PAS_AUTH_AND_RESET kod ", (uint64_t)r);
    say_hex("PIL:   rezultat ", res);
    if (r != 0 || res != 0) return -14;

    say("PIL: TrustZone proverila i zapustila processor\n");
    return 0;
}

/* ---------------- Wi-Fi ---------------- */

/* Список каналов SMD с их краями — появился ли у Wi-Fi свой. */
void pil_list_smd_channels(void) {
    /* Таблиц две: 13 (состояния с 14-го предмета) и 266 (со 138-го). */
    static const int tbl_id[2] = { 13, 266 }, info_base[2] = { 14, 138 };
    int n = 0;
    /* Сначала общее оглавление (там каналы к RPM), затем раздел пары
       «приложения — Wi-Fi» (хост 4). */
    for (int part = 0; part < 2; part++)
    for (int t = 0; t < 2; t++) {
        int host = part ? 4 : -1;
        uint32_t sz = 0;
        uint64_t tbl = smem_item_host(host, tbl_id[t], &sz);
        if (!tbl) { say_hex("PIL: net tablicy kanalov ", (uint64_t)tbl_id[t]); continue; }
        for (int i = 0; i < 64; i++) {
            uint64_t e = tbl + (uint32_t)i * 32;
            if (!mmio_read32(e + 28) || !mmio_read8(e)) continue;
            char name[21];
            for (int k = 0; k < 20; k++) name[k] = (char)mmio_read8(e + (uint32_t)k);
            name[20] = 0;
            uint32_t flags = mmio_read32(e + 24);
            uint32_t cid = mmio_read32(e + 20);
            say("PIL: kanal SMD ");
            say(name);
            say(" kray ");
            early_con_hex32(flags & 0xFF);
            /* Состояния обеих сторон: 2 — открыт, 0 — закрыт. */
            uint32_t isz = 0;
            uint64_t info = smem_item_host(host, info_base[t] + (int)cid, &isz);
            if (info && isz >= 40) {
                uint32_t half = isz >= 88 ? 44 : 20;
                say(" sostoyaniya ");
                early_con_hex32(mmio_read32(info));
                say("/");
                early_con_hex32(mmio_read32(info + half));
            }
            say("\n");
            n++;
        }
    }
    say_hex("PIL: kanalov SMD vsego ", (uint64_t)n);
}

/* Признаки жизни сопроцессоров: общие состояния SMSM (предмет 85: по
   слову на хозяина — приложения, модем, Q6, Wi-Fi...) и строка причины
   последнего сбоя Wi-Fi (предмет 422), которую прошивка пишет, падая. */
void pil_status(void) {
    uint32_t sz = 0;
    uint64_t smsm = smem_item(85, &sz);
    if (smsm) {
        say_hex("PIL: SMSM dlina ", sz);
        for (uint32_t i = 0; i < 8 && (i + 1) * 4 <= sz; i++) {
            say("PIL:  SMSM ");
            early_con_hex32(i);
            say(" = ");
            early_con_hex32(mmio_read32(smsm + 4 * i));
            say("\n");
        }
    } else {
        smem_item_debug(85);
    }
    /* SMP2P от Pronto (предмет 451): его «slave-kernel» — бит 0 фатальная
       ошибка, 1 готов, 2 передача питания, 3 подтверждение останова. */
    uint64_t p2p = smem_item_host(4, 451, &sz);
    if (p2p && sz >= 24) {
        say_hex("PIL: SMP2P Pronto magic ", mmio_read32(p2p));
        say_hex("PIL:   versiya/pid ", mmio_read32(p2p + 4));
        uint32_t valid = mmio_read16(p2p + 14);
        say_hex("PIL:   zapisey ", valid);
        for (uint32_t i = 0; i < valid && i < 16; i++) {
            uint64_t e = p2p + 20 + i * 20;
            char name[17];
            for (int k = 0; k < 16; k++) name[k] = (char)mmio_read8(e + (uint32_t)k);
            name[16] = 0;
            say("PIL:   "); say(name); say(" = ");
            early_con_hex32(mmio_read32(e + 16)); say("\n");
        }
    } else {
        say("PIL: SMP2P ot Pronto net (predmet 451)\n");
        smem_item_debug(451);
    }
    /* Ожидающие прерывания SPI 142..149 (номера GIC 174..181): SMD, SMP2P,
       SMSM Wi-Fi, 145/146 — WLAN TX/RX, 149 — сторожевой таймер Pronto.
       Флаг «ожидает» ставится фронтом и без разрешения прерывания. */
    say_hex("PIL: GIC ozhidayut 160-191 ", mmio_read32(0x0B000000ull + 0x214));
    uint64_t reason = smem_item(422, &sz);
    if (reason && sz) {
        char buf[81];
        uint32_t n = sz < 80 ? sz : 80;
        for (uint32_t i = 0; i < n; i++) {
            char ch = (char)mmio_read8(reason + i);
            buf[i] = (ch >= 32 && ch < 127) ? ch : (ch ? '.' : 0);
        }
        buf[n] = 0;
        say("PIL: prichina sboya Wi-Fi: ");
        say(buf);
        say("\n");
    } else {
        smem_item_debug(422);
    }
}

/* Рукопожатие SMSM со стороны приложений: «инициализированы, SMD готов,
   работаем» — и сигнал процессору Wi-Fi (бит 19 регистра межпроцессорных
   сигналов, qcom,smsm-wcnss в дереве). Без этого сопроцессоры не знают,
   что с ними есть кому говорить. */
#define SMSM_INIT      0x00000001u
#define SMSM_SMDINIT   0x00000008u
#define SMSM_RPCINIT   0x00000020u
#define SMSM_RUN       0x00000100u
#define IPC_SMSM_WCNSS 0x00080000u
#define IPC_SMD_WCNSS  0x00020000u

void pil_smsm_apps_ready(void) {
    uint32_t sz = 0;
    uint64_t smsm = smem_item(85, &sz);
    if (!smsm || sz < 4) { say("PIL: SMSM eshchyo net\n"); return; }
    uint32_t v = mmio_read32(smsm);
    v |= SMSM_INIT | SMSM_SMDINIT | SMSM_RPCINIT | SMSM_RUN;
    mmio_write32(smsm, v);
    dsb_sy();
    mmio_write32(BOARD_SMD_IPC_REG, IPC_SMSM_WCNSS);
    mmio_write32(BOARD_SMD_IPC_REG, IPC_SMD_WCNSS);
    say_hex("PIL: SMSM prilozheniy teper ", mmio_read32(smsm));
}

/* НАША ПОЛОВИНА SMP2P.
 *
 * SMP2P — тридцатидвухбитные «флажки» между парой процессоров. Каждая
 * сторона пишет только в свой предмет SMEM: приложения к Wi-Fi — 431,
 * Wi-Fi к приложениям — 451. Через эти флажки Pronto сообщает «готов»,
 * «упал», а мы просим его остановиться.
 *
 * В Linux драйвер заводит свой предмет при загрузке, задолго до старта
 * Pronto. Прошивка на это рассчитывает: без нашей половины её SSR-модуль
 * ждёт («SMP2P not ready» в строках прошивки), и загрузка дальше не идёт —
 * каналы SMD так и не появляются. Делаем как mainline: заголовок, одна
 * запись «master-kernel», версия — последней, затем сигнал (бит 18). */
#define SMP2P_MAGIC        0x504D5324u
#define SMP2P_ITEM_BYTES   (20u + 16u * 20u)
#define IPC_SMP2P_WCNSS    0x00040000u

void pil_smp2p_out(void) {
    uint64_t out = smem_alloc_host(4, 431, SMP2P_ITEM_BYTES);
    if (!out) { say("PIL: predmet SMP2P 431 ne vydelen\n"); return; }
    if (mmio_read32(out) == SMP2P_MAGIC) { say("PIL: SMP2P 431 uzhe est\n"); return; }
    mmio_write32(out + 4, 0);                   /* версия 0 — ещё не готово */
    mmio_write16(out + 8, 0);                   /* наш номер: приложения    */
    mmio_write16(out + 10, 4);                  /* их номер: Wi-Fi          */
    mmio_write16(out + 12, 16);
    mmio_write16(out + 14, 1);
    mmio_write32(out + 16, 0);
    static const char name[] = "master-kernel";
    for (uint32_t k = 0; k < sizeof(name); k++) mmio_write8(out + 20 + k, (uint8_t)name[k]);
    mmio_write32(out + 36, 0);
    mmio_write32(out, SMP2P_MAGIC);
    dsb_sy();
    mmio_write32(out + 4, 1);                   /* версия 1, без особенностей */
    dsb_sy();
    mmio_write32(BOARD_SMD_IPC_REG, IPC_SMP2P_WCNSS);
    say("PIL: nasha polovina SMP2P (431) zavedena\n");
}

/* Питание радио — как wcnss_wlan_power() в ядре: память и ядро
   кристалла уровнями, выводы 1,8 В, затем радиочасть Iris. */
static void wcnss_power_on(void) {
    rpm_vote_smpa_level(7, 384);                        /* vddmx  */
    rpm_vote_smpa_level(2, 384);                        /* vddcx  */
    rpm_regulator_enable(RPM_RES_LDOA, 5, 1800000);     /* vddpx, vdddig */
    rpm_regulator_enable(RPM_RES_LDOA, 7, 1800000);     /* vddxo, vdd_pronto_pll */
    rpm_regulator_enable(RPM_RES_LDOA, 19, 1300000);    /* vddrfa */
    rpm_regulator_enable(RPM_RES_LDOA, 9, 3300000);     /* vddpa  */
    hal_time_delay_ms(20);
    say("PIL: pitanie radio podano\n");
}

/* ШЕСТЬ ПРОВОДОВ К IRIS И ЕГО КВАРЦ.
 *
 * Pronto — только цифровая половина Wi-Fi. Радио живёт в отдельной
 * микросхеме Iris (WCN36xx), и говорят они по пятипроводной шине на
 * выводах 76–80. Пока выводы остаются обычными GPIO, Pronto стучится в
 * пустоту: прошивка встаёт на опросе радио и до своих каналов SMD так и
 * не доходит. Дерево (wcnss_default) отдаёт их функции 1, 6 мА, подтяжка
 * вверх.
 *
 * Кварц для Iris даёт буфер rf_clk2 сопроцессора питания (ресурс "clka"
 * номер 5), а режим кварца — 19,2 или 48 МГц — прошивка узнаёт из
 * регистра PMU Pronto. Ядро определяет его само, прочитав у Iris
 * идентификатор (qcom,has-autodetect-xo), — так и делаем. */
#define PRONTO_PMU            0x0A21B000ull
#define PMU_CFG               (PRONTO_PMU + 0x1004)
#define PMU_SPARE             (PRONTO_PMU + 0x1088)
#define PMU_IRIS_READ         (PRONTO_PMU + 0x1134)
#define PMU_XO_CFG            (1u << 3)
#define PMU_XO_EN             (1u << 4)
#define PMU_BUS_MUX_TOP       (1u << 5)
#define PMU_XO_CFG_STS        (1u << 6)
#define PMU_IRIS_RESET        (1u << 7)
#define PMU_IRIS_RESET_STS    (1u << 8)
#define PMU_IRIS_READ_BIT     (1u << 9)
#define PMU_IRIS_READ_STS     (1u << 10)
#define PMU_XO_MODE_MASK      (3u << 1)
#define PMU_XO_MODE_48        (3u << 1)
#define SPARE_NVBIN_DLND      (1u << 25)

static int pmu_wait_clear(uint32_t bit) {
    for (int i = 0; i < 2000; i++) {
        if (!(mmio_read32(PMU_CFG) & bit)) return 1;
        hal_time_delay_us(50);
    }
    say_hex("PIL: PMU zavis na bite ", bit);
    return 0;
}

static void iris_reset(uint32_t reg) {
    mmio_write32(PMU_CFG, reg | PMU_IRIS_RESET);
    pmu_wait_clear(PMU_IRIS_RESET_STS);
    mmio_write32(PMU_CFG, reg & ~PMU_IRIS_RESET);
}

static int iris_id_valid(uint32_t v) {
    switch ((v >> 16) & 0xFFFF) {
    case 0x0200: case 0x0300: case 0x0400:      /* WCN3660, 3660A, 3660B/3680 */
    case 0x5111: case 0x5112:                   /* WCN3620, 3620A */
    case 0x9101: case 0x9110:                   /* WCN3610 */
        return 1;
    }
    return 0;
}

static void wcnss_iris_on(void) {
    for (int g = 76; g <= 80; g++) tlmm_gpio_func(g, 1, 6, 3);
    int rf = rpm_vote_kv(0x616B6C63u /* "clka" */, 5, 0x6E657773u /* "swen" */, 1);
    say(rf ? "PIL: vyvody 76-80 otdany Iris, kvarc rf_clk2 vklyuchen\n"
           : "PIL: golos za rf_clk2 NE USHEL\n");
    hal_time_delay_ms(5);

    mmio_write32(PMU_SPARE, mmio_read32(PMU_SPARE) | SPARE_NVBIN_DLND);

    mmio_write32(PMU_CFG, 0);
    uint32_t reg = mmio_read32(PMU_CFG) | PMU_BUS_MUX_TOP | PMU_XO_EN;
    mmio_write32(PMU_CFG, reg);

    uint32_t id = mmio_read32(PMU_IRIS_READ);
    iris_reset(reg);
    int valid = 0;
    for (int i = 0; i < 6; i++) {
        mmio_write32(PMU_IRIS_READ, (id & 0xFFFF) | 0x04);  /* регистр 4: идентификатор */
        reg = mmio_read32(PMU_CFG) | PMU_IRIS_READ_BIT;
        mmio_write32(PMU_CFG, reg);
        pmu_wait_clear(PMU_IRIS_READ_STS);
        id = mmio_read32(PMU_IRIS_READ);
        reg &= ~PMU_IRIS_READ_BIT;
        say_hex("PIL: Iris otvetil ", id);
        if (iris_id_valid(id)) { valid = 1; break; }
        mmio_write32(PMU_CFG, reg);
        iris_reset(reg);
    }

    reg &= ~PMU_XO_MODE_MASK;
    /* Старшие два бита ответа — семейство: 0 — WCN3660/3680 на 48 МГц,
       иначе 19,2. Не ответил — как ядро, берём 48. */
    int xo48 = !valid || (id >> 30) == 0;
    if (xo48) reg |= PMU_XO_MODE_48;
    say(valid ? "PIL: Iris najden, " : "PIL: Iris NE otvechaet, ");
    say(xo48 ? "kvarc 48 MGc\n" : "kvarc 19.2 MGc\n");

    mmio_write32(PMU_CFG, reg);
    iris_reset(reg);
    reg |= PMU_XO_CFG;
    mmio_write32(PMU_CFG, reg);
    pmu_wait_clear(PMU_XO_CFG_STS);
    reg &= ~(PMU_BUS_MUX_TOP | PMU_XO_CFG);
    mmio_write32(PMU_CFG, reg);
    hal_time_delay_ms(20);
    say_hex("PIL: PMU Pronto ", mmio_read32(PMU_CFG));
}

int pil_start_wifi(void) {
    extern void wlan_mark_enabled(void);
    wlan_mark_enabled();
    say("PIL: zapusk processora Wi-Fi (Pronto)\n");
    wcnss_power_on();
    wcnss_iris_on();
    pil_smp2p_out();
    int r = pil_boot("wcnss", 6, 0x8E700000ull, 0x700000ull);
    if (r == 0) {
        extern void wcnss_ctrl_start(void);
        wcnss_ctrl_start();
    }
    return r;
}

int pil_start_gpu_zap(void) {
    say("PIL: zagruzka zap-sheydera GPU\n");
    return pil_boot("a506_zap", 13, 0x8F800000ull, 0x800000ull);
}
