/* arch/arm64/smd_rpm.c — просьба к сопроцессору питания.
 *
 * ЗАЧЕМ. Источник питания тачскрина нам не принадлежит. Мы это выяснили
 * дорогой ценой: попытка включить его прямой записью в микросхему
 * питания роняла процессор мгновенно и без единой жалобы, потому что
 * распоряжается им другое ядро исполнения — сопроцессор питания.
 *
 * Значит, надо не приказывать, а просить. Просьба идёт через общую
 * память по протоколу SMD, и выглядит она так:
 *
 *   заголовок службы   "req"  и длина
 *   заголовок просьбы  номер, состояние, ВИД РЕСУРСА, НОМЕР РЕСУРСА
 *   тело               пары "ключ - значение": напряжение, включение
 *
 * Вид и номер ресурса берутся из дерева устройств телефона: у источника
 * pm8953_l10 записано qcom,resource-name = "ldoa", resource-id = 10.
 *
 * ЧТО ТАКОЕ SMD. Канал связи из двух кольцевых очередей в общей памяти —
 * своей на передачу и своей на приём — плюс небольшая запись состояния,
 * где стороны сообщают друг другу, открыт ли канал и докуда прочитано.
 * Разбудить собеседника отдельно: записью в особый регистр.
 *
 * Канал надо ещё и ОТКРЫТЬ: обе стороны по очереди объявляют состояние
 * "открываю", потом "открыт". Загрузчик этот канал не использует — он
 * разговаривает с тем же сопроцессором другим способом, — так что
 * рукопожатие приходится делать самим.
 */
#include "arm64.h"
#include "boards/board.h"
#include "hal_time.h"

/* Номера предметов в общей памяти. Закреплены, не выбираются. */
#define SMEM_ALLOC_TBL      13
#define SMEM_INFO_BASE      14
#define SMEM_FIFO_BASE      338
#define ALLOC_TBL_ENTRIES   64
#define ALLOC_ENTRY_BYTES   32
#define ALLOC_NAME_BYTES    20

#define CH_FLAG_EDGE_MASK   0xFF
#define CH_FLAG_PACKET      (1u << 9)

/* Состояния канала */
#define SMD_CLOSED          0
#define SMD_OPENING         1
#define SMD_OPENED          2

/* Поля записи состояния канала. Их две разновидности — побайтная и
   пословная, — и какая именно, видно по длине предмета. */
enum { FLD_STATE, FLD_fDSR, FLD_fCTS, FLD_fCD, FLD_fRI,
       FLD_fHEAD, FLD_fTAIL, FLD_fSTATE, FLD_fBLOCK, FLD_TAIL, FLD_HEAD,
       FLD_COUNT };

static const uint8_t g_off_byte[FLD_COUNT] = { 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16 };
static const uint8_t g_off_word[FLD_COUNT] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40 };
static const uint8_t g_wid_byte[FLD_COUNT] = { 4, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4 };

#define INFO_PAIR_BYTE      40
#define INFO_PAIR_WORD      88

#define SMD_PACKET_HDR      20

/* Заголовки просьбы к сопроцессору */
#define RPM_SERVICE_REQUEST 0x00716572u     /* "req"  */
#define RPM_STATE_ACTIVE    0

/* Ключи тела просьбы */
#define RPM_KEY_SWEN        0x6E657773u     /* "swen" — включение       */
#define RPM_KEY_UV          0x00007675u     /* "uv"   — напряжение, мкВ */

static uint64_t g_info = 0;
static uint64_t g_tx_fifo = 0, g_rx_fifo = 0;
static uint32_t g_fifo_size = 0;
static int      g_word = 0;
static int      g_ready = 0;
static uint32_t g_msg_id = 1;

/* ---- Доступ к записи состояния ---- */

static uint64_t fld_addr(int rx, int fld) {
    uint64_t base = g_info + (uint64_t)(rx ? (g_word ? INFO_PAIR_WORD / 2
                                                     : INFO_PAIR_BYTE / 2) : 0);
    return base + (g_word ? g_off_word[fld] : g_off_byte[fld]);
}

static uint32_t fld_get(int rx, int fld) {
    uint64_t a = fld_addr(rx, fld);
    if (g_word || g_wid_byte[fld] == 4) return mmio_read32(a);
    return mmio_read8(a);
}

static void fld_set(int rx, int fld, uint32_t v) {
    uint64_t a = fld_addr(rx, fld);
    if (g_word || g_wid_byte[fld] == 4) mmio_write32(a, v);
    else                                mmio_write8(a, (uint8_t)v);
}

/* Разбудить сопроцессор: он не опрашивает память, он ждёт сигнала. */
static void smd_signal(void) {
    dsb_sy();
    mmio_write32(BOARD_SMD_IPC_REG, BOARD_SMD_RPM_IPC_BIT);
    dsb_sy();
}

/* ---- Поиск канала ---- */

/* Имя в списке занимает ровно двадцать байт и дополнено нулями. Дойдя
   до нуля в обеих строках сразу, считаем, что совпало; двадцать
   совпавших байт без нуля — тоже совпадение, поле просто заполнено
   целиком. */
static int name_matches(uint64_t entry, const char *want) {
    for (int i = 0; i < ALLOC_NAME_BYTES; i++) {
        uint8_t c = mmio_read8(entry + (uint32_t)i);
        if (c != (uint8_t)want[i]) return 0;
        if (!c) return 1;
    }
    return 1;
}

static int find_channel(const char *name, int edge) {
    uint32_t tbl_size = 0;
    uint64_t tbl = smem_item(SMEM_ALLOC_TBL, &tbl_size);
    if (!tbl) { early_con_puts("SMD: net spiska kanalov\n"); return -1; }

    for (int i = 0; i < ALLOC_TBL_ENTRIES; i++) {
        uint64_t e = tbl + (uint32_t)i * ALLOC_ENTRY_BYTES;

        uint32_t ref = mmio_read32(e + 28);
        if (!ref) continue;
        if (!mmio_read8(e)) continue;

        uint32_t flags = mmio_read32(e + 24);
        if (!(flags & CH_FLAG_PACKET)) continue;
        if ((int)(flags & CH_FLAG_EDGE_MASK) != edge) continue;

        if (!name_matches(e, name)) continue;

        return (int)mmio_read32(e + 20);        /* номер канала */
    }
    return -1;
}

/* ---- Подъём канала ---- */

static int wait_remote(uint32_t want_a, uint32_t want_b) {
    for (int i = 0; i < 2000; i++) {
        uint32_t st = fld_get(1, FLD_STATE);
        if (st == want_a || st == want_b) return 1;
        hal_time_delay_us(500);
    }
    return 0;
}

static void set_local_state(uint32_t state) {
    int open = (state == SMD_OPENED);
    fld_set(0, FLD_fDSR, open);
    fld_set(0, FLD_fCTS, open);
    fld_set(0, FLD_fCD,  open);
    fld_set(0, FLD_STATE, state);
    fld_set(0, FLD_fSTATE, 1);
    smd_signal();
}

int smd_rpm_init(void) {
    g_ready = 0;

    if (!smem_init()) return 0;

    int cid = find_channel("rpm_requests", BOARD_SMD_RPM_EDGE);
    if (cid < 0) {
        early_con_puts("SMD: kanal rpm_requests ne nayden\n");
        return 0;
    }

    uint32_t info_size = 0, fifo_size = 0;
    g_info = smem_item(SMEM_INFO_BASE + cid, &info_size);
    uint64_t fifo = smem_item(SMEM_FIFO_BASE + cid, &fifo_size);

    if (!g_info || !fifo) {
        early_con_puts("SMD: net sostoyaniya ili ocheredey, kanal ");
        early_con_hex32((uint32_t)cid);
        early_con_puts("\n");
        smem_item_debug(SMEM_ALLOC_TBL);
        smem_item_debug(SMEM_INFO_BASE + cid);
        smem_item_debug(SMEM_FIFO_BASE + cid);
        return 0;
    }

    if (info_size == INFO_PAIR_WORD)      g_word = 1;
    else if (info_size == INFO_PAIR_BYTE) g_word = 0;
    else {
        early_con_puts("SMD: neponyatnaya dlina sostoyaniya ");
        early_con_hex32(info_size);
        early_con_puts("\n");
        return 0;
    }

    g_fifo_size = fifo_size / 2;
    g_tx_fifo = fifo;
    g_rx_fifo = fifo + g_fifo_size;

    early_con_puts("SMD: kanal ");
    early_con_hex32((uint32_t)cid);
    early_con_puts(g_word ? " poslovnyy" : " pobaytnyy");
    early_con_puts(" ochered ");
    early_con_hex32(g_fifo_size);
    early_con_puts(" nashe ");
    early_con_hex32(fld_get(0, FLD_STATE));
    early_con_puts(" ih ");
    early_con_hex32(fld_get(1, FLD_STATE));
    early_con_puts("\n");

    /* Рукопожатие. Если канал уже открыт с обеих сторон — не трогаем:
       переоткрытие сбросило бы очереди под ногами у того, кто их уже
       использует. */
    if (fld_get(0, FLD_STATE) != SMD_OPENED || fld_get(1, FLD_STATE) != SMD_OPENED) {
        early_con_puts("SMD: sbrasyvayu svoyu storonu\n");
        fld_set(0, FLD_STATE, SMD_CLOSED);
        fld_set(0, FLD_fDSR, 0); fld_set(0, FLD_fCTS, 0);
        fld_set(0, FLD_fCD, 0);  fld_set(0, FLD_fRI, 0);
        fld_set(0, FLD_fHEAD, 0); fld_set(0, FLD_fTAIL, 0);
        fld_set(0, FLD_fSTATE, 1); fld_set(0, FLD_fBLOCK, 1);
        fld_set(0, FLD_HEAD, 0);
        fld_set(1, FLD_TAIL, 0);
        smd_signal();

        early_con_puts("SMD: obyavlyayu otkrytie\n");
        set_local_state(SMD_OPENING);
        early_con_puts("SMD: zhdu otveta\n");
        if (!wait_remote(SMD_OPENING, SMD_OPENED)) {
            early_con_puts("SMD: soprocessor ne otkryl kanal (ego sostoyanie ");
            early_con_hex32(fld_get(1, FLD_STATE));
            early_con_puts(")\n");
            return 0;
        }

        early_con_puts("SMD: obyavlyayu otkryt\n");
        set_local_state(SMD_OPENED);
        if (!wait_remote(SMD_OPENED, SMD_OPENED)) {
            early_con_puts("SMD: kanal ne doshyol do otkrytogo\n");
            return 0;
        }
    }

    early_con_puts("SMD: kanal k soprocessoru pitaniya otkryt\n");
    g_ready = 1;
    return 1;
}

/* ---- Передача ---- */

static uint32_t tx_avail(void) {
    uint32_t mask = g_fifo_size - 1;
    uint32_t head = fld_get(0, FLD_HEAD);
    uint32_t tail = fld_get(0, FLD_TAIL);
    return mask - ((head - tail) & mask);
}

static void fifo_write(const uint8_t *data, uint32_t count) {
    uint32_t head = fld_get(0, FLD_HEAD);
    for (uint32_t i = 0; i < count; i++) {
        mmio_write8(g_tx_fifo + ((head + i) & (g_fifo_size - 1)), data[i]);
    }
    head = (head + count) & (g_fifo_size - 1);
    fld_set(0, FLD_HEAD, head);
}

static int smd_send(const uint8_t *data, uint32_t len) {
    uint32_t total = SMD_PACKET_HDR + len;
    if (total >= g_fifo_size) return 0;

    for (int i = 0; i < 2000 && tx_avail() < total; i++) hal_time_delay_us(500);
    if (tx_avail() < total) { early_con_puts("SMD: ochered ne osvobodilas\n"); return 0; }

    uint8_t hdr[SMD_PACKET_HDR];
    for (int i = 0; i < SMD_PACKET_HDR; i++) hdr[i] = 0;
    hdr[0] = (uint8_t)(len & 0xFF);
    hdr[1] = (uint8_t)((len >> 8) & 0xFF);
    hdr[2] = (uint8_t)((len >> 16) & 0xFF);
    hdr[3] = (uint8_t)((len >> 24) & 0xFF);

    fld_set(0, FLD_fTAIL, 0);
    fifo_write(hdr, SMD_PACKET_HDR);
    fifo_write(data, len);
    fld_set(0, FLD_fHEAD, 1);

    smd_signal();
    return 1;
}

/* Сложить 32-битное значение в буфер младшим байтом вперёд. */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Попросить сопроцессор включить источник питания.
 *
 * res_type — четырёхбуквенный вид ресурса ("ldoa"), res_id — его номер,
 * uv — напряжение в микровольтах (0 — не задавать). */
int rpm_regulator_enable(uint32_t res_type, uint32_t res_id, uint32_t uv) {
    if (!g_ready) return 0;

    uint8_t msg[8 + 20 + 24];
    int n = 0;

    int kvps = uv ? 2 : 1;
    uint32_t body = 20 + (uint32_t)kvps * 12;

    put32(msg + n, RPM_SERVICE_REQUEST); n += 4;   /* какая служба     */
    put32(msg + n, body);                n += 4;   /* длина остального */

    put32(msg + n, g_msg_id++);          n += 4;   /* номер просьбы    */
    put32(msg + n, RPM_STATE_ACTIVE);    n += 4;   /* для рабочего хода */
    put32(msg + n, res_type);            n += 4;   /* вид ресурса      */
    put32(msg + n, res_id);              n += 4;   /* его номер        */
    put32(msg + n, (uint32_t)kvps * 12); n += 4;   /* длина тела       */

    if (uv) {
        put32(msg + n, RPM_KEY_UV); n += 4;
        put32(msg + n, 4);          n += 4;
        put32(msg + n, uv);         n += 4;
    }
    put32(msg + n, RPM_KEY_SWEN); n += 4;
    put32(msg + n, 4);            n += 4;
    put32(msg + n, 1);            n += 4;

    early_con_puts("RPM: prosim vklyuchit ");
    early_con_hex32(res_type);
    early_con_puts(" nomer ");
    early_con_hex32(res_id);
    early_con_puts("\n");

    return smd_send(msg, (uint32_t)n);
}
