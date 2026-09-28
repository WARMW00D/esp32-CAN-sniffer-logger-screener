#!/usr/bin/env python3
"""
can_time_decode.py — дата/время и пробег из CAN-лога сниффера.

Источник: кадр 0x6B2 (Diagnose_01, VAG MQB/MLB), шлётся раз в ~1 с.
Раскладка сигналов (Intel / little-endian, start|len):
    KBI_Kilometerstand_2  8|20   пробег, км
    UH_Jahr              28|7    год - 2000
    UH_Monat             35|4
    UH_Tag               39|5
    UH_Stunde            44|5
    UH_Minute            49|6
    UH_Sekunde           55|6

Формат лога: "<millis> <S|X> <ID hex> [R]<DLC> <байты hex>", строки с '#' —
маркеры. Маркер BOOT начинает новую сессию (millis отсчитывается заново).

Использование:
    python can_time_decode.py can_log_0001.txt.gz [can_log_0002.txt.gz ...] [--tz 3] [--all]

Понимает и простые .txt, и сжатые .txt.gz (в том числе оборванные — без
штатного закрытия файла: читается всё, что успело записаться).

    --tz N   сдвиг в часах, если машина шлёт UTC (для Москвы: --tz 3)
    --all    печатать все кадры 0x6B2, а не только начало/конец сессии

Для каждой сессии выводится привязка: какому времени машины соответствует
millis сниффера. По ней любое <millis> из лога (и имя кадра камеры
f_<millis>.jpg) переводится в реальное время.
"""
import argparse
import datetime as dt
import sys
import zlib

TIME_ID = 0x6B2


def sig(data: bytes, start: int, length: int) -> int:
    raw = int.from_bytes(data.ljust(8, b"\0"), "little")
    return (raw >> start) & ((1 << length) - 1)


def decode_6b2(data: bytes):
    """-> (одометр_км, datetime) или None, если поля вне диапазона."""
    try:
        t = dt.datetime(2000 + sig(data, 28, 7), sig(data, 35, 4), sig(data, 39, 5),
                        sig(data, 44, 5), sig(data, 49, 6), sig(data, 55, 6))
    except ValueError:
        return None
    return sig(data, 8, 20), t


def read_lines(path):
    """Строки лога из .txt или .txt.gz (оборванный gzip тоже читается)."""
    if path.lower().endswith(".gz"):
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        tail = b""
        with open(path, "rb") as f:
            while True:
                chunk = f.read(1 << 16)
                if not chunk:
                    break
                try:
                    data = tail + d.decompress(chunk)
                except zlib.error:
                    break              # повреждённый хвост — отдаём, что есть
                *lines, tail = data.split(b"\n")
                for l in lines:
                    yield l.decode("utf-8", "replace")
        if tail:
            yield tail.decode("utf-8", "replace")
    else:
        with open(path, encoding="utf-8", errors="replace") as f:
            yield from f


def parse(paths):
    """Генератор (имя_файла, номер_сессии, millis, одометр, datetime)."""
    session = 0
    for path in paths:
        for line in read_lines(path):
            if True:
                line = line.strip()
                if not line:
                    continue
                if line.startswith("#"):
                    if "BOOT" in line:
                        session += 1
                    continue
                p = line.split()
                if len(p) < 4 or p[1] != "S":
                    continue
                try:
                    if int(p[2], 16) != TIME_ID or p[3].startswith("R"):
                        continue
                    ms = int(p[0])
                    data = bytes(int(x, 16) for x in p[4:4 + int(p[3])])
                except ValueError:
                    continue
                r = decode_6b2(data)
                if r:
                    yield path, session, ms, r[0], r[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--tz", type=float, default=0.0, help="сдвиг в часах (машина шлёт UTC)")
    ap.add_argument("--all", action="store_true", help="печатать все кадры 0x6B2")
    a = ap.parse_args()
    shift = dt.timedelta(hours=a.tz)

    rows = list(parse(a.logs))
    if not rows:
        print("Кадров 0x6B2 не найдено (нет трафика или другая шина).")
        return 1

    by_session = {}
    for r in rows:
        by_session.setdefault((r[0], r[1]), []).append(r)

    for (path, sess), items in by_session.items():
        first, last = items[0], items[-1]
        t0 = first[4] + shift
        # Привязка: реальное время в момент millis=0 этой сессии
        base = t0 - dt.timedelta(milliseconds=first[2])
        print(f"\n=== {path}, сессия {sess}: {len(items)} кадров 0x6B2, пробег {first[3]} км")
        print(f"    millis {first[2]:>9} -> {t0:%Y-%m-%d %H:%M:%S}")
        print(f"    millis {last[2]:>9} -> {last[4] + shift:%Y-%m-%d %H:%M:%S}")
        print(f"    привязка: время = {base:%Y-%m-%d %H:%M:%S.%f}"[:-3] + " + millis")
        # Проверка: время машины должно идти вровень с millis сниффера
        drift = (last[4] - first[4]).total_seconds() - (last[2] - first[2]) / 1000.0
        print(f"    расхождение хода часов машины и millis: {drift:+.1f} с")
        if a.all:
            for _, _, ms, odo, t in items:
                print(f"      {ms:>9}  {odo} км  {t + shift:%Y-%m-%d %H:%M:%S}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
