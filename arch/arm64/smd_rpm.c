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

#ifdef CONFIG_RPM_HANDSHAKE
/* Отметка шага с паузой.
 *
 * Пауза миллисекундами — единственная, что прокручивает провод, поэтому
 * именно она и делает отметки видимыми на компьютере вовремя. */
static void step(const char *what) {
    early_con_puts("SMD shag ");
    early_con_puts(what);
    early_con_puts("\n");
    hal_time_delay_ms(200);
}

static void report_states(void) {
    early_con_puts("SMD: nashe ");
    early_con_hex32(fld_get(0, FLD_STATE));
    early_con_puts(" ih ");
    early_con_hex32(fld_get(1, FLD_STATE));
    early_con_puts("\n");
}

/* Ждём ответа сопроцессора.
 *
 * Ожидание идёт миллисекундами, а не микросекундами, и это важно: только
 * миллисекундная задержка прокручивает провод (см. hal_background_poll).
 * Микросекундная молчит, и всё это время журнал не уходит на компьютер —
 * то есть именно тогда, когда он нужнее всего. */
static int wait_remote(uint32_t want_a, uint32_t want_b) {
    for (int i = 0; i < 2000; i++) {
        uint32_t st = fld_get(1, FLD_STATE);
        if (st == want_a || st == want_b) return 1;
        hal_time_delay_ms(1);
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
#endif

int smd_rpm_init(void) {
    g_ready = 0;

    if (!smem_init()) return 0;

    early_con_puts("SMD: ishchu kanal\n");
    int cid = find_channel("rpm_requests", BOARD_SMD_RPM_EDGE);
    if (cid < 0) {
        early_con_puts("SMD: kanal rpm_requests ne nayden\n");
        return 0;
    }

    early_con_puts("SMD: kanal nayden, beru sostoyanie i ocheredi\n");
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

    /* СНАЧАЛА ТОЛЬКО СМОТРИМ.
     *
     * Рукопожатие дважды подвесило аппарат, и оба раза узнать почему
     * было нечем. Поэтому оно теперь отдельно и по умолчанию выключено:
     * сборка без CONFIG_RPM_HANDSHAKE доходит до этого места, печатает
     * всё, что видит, и идёт дальше живой.
     *
     * Это тот же порядок, который уже дважды окупился за этот вечер:
     * прочитать, посмотреть, и только потом писать. Оба раза, когда я
     * его нарушал, приходилось звать человека с кнопкой питания. */
#ifndef CONFIG_RPM_HANDSHAKE
    early_con_puts("SMD: rukopozhatie vyklyucheno v etoy sborke\n");
    return 0;
#else
    /* РУКОПОЖАТИЕ ПО ШАГАМ.
     *
     * Оно дважды подвешивало аппарат наглухо, и оба раза непонятно было
     * даже, на чём именно. Поэтому теперь перед каждой записью пишется
     * отметка, а после — пауза. Пауза здесь не вежливость: только она
     * прокручивает провод, и значит каждая отметка успевает уйти на
     * компьютер до того, как случится следующая запись.
     *
     * Первая же НЕдошедшая отметка называет виновника поимённо. */
    step("1: svoyo sostoyanie -> zakryto");
    fld_set(0, FLD_STATE, SMD_CLOSED);

    step("2: sbros priznakov");
    fld_set(0, FLD_fDSR, 0); fld_set(0, FLD_fCTS, 0);
    fld_set(0, FLD_fCD, 0);  fld_set(0, FLD_fRI, 0);
    fld_set(0, FLD_fHEAD, 0); fld_set(0, FLD_fTAIL, 0);
    fld_set(0, FLD_fSTATE, 1); fld_set(0, FLD_fBLOCK, 1);

    step("3: obnulenie ukazateley ocheredey");
    fld_set(0, FLD_HEAD, 0);
    fld_set(1, FLD_TAIL, 0);

    step("4: budim soprocessor");
    smd_signal();

    step("5: zhdu, poka on zakroet svoyu storonu");
    wait_remote(SMD_CLOSED, SMD_CLOSED);
    report_states();

    step("6: obyavlyayu otkrytie");
    set_local_state(SMD_OPENING);

    step("7: zhdu vstrechnogo otkrytiya");
    if (!wait_remote(SMD_OPENING, SMD_OPENED)) {
        report_states();
        early_con_puts("SMD: soprocessor ne otkryl kanal\n");
        return 0;
    }

    step("8: obyavlyayu otkryt");
    set_local_state(SMD_OPENED);

    step("9: zhdu podtverzhdeniya");
    if (!wait_remote(SMD_OPENED, SMD_OPENED)) {
        report_states();
        early_con_puts("SMD: kanal ne doshyol do otkrytogo\n");
        return 0;
    }

    early_con_puts("SMD: kanal k soprocessoru pitaniya otkryt\n");
    g_ready = 1;
    return 1;
#endif
}

/* ---- Передача ---- */

static uint32_t tx_avail(void) {
    uint32_t mask = g_fifo_size - 1;
    uint32_t head = fld_get(0, FLD_HEAD);
    uint32_t tail = fld_get(0, FLD_TAIL);
    return mask - ((head - tail) & mask);
}

/* Положить слова в кольцевую очередь передачи.
 *
 * ТОЛЬКО СЛОВАМИ, И ЭТО НЕ ПРИДИРКА. У канала две разновидности —
 * побайтная и пословная, — и наш пословный. Такая очередь лежит во
 * встроенной памяти, которая байтовых обращений не принимает вовсе:
 * первая же попытка записать в неё байт закончилась внешним отказом
 * шины (SError) прямо посреди отправки просьбы.
 *
 * Длины у нас все кратны четырём — и заголовок посылки, и тело
 * просьбы, — так что делить нечего. */
static void fifo_write(const uint32_t *words, uint32_t n) {
    uint32_t head = fld_get(0, FLD_HEAD);
    uint32_t mask = g_fifo_size - 1;

    for (uint32_t i = 0; i < n; i++) {
        mmio_write32(g_tx_fifo + ((head + i * 4) & mask), words[i]);
    }

    head = (head + n * 4) & mask;
    fld_set(0, FLD_HEAD, head);
}

static int smd_send(const uint32_t *body, uint32_t n_words) {
    uint32_t len = n_words * 4;
    uint32_t total = SMD_PACKET_HDR + len;
    if (total >= g_fifo_size) return 0;

    for (int i = 0; i < 2000 && tx_avail() < total; i++) hal_time_delay_ms(1);
    if (tx_avail() < total) { early_con_puts("SMD: ochered ne osvobodilas\n"); return 0; }

    /* Заголовок посылки — пять слов, из которых значимо только первое. */
    uint32_t hdr[SMD_PACKET_HDR / 4] = { len, 0, 0, 0, 0 };

    /* Отправка тоже разбита отметками с паузами.
     *
     * Отказ шины здесь приходит АСИНХРОННО: команда, на которой его
     * поймали, оказывается арифметической, а виновата запись, случившаяся
     * раньше. По одному адресу такое не разобрать — зато разбирается
     * паузами: отказ успевает всплыть внутри паузы, и последняя дошедшая
     * отметка называет виноватого. */
    step("A: pered zapisyu v ochered");
    fld_set(0, FLD_fTAIL, 0);

    step("B: pishu zagolovok posylki");
    fifo_write(hdr, SMD_PACKET_HDR / 4);

    step("C: pishu telo prosby");
    fifo_write(body, n_words);

    step("D: podnimayu priznak");
    fld_set(0, FLD_fHEAD, 1);

    step("E: budim soprocessor");
    smd_signal();

    step("F: otpravleno");
    return 1;
}

/* ВЫЧИТАТЬ ОТВЕТЫ СОПРОЦЕССОРА. ОБЯЗАТЕЛЬНО.
 *
 * На каждую просьбу сопроцессор питания отвечает, и ответ ложится в
 * очередь приёма, которую надо освобождать. Мы этого не делали: канал
 * открыли, просьбу послали, а дальше не читали ничего.
 *
 * Кончалось это тем, что аппарат молча перезагружался через десять-
 * двадцать секунд после выхода на рабочий стол. Ни сообщения, ни следа
 * в журнале — и я успел обвинить в этом и отрисовку, и сторожевой
 * таймер, и частоту главного цикла. А сопроцессор питания просто не
 * устройство, у которого можно переполнить очередь и получить ошибку:
 * он распоряжается питанием всего кристалла, и собеседника, который
 * перестал его слушать, перезагружает.
 *
 * Содержимое ответов нам не нужно: просьба либо выполнена, что видно по
 * состоянию источника, либо нет. Важно освободить очередь и сказать об
 * этом — поэтому читаем длину, сдвигаем хвост и будим. */
#define RPM_POLL_INTERVAL_MS 20

void smd_rpm_poll(void) {
    if (!g_ready) return;

    /* НЕ ЧАЩЕ РАЗА В ДВАДЦАТЬ МИЛЛИСЕКУНД.
     *
     * Третий раз за вечер одна и та же моя ошибка: работа, повешенная в
     * фоновую прокрутку, выполняется со скоростью процессора — сотни
     * тысяч раз в секунду. Для сторожевого таймера это был поток записей
     * в его регистр, для показа загрузки — сотни мегабайт в память
     * экрана, а здесь — обращения к общей памяти и звонки в дверь
     * сопроцессору питания.
     *
     * Звонок это запись в регистр межпроцессорного прерывания. Делать
     * это непрерывно означает не давать сопроцессору заниматься ничем
     * другим — а он распоряжается питанием кристалла и вправе такого
     * собеседника перезагрузить.
     *
     * Ответы приходят раз в жизни (мы посылаем одну просьбу за
     * загрузку), так что двадцать миллисекунд здесь — с огромным
     * запасом. */
    static uint64_t last = 0;
    uint64_t now = hal_time_ms();
    if (now - last < RPM_POLL_INTERVAL_MS) return;
    last = now;

    /* Признак смены состояния снимаем всегда: пока он поднят,
       сопроцессор считает, что мы его не услышали. */
    if (fld_get(1, FLD_fSTATE)) fld_set(1, FLD_fSTATE, 0);

    uint32_t mask = g_fifo_size - 1;

    if (!fld_get(1, FLD_fHEAD) &&
        fld_get(1, FLD_HEAD) == fld_get(1, FLD_TAIL)) return;

    /* "Новые данные увидели" — ровно так это подтверждает драйвер ядра. */
    fld_set(1, FLD_fHEAD, 0);

    /* Предел оборотов: испорченные указатели не должны становиться
       вечным циклом посреди фоновой прокрутки. */
    for (int guard = 0; guard < 16; guard++) {
        uint32_t head = fld_get(1, FLD_HEAD);
        uint32_t tail = fld_get(1, FLD_TAIL);
        uint32_t avail = (head - tail) & mask;

        if (avail < SMD_PACKET_HDR) break;

        /* Длина посылки — первое слово заголовка. */
        uint32_t len = mmio_read32(g_rx_fifo + (tail & mask));

        if (len > g_fifo_size) {      /* явный мусор — очередь наотрез */
            fld_set(1, FLD_TAIL, head);
            break;
        }

        uint32_t total = SMD_PACKET_HDR + ((len + 3u) & ~3u);
        if (avail < total) break;     /* посылка ещё не дописана */

        fld_set(1, FLD_TAIL, (tail + total) & mask);
    }

    /* "Хвост подвинули" и будим — иначе сопроцессор не узнает, что
       место в очереди освободилось. */
    fld_set(1, FLD_fTAIL, 1);
    smd_signal();
}


/* Фоновая прокрутка: вызывается из каждого ожидания в системе. */
void hal_rpm_pump(void) { smd_rpm_poll(); }

/* Попросить сопроцессор включить источник питания.
 *
 * res_type — четырёхбуквенный вид ресурса ("ldoa"), res_id — его номер,
 * uv — напряжение в микровольтах (0 — не задавать).
 *
 * Просьба состоит из трёх частей: какая служба и сколько всего, затем
 * заголовок с видом и номером ресурса, затем тело — пары "ключ,
 * длина, значение". */
int rpm_regulator_enable(uint32_t res_type, uint32_t res_id, uint32_t uv) {
    if (!g_ready) return 0;

    uint32_t msg[13];
    uint32_t n = 0;

    uint32_t kvps = uv ? 2u : 1u;
    uint32_t body_bytes = kvps * 12u;

    msg[n++] = RPM_SERVICE_REQUEST;      /* какая служба                */
    msg[n++] = 20u + body_bytes;         /* длина всего остального      */

    msg[n++] = g_msg_id++;               /* номер просьбы               */
    msg[n++] = RPM_STATE_ACTIVE;         /* для рабочего хода           */
    msg[n++] = res_type;                 /* вид ресурса                 */
    msg[n++] = res_id;                   /* его номер                   */
    msg[n++] = body_bytes;               /* длина тела                  */

    if (uv) {
        msg[n++] = RPM_KEY_UV;
        msg[n++] = 4;
        msg[n++] = uv;
    }
    msg[n++] = RPM_KEY_SWEN;
    msg[n++] = 4;
    msg[n++] = 1;

    early_con_puts("RPM: prosim vklyuchit ");
    early_con_hex32(res_type);
    early_con_puts(" nomer ");
    early_con_hex32(res_id);
    early_con_puts("\n");

    return smd_send(msg, n);
}
