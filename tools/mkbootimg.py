#!/usr/bin/env python3
"""mkbootimg.py — упаковка ядра proshivkaOS NEXT в Android boot.img.

Зачем свой упаковщик, а не тот, что из AOSP: формат заголовка нулевой
версии — это 1632 байта простых полей, и тащить ради него зависимость с
половиной системы сборки Android незачем. Заодно скрипт документирует
сам формат, что полезнее ссылки на чужой репозиторий.

Что такое boot.img. Загрузчик телефона (LK/ABL у Qualcomm, аналоги у
других вендоров) не умеет читать файловые системы и не знает про ELF. Он
читает раздел boot целиком, разбирает вот этот заголовок, копирует ядро
по адресу kernel_addr, ramdisk — по ramdisk_addr, и прыгает на начало
ядра. Поэтому в образ кладётся не ELF, а СЫРОЙ бинарник с заголовком
ARM64 Image в первых байтах (см. arch/arm64/boot.S) — его делает
objcopy -O binary, цель "make ARCH=arm64 image".

Адреса по умолчанию — каноничные смещения от базы, которыми пользуется
mkbootimg из AOSP. Под конкретный аппарат их берут из его ядра
(BOARD_KERNEL_BASE в BoardConfig.mk) или из распакованного стокового
boot.img.

Использование:
    python3 tools/mkbootimg.py --kernel build/arm64/proshivkaos.img \\
        --base 0x80000000 --pagesize 2048 --output boot.img
"""
import argparse
import hashlib
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
HEADER_SIZE_V0 = 1632       # размер struct boot_img_hdr версии 0
HEADER_SIZE_V3 = 1580       # версии 3
HEADER_SIZE_V4 = 1584       # версии 4 (добавлено поле signature_size)


def pad_to(size, page_size):
    """Сколько байт добить до границы страницы."""
    rem = size % page_size
    return 0 if rem == 0 else page_size - rem


def read(path):
    if not path:
        return b""
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description="Собрать Android boot.img (header v0)")
    ap.add_argument("--kernel", required=True, help="сырой ARM64 Image")
    ap.add_argument("--ramdisk", help="ramdisk (не обязателен: у нас RamFS внутри ядра)")
    ap.add_argument("--second", help="второй загрузчик (почти никогда не нужен)")
    ap.add_argument("--base", default="0x80000000", help="базовый адрес загрузки")
    ap.add_argument("--pagesize", type=int, default=2048, help="размер страницы раздела")
    ap.add_argument("--cmdline", default="", help="командная строка ядра")
    ap.add_argument("--name", default="proshivkaOS", help="имя продукта (до 16 байт)")
    ap.add_argument("--header-version", type=int, default=0, choices=[0, 3, 4],
                    help="версия заголовка: 0 — старые устройства (Redmi Note 4 и т.п.), "
                         "3/4 — Android 12+ с GKI (Pixel 6/6a и новее)")
    ap.add_argument("--os-version", default="0.0.0",
                    help="версия Android для поля os_version, например 13.0.0. "
                         "Некоторые загрузчики откатывают образ с версией ниже текущей")
    ap.add_argument("--os-patch-level", default="1970-01",
                    help="уровень патчей в формате ГГГГ-ММ")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    base = int(args.base, 0)
    page = args.pagesize

    # В третьей версии заголовка размер страницы зафиксирован спецификацией
    # и в самом образе больше не хранится — договорённость на 4096.
    if args.header_version >= 3:
        page = 4096

    if page & (page - 1):
        sys.exit("pagesize должен быть степенью двойки")

    header_size = {0: HEADER_SIZE_V0, 3: HEADER_SIZE_V3, 4: HEADER_SIZE_V4}[args.header_version]
    if page < header_size:
        sys.exit("pagesize меньше размера заголовка (%d)" % header_size)

    kernel = read(args.kernel)
    ramdisk = read(args.ramdisk)
    second = read(args.second)

    if not kernel:
        sys.exit("пустое ядро: %s" % args.kernel)

    # Проверяем, что нам подсунули именно ARM64 Image, а не ELF. Ошибиться
    # тут легко (ELF и Image лежат в одном каталоге сборки), а загрузчик
    # телефона на такую ошибку отвечает молчаливым чёрным экраном.
    if kernel[:4] == b"\x7fELF":
        sys.exit("это ELF, а не сырой Image. Сначала: make ARCH=arm64 image")
    if kernel[:2] == b"\x1f\x8b":
        # Ядро сжато. Проверять магию бессмысленно — она появится только
        # после распаковки, которую сделает сам загрузчик.
        #
        # Сжатие здесь не ради экономии места. Загрузчик LK ищет приделанное
        # дерево устройств ровно двумя способами: у сжатого ядра смещение
        # возвращает распаковщик (там, где кончился поток gzip, начинается
        # дерево), а у несжатого читается 32-битное слово по смещению 0x2C.
        # Второе — соглашение 32-битного zImage; у ARM64 Image по этому
        # смещению лежит зарезервированное поле, то есть ноль, и загрузчик
        # уходит искать дерево в начало образа, где его нет. Итог —
        # "ERROR: Appended Device Tree Blob not found" и отказ грузиться.
        #
        # Поэтому образ с приделанным деревом собирается так:
        #     gzip -9 -c Image > Image.gz
        #     cat Image.gz mido.dtb > Image.gz-dtb
        print("ядро сжато (gzip) — так и надо, если к нему приделано дерево",
              file=sys.stderr)
    else:
        magic = struct.unpack_from("<I", kernel, 56)[0]
        if magic != 0x644D5241:
            print("ВНИМАНИЕ: в ядре нет магии ARM64 Image по смещению 56 "
                  "(получено 0x%08X). Загрузчик может его не принять." % magic,
                  file=sys.stderr)

    # os_version — упакованное поле: версия Android и уровень патчей в одном
    # 32-битном числе. Загрузчики с защитой от отката сравнивают именно его.
    def pack_os_version(ver, patch):
        try:
            parts = [int(x) for x in ver.split(".")][:3]
            while len(parts) < 3:
                parts.append(0)
            a, b, c = parts
            year, month = (int(x) for x in patch.split("-")[:2])
        except ValueError:
            sys.exit("не разобрать --os-version/--os-patch-level")
        if not (2000 <= year <= 2127 and 1 <= month <= 12):
            year, month = 2000, 1
        return ((a << 25) | (b << 18) | (c << 11) |
                ((year - 2000) << 4) | month)

    os_version = pack_os_version(args.os_version, args.os_patch_level)

    # Каноничные смещения mkbootimg из AOSP.
    kernel_addr = base + 0x00008000
    ramdisk_addr = base + 0x01000000
    second_addr = base + 0x00F00000
    tags_addr = base + 0x00000100

    cmdline = args.cmdline.encode()
    if len(cmdline) > 512 + 1024:
        sys.exit("слишком длинная cmdline")

    # id — SHA1 по содержимому и размерам всех трёх частей. Загрузчики его
    # обычно не проверяют, но инструменты распаковки сверяют.
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    img_id = sha.digest()[:20] + b"\x00" * 12   # поле id[8] — 32 байта

    if args.header_version >= 3:
        # В версии 3 из заголовка убрали ВСЕ адреса загрузки: куда класть
        # ядро и ramdisk, решает сам загрузчик. Убрали и second, и dtb —
        # устройство дерево теперь лежит в отдельном образе vendor_boot.
        header = b"".join([
            BOOT_MAGIC,
            struct.pack("<I", len(kernel)),
            struct.pack("<I", len(ramdisk)),
            struct.pack("<I", os_version),
            struct.pack("<I", header_size),
            struct.pack("<IIII", 0, 0, 0, 0),          # reserved
            struct.pack("<I", args.header_version),
            cmdline[:1536].ljust(1536, b"\x00"),
        ])
        if args.header_version == 4:
            header += struct.pack("<I", 0)             # signature_size
        assert len(header) == header_size, len(header)

        with open(args.output, "wb") as out:
            out.write(header)
            out.write(b"\x00" * pad_to(len(header), page))
            for blob in (kernel, ramdisk):
                if not blob:
                    continue
                out.write(blob)
                out.write(b"\x00" * pad_to(len(blob), page))

        print("собрано: %s (заголовок версии %d)" % (args.output, args.header_version))
        print("  ядро    : %d байт" % len(kernel))
        print("  адреса загрузки в заголовке версии 3+ не хранятся —")
        print("  их выбирает загрузчик устройства.")
        print()
        print("Прошивка (бутлоадер должен быть разлочен):")
        print("  fastboot flash boot %s" % args.output)
        return

    header = b"".join([
        BOOT_MAGIC,
        struct.pack("<I", len(kernel)),
        struct.pack("<I", kernel_addr),
        struct.pack("<I", len(ramdisk)),
        struct.pack("<I", ramdisk_addr),
        struct.pack("<I", len(second)),
        struct.pack("<I", second_addr),
        struct.pack("<I", tags_addr),
        struct.pack("<I", page),
        struct.pack("<I", 0),          # header_version = 0
        struct.pack("<I", os_version), # os_version + os_patch_level
        args.name.encode()[:16].ljust(16, b"\x00"),
        cmdline[:512].ljust(512, b"\x00"),
        img_id,
        cmdline[512:].ljust(1024, b"\x00"),   # extra_cmdline
    ])
    assert len(header) == HEADER_SIZE_V0, len(header)

    with open(args.output, "wb") as out:
        out.write(header)
        out.write(b"\x00" * pad_to(len(header), page))
        for blob in (kernel, ramdisk, second):
            if not blob:
                continue
            out.write(blob)
            out.write(b"\x00" * pad_to(len(blob), page))

    total = HEADER_SIZE_V0 + pad_to(HEADER_SIZE_V0, page)
    for blob in (kernel, ramdisk, second):
        if blob:
            total += len(blob) + pad_to(len(blob), page)

    print("собрано: %s (%d байт)" % (args.output, total))
    print("  ядро      : %d байт -> 0x%08X" % (len(kernel), kernel_addr))
    if ramdisk:
        print("  ramdisk   : %d байт -> 0x%08X" % (len(ramdisk), ramdisk_addr))
    print("  tags      : 0x%08X" % tags_addr)
    print("  pagesize  : %d" % page)
    print()
    print("Прошивка (бутлоадер должен быть разлочен):")
    print("  fastboot flash boot %s" % args.output)
    print("или без записи в память устройства, разово:")
    print("  fastboot boot %s" % args.output)


if __name__ == "__main__":
    main()
