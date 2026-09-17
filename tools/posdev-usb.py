#!/usr/bin/env python3
"""tools/posdev-usb.py — разговор с телефоном по проводу.

Система поднимает контроллер USB и представляется компьютеру
устройством. Всё, что она печатает на экран, попутно уходит в провод.

Отсюда ею можно и управлять — набором односимвольных команд:

    d   отдать весь журнал этой загрузки с самого начала
    b   перезагрузиться в загрузчик  (уходит как "POSb")
    s   перезагрузиться в систему    (уходит как "POSs")
    p   отозваться
    v   сказать, кто ты и на какой скорости

Режимы:
    ping   [сколько]      замерить отклик телефона
    listen <сек> [файл]   слушать до Ctrl-C
    dump   <сек> [файл]   забрать журнал и выйти самому
    cmd    <буква>        отдать команду и выйти
    stress <сек> [в сек]  нагрузка: быстрый набор и касания клавиатуры,
                          с проверкой, что телефон отвечает

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


# Опасные команды требуют отличительной приставки: один байт в потоке
# берётся из чего угодно, и однобайтовая перезагрузка оказалась прямой
# причиной того, что телефон выключался сам через случайное время.
DANGEROUS = "bsw"
MAGIC = "POS"


def send(dev, ch):
    if ch in DANGEROUS:
        ch = MAGIC + ch
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


# Экран mido. Касания бьют по буквенным рядам клавиатуры — туда, куда
# целится палец при наборе; нижний ряд с кнопкой «убрать клавиатуру» и
# панель навигации не трогаем, иначе проверка уйдёт с терминала.
SCREEN_W, SCREEN_H = 1080, 1920


def stress(seconds, rate, open_terminal=True):
    """Набирать быстрее человека и следить, что телефон жив.

    Раз в секунду — отзыв 'p'. Два пропуска подряд — телефон считается
    упавшим: провод не отвечает, а значит не отвечает и система."""
    import random
    dev = open_device(30.0)
    if dev is None:
        sys.stderr.write("posdev: устройство не появилось\n")
        return 2
    describe(dev)

    # С рабочего стола Esc открывает терминал — туда и идёт набор. После
    # загрузки на экране именно рабочий стол; если уже открыт терминал,
    # Esc вернёт на стол, и касания по клавиатуре откроют его снова не
    # сразу — поэтому тест лучше запускать на свежей загрузке.
    if open_terminal:
        dev.write(EP_OUT, b"POSk\x1b", timeout=1000)
        time.sleep(0.5)

    words = ["ls", "help", "pwd", "uptime", "date", "abcdefghij",
             "the quick brown fox", "0123456789"]
    t0 = time.time()
    sent_keys = sent_taps = 0
    last_ping = 0.0
    misses = 0
    answered = 0
    buf = b""

    def pump():
        nonlocal buf
        try:
            buf += bytes(dev.read(EP_IN, 4096, timeout=5))
        except usb.core.USBError:
            pass
        if len(buf) > 65536:
            buf = buf[-4096:]

    try:
        while time.time() - t0 < seconds:
            now = time.time()
            if now - last_ping >= 1.0:
                if last_ping:
                    if b"ping" in buf:
                        answered += 1
                        misses = 0
                    else:
                        misses += 1
                        if misses >= 2:
                            print("УПАЛ на %.1f с: два отзыва подряд без ответа "
                                  "(клавиш %d, касаний %d)"
                                  % (now - t0, sent_keys, sent_taps))
                            return 1
                buf = b""
                dev.write(EP_OUT, b"p", timeout=1000)
                last_ping = now

            if random.random() < 0.7:
                w = random.choice(words)
                for ch in w + ("\n" if random.random() < 0.5 else " "):
                    dev.write(EP_OUT, b"POSk" + ch.encode(), timeout=1000)
                    sent_keys += 1
                    pump()
                    time.sleep(1.0 / rate)
            else:
                x = random.randint(20, SCREEN_W - 20)
                y = random.randint(int(SCREEN_H * 0.66), int(SCREEN_H * 0.84))
                dev.write(EP_OUT, b"POSt" + bytes([x >> 8, x & 255, y >> 8, y & 255]),
                          timeout=1000)
                sent_taps += 1
                pump()
                time.sleep(1.0 / rate)
            pump()
    except usb.core.USBError as e:
        print("УПАЛ на %.1f с: провод пропал (%s), клавиш %d, касаний %d"
              % (time.time() - t0, e, sent_keys, sent_taps))
        return 1
    finally:
        usb.util.dispose_resources(dev)

    print("ЖИВ %.0f с под нагрузкой: клавиш %d, касаний %d, отзывов %d"
          % (seconds, sent_keys, sent_taps, answered))
    return 0


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 1

    mode = sys.argv[1]

    if mode == "ping":
        # НАСТОЯЩИЙ ЗАМЕР, А НЕ "ВРОДЕ РАБОТАЕТ".
        #
        # Посылаем букву и ждём ответа, засекая время. Это проверяет всю
        # цепочку разом: контроллер USB на телефоне, разбор команд, кольцо
        # журнала, обратную передачу. Если отвечает — работает всё.
        count = int(sys.argv[2]) if len(sys.argv) > 2 else 5
        dev = open_device(30.0)
        if dev is None:
            sys.stderr.write("posdev: устройство не появилось\n")
            return 2
        describe(dev)

        # Сначала осушаем: в кольце мог накопиться журнал загрузки, и
        # первый же ответ пришёл бы вперемешку с ним.
        t = time.time()
        while time.time() - t < 1.5:
            try:
                dev.read(EP_IN, 1024, timeout=200)
            except usb.core.USBError:
                pass

        times = []
        for i in range(count):
            t0 = time.time()
            if not send(dev, "p"):
                print("%d: не ушло" % (i + 1))
                continue
            got = False
            while time.time() - t0 < 2.0:
                try:
                    data = bytes(dev.read(EP_IN, 1024, timeout=300))
                except usb.core.USBError:
                    continue
                if b"ping" in data:
                    got = True
                    break
            dt = (time.time() - t0) * 1000.0
            if got:
                times.append(dt)
                print("otvet ot mido-0001: %.1f ms" % dt)
            else:
                print("%d: net otveta" % (i + 1))
            time.sleep(0.3)

        usb.util.dispose_resources(dev)
        print("---")
        if times:
            print("otpravleno %d, polucheno %d, poteryano %d%%"
                  % (count, len(times), (count - len(times)) * 100 // count))
            print("min/sred/max = %.1f/%.1f/%.1f ms"
                  % (min(times), sum(times) / len(times), max(times)))
            return 0
        print("otveta net vovse")
        return 1

    if mode == "time":
        # Настоящее время телефону: секунды UNIX и часовой пояс компьютера.
        dev = open_device(float(sys.argv[2]) if len(sys.argv) > 2 else 30.0)
        if dev is None:
            sys.stderr.write("posdev: устройство не появилось\n")
            return 2
        now = int(time.time())
        tz = int(time.localtime(now).tm_gmtoff // 60)
        dev.write(EP_OUT, b"POSu" + now.to_bytes(4, "big") + tz.to_bytes(2, "big", signed=True),
                  timeout=1000)
        usb.util.dispose_resources(dev)
        print("posdev: время отдано (UTC%+d:%02d)" % (tz // 60, abs(tz) % 60))
        return 0

    if mode == "shot":
        # Снимок экрана телефона: команда S, в ответ заголовок POSSHOT и
        # пиксели RGB уменьшенного втрое кадра.
        out_path = sys.argv[2] if len(sys.argv) > 2 else "shot.png"
        dev = open_device(30.0)
        if dev is None:
            sys.stderr.write("posdev: устройство не появилось\n")
            return 2
        try:
            while True:
                dev.read(EP_IN, 4096, timeout=200)
        except usb.core.USBError:
            pass
        dev.write(EP_OUT, b"S", timeout=1000)
        buf = b""
        t0 = time.time()
        need = None
        while time.time() - t0 < 60:
            try:
                buf += bytes(dev.read(EP_IN, 65536, timeout=500))
            except usb.core.USBError:
                if need is None and time.time() - t0 > 10:
                    break
            i = buf.find(b"POSSHOT")
            if i >= 0 and len(buf) >= i + 11:
                w = (buf[i + 7] << 8) | buf[i + 8]
                h = (buf[i + 9] << 8) | buf[i + 10]
                need = i + 11 + w * h * 3
                if len(buf) >= need:
                    break
        usb.util.dispose_resources(dev)
        if need is None or len(buf) < need:
            sys.stderr.write("posdev: снимок не пришёл целиком (%d байт)\n" % len(buf))
            return 1
        try:
            from PIL import Image
        except ImportError:
            sys.stderr.write("posdev: для PNG нужен Pillow (pip3 install pillow)\n")
            return 3
        Image.frombytes("RGB", (w, h), buf[i + 11:need]).save(out_path)
        print("снимок %dx%d за %.1f с: %s" % (w, h, time.time() - t0, out_path))
        return 0

    if mode == "bench":
        # Замер кадра: клавиша с кодом 1 запускает бенчмарк в оболочке,
        # итог приходит строками BENCH по проводу.
        dev = open_device(30.0)
        if dev is None:
            sys.stderr.write("posdev: устройство не появилось\n")
            return 2
        try:
            while True:
                dev.read(EP_IN, 4096, timeout=200)
        except usb.core.USBError:
            pass
        dev.write(EP_OUT, b"POSk\x01", timeout=1000)
        buf = b""
        t0 = time.time()
        while time.time() - t0 < 60 and b"BENCH: gotovo" not in buf:
            try:
                buf += bytes(dev.read(EP_IN, 4096, timeout=300))
            except usb.core.USBError:
                pass
        for line in buf.decode("utf-8", "replace").splitlines():
            if line.startswith("BENCH"):
                print(line)
        usb.util.dispose_resources(dev)
        return 0 if b"BENCH: gotovo" in buf else 1

    if mode == "stress":
        return stress(float(sys.argv[2]) if len(sys.argv) > 2 else 60.0,
                      float(sys.argv[3]) if len(sys.argv) > 3 else 40.0)

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
