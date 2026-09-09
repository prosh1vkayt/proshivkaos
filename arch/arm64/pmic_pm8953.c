/* arch/arm64/pmic_pm8953.c — источники питания внутри микросхемы PM8953.
 *
 * ЧТО ЗДЕСЬ ДЕЛАЕТСЯ. Внутри микросхемы питания десятки отдельных
 * источников; каждый выглядит как небольшой набор регистров по своему
 * адресу. Нам нужны пять:
 *
 *   l6   1.8 В   подтяжки шины I2C тачскрина  (vcc_i2c)
 *   l10  2.85 В  сам тачскрин                 (vdd)
 *   l3   0.925 В приёмопередатчик USB, цифра  (vdd)
 *   l7   1.8 В   приёмопередатчик USB, аналог (vdda18)
 *   l13  3.075 В приёмопередатчик USB, линия  (vdda33)
 *
 * В штатной системе ими распоряжается не процессор, а отдельный
 * сопроцессор питания: драйверы просят его через общую память, а он уже
 * пишет в микросхему. Разговаривать с ним — отдельная большая работа, и
 * здесь мы идём короче: пишем в микросхему напрямую по шине SPMI. Так же
 * поступает загрузчик, когда ему надо зажечь экран.
 *
 * ПОЧЕМУ ЭТО НЕ ОПАСНО, ХОТЯ ВЫГЛЯДИТ ОПАСНО.
 *
 * Адреса источников получены не из документации, а из принятой в этих
 * микросхемах раскладки: источник номер n лежит по 0x4000 + (n-1)*0x100.
 * Ошибиться в такой догадке — значит писать в чужой узел, и вот это уже
 * может стоить железа. Поэтому перед любой записью узел ОПРАШИВАЕТСЯ: у
 * каждого узла есть регистр вида, и мы пишем, только если он ответил
 * "источник питания". Если по адресу окажется что-то другое, мы просто
 * ничего не сделаем и скажем об этом.
 *
 * И второе: напряжение мы НЕ ТРОГАЕМ. Оно уже задано загрузчиком, и
 * менять его вслепую незачем — нам нужно только, чтобы источник был
 * включён. Единственная запись здесь — разряд включения.
 */
#include "arm64.h"
#include "boards/board.h"

/* Регистры узла — общие для всех источников */
#define REG_TYPE            0x04
#define REG_SUBTYPE         0x05
#define REG_VOLTAGE_RANGE   0x40
#define REG_VOLTAGE_SET     0x41
#define REG_MODE            0x45
#define REG_ENABLE          0x46

#define ENABLE_BIT          0x80

/* Виды узлов, которые считаются источниками питания. */
#define TYPE_LDO            0x04
#define TYPE_ULT_LDO        0x21

/* НОМЕР ВЕДОМОГО. Источники питания живут на ведомом НОМЕР ОДИН, а не
 * ноль, как я поначалу решил. Выяснилось это не рассуждением, а обходом:
 * арбитр шины перечисляет доступные узлы, у каждого спрашивается вид, и
 * двадцать четыре из них ответили "источник питания" — все с адресами
 * вида 01xx.
 *
 * Раскладка адресов при этом оказалась той самой, предполагавшейся:
 * источник номер n лежит по 0x4000 + (n-1)*0x100. Неверна была только
 * половина догадки, и именно из-за неё все двадцать три обращения
 * уходили в пустоту. */
#define PMIC_SID            1

uint16_t pmic_ldo_base(int n) { return (uint16_t)(0x4000 + (n - 1) * 0x100); }

/* Показать всё, что узел о себе рассказывает. Это первое, на что смотришь,
   когда источник не включается: по виду и подвиду сразу видно, попали ли
   мы вообще в источник питания. */
void pmic_ldo_dump(int n) {
    uint16_t base = pmic_ldo_base(n);
    uint8_t type = 0, sub = 0, range = 0, vset = 0, mode = 0, en = 0;

    int ok = spmi_read(PMIC_SID, base + REG_TYPE, &type) &&
             spmi_read(PMIC_SID, base + REG_SUBTYPE, &sub) &&
             spmi_read(PMIC_SID, base + REG_VOLTAGE_RANGE, &range) &&
             spmi_read(PMIC_SID, base + REG_VOLTAGE_SET, &vset) &&
             spmi_read(PMIC_SID, base + REG_MODE, &mode) &&
             spmi_read(PMIC_SID, base + REG_ENABLE, &en);

    early_con_puts("PMIC: l");
    early_con_hex32((uint32_t)n);
    if (!ok) { early_con_puts(" ne otvechaet\n"); return; }

    early_con_puts(" vid ");   early_con_hex8(type);
    early_con_puts("/");       early_con_hex8(sub);
    early_con_puts(" napr ");  early_con_hex8(range);
    early_con_puts(":");       early_con_hex8(vset);
    early_con_puts(" rezhim "); early_con_hex8(mode);
    early_con_puts(" vkl ");   early_con_hex8(en);
    early_con_puts((en & ENABLE_BIT) ? " (RABOTAET)\n" : " (VYKLYUCHEN)\n");
}

/* ОБОЙТИ ВСЁ, ЧТО ШИНА ОБЪЯВИЛА ДОСТУПНЫМ.
 *
 * Предыдущий заход показал, что искать источники по предполагаемым
 * адресам бесполезно: из двадцати трёх ни один не оказался источником
 * питания. Гадать дальше незачем — арбитр шины сам перечисляет, какие
 * узлы через него видны, и у каждого узла можно спросить, что он такое.
 *
 * Печатаются все узлы подряд (адрес и вид), а отдельной строкой — те,
 * чей вид означает источник питания. Это и есть ответ на вопрос, где они
 * лежат и доступны ли нам вообще. */
void pmic_scan(void) {
    int total = spmi_channel_count();
    int shown = 0, regs = 0;

    early_con_puts("PMIC: uzly na shine (adres:vid)\n ");

    for (int apid = 0; apid < total; apid++) {
        uint16_t ppid = spmi_channel_ppid(apid);
        if (ppid == 0xFFFF) continue;

        uint8_t sid  = (uint8_t)(ppid >> 8);
        uint16_t addr = (uint16_t)((ppid & 0xFF) << 8);

        uint8_t type = 0;
        if (!spmi_read(sid, addr, &type)) continue;      /* разбудить канал */
        if (!spmi_read(sid, (uint16_t)(addr + REG_TYPE), &type)) continue;

        early_con_puts(" ");
        early_con_hex8(sid);
        early_con_hex8((uint8_t)(ppid & 0xFF));
        early_con_puts(":");
        early_con_hex8(type);

        if (++shown % 8 == 0) early_con_puts("\n ");

        if (type == TYPE_LDO || type == TYPE_ULT_LDO) regs++;
    }
    early_con_puts("\n");

    /* А теперь подробно — только источники питания. */
    early_con_puts("PMIC: iz nih istochnikov pitaniya ");
    early_con_hex32((uint32_t)regs);
    early_con_puts("\n");

    for (int apid = 0; apid < total; apid++) {
        uint16_t ppid = spmi_channel_ppid(apid);
        if (ppid == 0xFFFF) continue;

        uint8_t sid  = (uint8_t)(ppid >> 8);
        uint16_t addr = (uint16_t)((ppid & 0xFF) << 8);

        uint8_t type = 0, sub = 0, range = 0, vset = 0, en = 0;
        if (!spmi_read(sid, (uint16_t)(addr + REG_TYPE), &type)) continue;
        if (type != TYPE_LDO && type != TYPE_ULT_LDO) continue;

        spmi_read(sid, (uint16_t)(addr + REG_SUBTYPE), &sub);
        spmi_read(sid, (uint16_t)(addr + REG_VOLTAGE_RANGE), &range);
        spmi_read(sid, (uint16_t)(addr + REG_VOLTAGE_SET), &vset);
        spmi_read(sid, (uint16_t)(addr + REG_ENABLE), &en);

        early_con_puts("PMIC:  ");
        early_con_hex8(sid); early_con_hex8((uint8_t)(ppid & 0xFF));
        early_con_puts(" podvid "); early_con_hex8(sub);
        early_con_puts(" napr ");   early_con_hex8(range);
        early_con_puts(":");        early_con_hex8(vset);
        early_con_puts((en & ENABLE_BIT) ? " RABOTAET" : " vyklyuchen");
        early_con_puts(" hozyain ");
        early_con_hex32((uint32_t)spmi_owner_of(sid, addr));
        early_con_puts("\n");
    }
}

/* Включить источник, если он выключен. Возвращает 1, если после вызова он
   работает. */
int pmic_ldo_enable(int n) {
    uint16_t base = pmic_ldo_base(n);
    uint8_t type = 0, en = 0;

    if (!spmi_read(PMIC_SID, base + REG_TYPE, &type)) {
        early_con_puts("PMIC: l");
        early_con_hex32((uint32_t)n);
        early_con_puts(" ne otvechaet, ne trogaem\n");
        return 0;
    }

    /* Самопроверка адреса. Пишем только туда, где узел сам подтвердил,
       что он источник питания. */
    if (type != TYPE_LDO && type != TYPE_ULT_LDO) {
        early_con_puts("PMIC: po adresu l");
        early_con_hex32((uint32_t)n);
        early_con_puts(" ne istochnik pitaniya, a vid ");
        early_con_hex8(type);
        early_con_puts(" — nichego ne pishem\n");
        return 0;
    }

    if (!spmi_read(PMIC_SID, base + REG_ENABLE, &en)) return 0;
    if (en & ENABLE_BIT) return 1;              /* уже работает */

    if (!spmi_write(PMIC_SID, base + REG_ENABLE, (uint8_t)(en | ENABLE_BIT))) {
        early_con_puts("PMIC: zapis ne proshla\n");
        return 0;
    }

    /* Читаем обратно: разряд мог не удержаться, если источником
       распоряжается кто-то ещё. */
    en = 0;
    spmi_read(PMIC_SID, base + REG_ENABLE, &en);

    early_con_puts("PMIC: l");
    early_con_hex32((uint32_t)n);
    early_con_puts((en & ENABLE_BIT) ? " vklyuchen\n" : " ne vklyuchilsya\n");
    return (en & ENABLE_BIT) ? 1 : 0;
}
