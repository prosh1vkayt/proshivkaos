#!/usr/bin/env python3
"""tools/fwpack.py — упаковать прошивки сопроцессоров в образ системы.

Прошивки радио Wi-Fi (wcnss.*) и zap-шейдер GPU (a506_zap.*) подписаны под
устройство и берутся с самого телефона (раздел modem и vendor). В
репозиторий их класть нельзя, поэтому они лежат локально — по умолчанию
~/mido/firmware — и попадают только в собранный образ.

Файлы из каталога (рекурсивно, по имени файла) складываются в один блок:
  "POSFW\\0\\0\\0", число файлов (u32), таблица {имя[56], смещение, размер},
  затем данные, выровненные на 8 байт.

Использование: fwpack.py <каталог> <выход.bin>
Каталога нет — пишется пустой блок: система соберётся и без прошивок.
"""
import os, struct, sys

src, out = sys.argv[1], sys.argv[2]
files = []
if os.path.isdir(src):
    for root, _, names in sorted(os.walk(src)):
        for n in sorted(names):
            if n.startswith("."):
                continue
            files.append((n, os.path.join(root, n)))

NAME = 56
head = bytearray(b"POSFW\0\0\0" + struct.pack("<I", len(files)))
table_size = len(files) * (NAME + 8)
offset = len(head) + table_size
offset = (offset + 7) & ~7
data = bytearray()
entries = bytearray()
for name, path in files:
    blob = open(path, "rb").read()
    entries += name.encode()[:NAME - 1].ljust(NAME, b"\0") + struct.pack("<II", offset + len(data), len(blob))
    data += blob
    data += b"\0" * ((-len(data)) % 8)
blk = head + entries
blk += b"\0" * (((len(blk) + 7) & ~7) - len(blk))
blk += data
open(out, "wb").write(blk)
print("fwpack: %d файлов, %d КБ" % (len(files), len(blk) // 1024))
