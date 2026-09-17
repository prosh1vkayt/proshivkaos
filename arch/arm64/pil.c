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
    uint32_t sz = 0;
    uint64_t tbl = smem_item(13, &sz);
    if (!tbl) { say("PIL: spiska kanalov SMD net\n"); return; }
    int n = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t e = tbl + (uint32_t)i * 32;
        if (!mmio_read32(e + 28) || !mmio_read8(e)) continue;
        char name[21];
        for (int k = 0; k < 20; k++) name[k] = (char)mmio_read8(e + (uint32_t)k);
        name[20] = 0;
        uint32_t flags = mmio_read32(e + 24);
        say("PIL: kanal SMD ");
        say(name);
        say(" kray ");
        early_con_hex32(flags & 0xFF);
        say("\n");
        n++;
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

int pil_start_wifi(void) {
    say("PIL: zapusk processora Wi-Fi (Pronto)\n");
    wcnss_power_on();
    return pil_boot("wcnss", 6, 0x8E700000ull, 0x700000ull);
}

int pil_start_gpu_zap(void) {
    say("PIL: zagruzka zap-sheydera GPU\n");
    return pil_boot("a506_zap", 13, 0x8F800000ull, 0x800000ull);
}
