#!/usr/bin/env python3
"""tools/posdev-usb.py — приём журнала с телефона по проводу.

ЗАЧЕМ. Раньше журнал загрузки доставался так: собрать образ, загрузить,
подождать, вручную вернуть телефон в загрузчик, поднять recovery,
вычитать сохранённую область ОЗУ. Минуты на заход и живой человек на
каждом шаге.

Теперь система сама поднимает контроллер USB и представляется
компьютеру устройством. Всё, что она печатает на экран, попутно уходит
в провод — и появляется здесь сразу, пока телефон работает.

Устройство своего, не стандартного вида: два потока данных, из телефона
идёт текст, обратно — односимвольные команды ('p' — отзовись, 'v' —
скажи, кто ты). Драйвер ему не нужен ни в macOS, ни в Linux; в Windows
он сам подставляется по описанию, которое устройство отдаёт.
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


def find_device(timeout):
    deadline = time.time() + timeout
    first = True
    while time.time() < deadline:
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is not None:
            return dev
        if first:
            sys.stderr.write("posdev: жду телефон на проводе...\n")
            first = False
        time.sleep(0.2)
    return None


def describe(dev):
    def s(getter):
        try:
            return getter()
        except Exception:
            return "?"
    name = s(lambda: dev.product)
    maker = s(lambda: dev.manufacturer)
    serial = s(lambda: dev.serial_number)
    sys.stderr.write("posdev: %s (%s, номер %s)\n" % (name, maker, serial))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    out_path = sys.argv[2] if len(sys.argv) > 2 else None

    dev = find_device(seconds)
    if dev is None:
        sys.stderr.write("posdev: устройство не появилось\n")
        return 2

    describe(dev)

    try:
        dev.set_configuration()
    except usb.core.USBError as e:
        # Конфигурация уже выбрана — это не ошибка.
        if "Busy" in str(e) or "busy" in str(e):
            sys.stderr.write("posdev: интерфейс занят другой программой\n")
            return 4

    out = open(out_path, "a", buffering=1) if out_path else None

    # Спрашиваем, кто на том конце: заодно проверяем, что канал работает
    # в обе стороны, а не только на приём.
    try:
        dev.write(EP_OUT, b"v", timeout=500)
    except usb.core.USBError:
        sys.stderr.write("posdev: команда не ушла (канал только на приём?)\n")

    sys.stderr.write("posdev: слушаю. Ctrl-C — закончить.\n")
    sys.stderr.write("-" * 60 + "\n")

    idle = 0.0
    try:
        while True:
            try:
                data = dev.read(EP_IN, 1024, timeout=1000)
            except usb.core.USBError as e:
                # Тишина на линии — обычное дело: система печатает
                # неравномерно. Отличаем её от пропавшего устройства.
                if e.errno in (110, 60) or "timeout" in str(e).lower():
                    idle += 1.0
                    continue
                sys.stderr.write("\nposdev: телефон отключился (%s)\n" % e)
                break
            idle = 0.0
            text = bytes(data).decode("utf-8", "replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            if out:
                out.write(text)
    except KeyboardInterrupt:
        sys.stderr.write("\nposdev: закончили\n")
    finally:
        if out:
            out.close()
        usb.util.dispose_resources(dev)

    return 0


if __name__ == "__main__":
    sys.exit(main())
