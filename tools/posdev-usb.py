#!/usr/bin/env python3
"""tools/posdev-usb.py — разговор с телефоном по проводу.

Система поднимает контроллер USB и представляется компьютеру
устройством. Всё, что она печатает на экран, попутно уходит в провод.

Отсюда ею можно и управлять — набором односимвольных команд:

    d   отдать весь журнал этой загрузки с самого начала
    b   перезагрузиться в загрузчик
    s   перезагрузиться в систему
    p   отозваться
    v   сказать, кто ты и на какой скорости

Режимы:
    listen <сек> [файл]   слушать до Ctrl-C
    dump   <сек> [файл]   забрать журнал и выйти самому
    cmd    <буква>        отдать команду и выйти

Режим dump выходит сам, когда провод замолчал, — на этом и держится
круг отладки без человека у стола.
"""
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    sys.stderr.write("posdev: нет модуля pyusb (pip3 install pyusb)\n")
    sys.exit(3)

# Номера выделены сообществом pid.codes под открытые любительские
# проекты; 0x0001 в них отведён под пробы. См. arch/arm64/usb_pos.c.
VID, PID = 0x1209, 0x0001

EP_IN, EP_OUT = 0x81, 0x01

# Сколько молчания считать концом выдачи. Журнал приходит одним потоком
# за доли секунды; три секунды тишины после него — это уже точно конец.
QUIET_SECONDS = 3.0

# Сколько дать системе поработать, прежде чем просить журнал. Загрузка до
# рабочего стола занимает около трёх секунд; берём с запасом.
PRE_DUMP_SECONDS = 6.0


def find_device(timeout):
    deadline = time.time() + timeout
    announced = False
    while time.time() < deadline:
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is not None:
            return dev
        if not announced:
            sys.stderr.write("posdev: жду телефон на проводе...\n")
            announced = True
        time.sleep(0.2)
    return None


def open_device(timeout):
    dev = find_device(timeout)
    if dev is None:
        return None
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass          # уже выбрана — это не ошибка
    return dev


def describe(dev):
    def s(getter):
        try:
            return getter()
        except Exception:
            return "?"
    sys.stderr.write("posdev: %s (%s, номер %s)\n"
                     % (s(lambda: dev.product), s(lambda: dev.manufacturer),
                        s(lambda: dev.serial_number)))


def send(dev, ch):
    try:
        dev.write(EP_OUT, ch.encode(), timeout=1000)
        return True
    except usb.core.USBError as e:
        sys.stderr.write("posdev: команда '%s' не ушла: %s\n" % (ch, e))
        return False


def is_timeout(err):
    return err.errno in (110, 60) or "timeout" in str(err).lower()


def stream(dev, out, stop_when_quiet):
    """Читать поток. Возвращает число принятых байт."""
    total = 0
    quiet = 0.0
    while True:
        try:
            data = dev.read(EP_IN, 1024, timeout=1000)
        except usb.core.USBError as e:
            if is_timeout(e):
                quiet += 1.0
                if stop_when_quiet and total and quiet >= QUIET_SECONDS:
                    return total
                if stop_when_quiet and quiet >= QUIET_SECONDS * 4:
                    return total     # так ничего и не пришло
                continue
            sys.stderr.write("\nposdev: телефон отключился (%s)\n" % e)
            return total
        quiet = 0.0
        total += len(data)
        text = bytes(data).decode("utf-8", "replace")
        sys.stdout.write(text)
        sys.stdout.flush()
        if out:
            out.write(text)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 1

    mode = sys.argv[1]

    if mode == "cmd":
        ch = sys.argv[2] if len(sys.argv) > 2 else "p"
        dev = open_device(float(sys.argv[3]) if len(sys.argv) > 3 else 30.0)
        if dev is None:
            sys.stderr.write("posdev: устройство не появилось\n")
            return 2
        ok = send(dev, ch)
        usb.util.dispose_resources(dev)
        return 0 if ok else 4

    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    out_path = sys.argv[3] if len(sys.argv) > 3 else None

    dev = open_device(seconds)
    if dev is None:
        sys.stderr.write("posdev: устройство не появилось\n")
        return 2

    describe(dev)
    out = open(out_path, "a", buffering=1) if out_path else None

    # СНАЧАЛА ДАЁМ СИСТЕМЕ ДОГОВОРИТЬ, ПОТОМ ПРОСИМ ЖУРНАЛ.
    #
    # Хост подключается примерно на второй секунде, а загрузка идёт ещё
    # несколько. Спросив журнал сразу, получаешь снимок на момент
    # вопроса — то есть ровно первые две секунды, — и обрыв посреди
    # подъёма тачскрина выглядит как зависание системы. Один заход на
    # этом и потерян: я полчаса искал несуществующий отказ.
    #
    # Живые строки после снимка приходят и сами, но надёжнее спросить
    # позже: тогда снимок и есть весь журнал.
    if mode == "dump":
        time.sleep(PRE_DUMP_SECONDS)
    send(dev, "d")

    sys.stderr.write("-" * 60 + "\n")
    try:
        stream(dev, out, stop_when_quiet=(mode == "dump"))
    except KeyboardInterrupt:
        sys.stderr.write("\nposdev: закончили\n")
    finally:
        if out:
            out.close()
        usb.util.dispose_resources(dev)
    return 0


if __name__ == "__main__":
    sys.exit(main())
