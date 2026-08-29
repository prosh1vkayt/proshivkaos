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
HEADER_SIZE = 1632          # размер struct boot_img_hdr версии 0


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
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    base = int(args.base, 0)
    page = args.pagesize

    if page & (page - 1):
        sys.exit("pagesize должен быть степенью двойки")
    if page < HEADER_SIZE:
        sys.exit("pagesize меньше размера заголовка (%d)" % HEADER_SIZE)

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
    magic = struct.unpack_from("<I", kernel, 56)[0]
    if magic != 0x644D5241:
        print("ВНИМАНИЕ: в ядре нет магии ARM64 Image по смещению 56 "
              "(получено 0x%08X). Загрузчик может его не принять." % magic,
              file=sys.stderr)

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
        struct.pack("<I", 0),          # os_version + os_patch_level
        args.name.encode()[:16].ljust(16, b"\x00"),
        cmdline[:512].ljust(512, b"\x00"),
        img_id,
        cmdline[512:].ljust(1024, b"\x00"),   # extra_cmdline
    ])
    assert len(header) == HEADER_SIZE, len(header)

    with open(args.output, "wb") as out:
        out.write(header)
        out.write(b"\x00" * pad_to(len(header), page))
        for blob in (kernel, ramdisk, second):
            if not blob:
                continue
            out.write(blob)
            out.write(b"\x00" * pad_to(len(blob), page))

    total = HEADER_SIZE + pad_to(HEADER_SIZE, page)
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
