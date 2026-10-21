#!/usr/bin/env python3
"""
log_index.py — индекс логов сниффера: в какое время начал писаться каждый файл.

В строках лога только millis от включения; реальное время берётся из кадра 0x6B2
(дата/время приборки, как в can_time_decode.py). Скрипт обходит папки с логами
(can_log_NNNN.txt / .txt.gz / .txt.lzma, в т.ч. оборванные), для каждого файла
определяет время начала и конца и пишет два файла:

    log_index.md   — читаемый индекс, сгруппированный по сессиям (включениям зажигания)
    log_index.csv  — то же таблицей (Excel, LibreOffice)

Если в файле нет кадров 0x6B2 (например, короткий файл-продолжение), время берётся
по предыдущему файлу той же сессии: millis при CONTINUED не сбрасывается. Если привязки нет
совсем — пишется только дата папки и пометка «время неизвестно».

Использование:
    python log_index.py D:\\SD                 # корень карты (папки ГГГГ-ММ-ДД) или любая папка с логами
    python log_index.py 2026-10-08 2026-10-09  # несколько папок
    python log_index.py D:\\SD --tz 3 --out D:\\Проекты\\индекс

    --tz N    сдвиг в часах, если машина шлёт UTC (для Москвы: --tz 3)
    --out DIR куда писать log_index.md/.csv (по умолчанию — первая указанная папка)
"""
import argparse
import csv
import datetime as dt
import os
import re
import statistics
import sys
from collections import deque

from can_time_decode import decode_6b2, read_lines      # та же раскладка 0x6B2 и тот же распаковщик

EXTS = (".txt", ".txt.gz", ".txt.lzma", ".txt.xz")
NAME_RE = re.compile(r"^can_log_(\d+)\.txt(\.gz|\.lzma|\.xz)?$", re.I)
YEAR_MIN, YEAR_MAX = 2020, 2045          # вне диапазона — мусор в кадре, не время
HEADER_FIELDS = ("fw", "can_bps", "rtc")


class FileInfo:
    def __init__(self, folder, name, path):
        self.folder, self.name, self.path = folder, name, path
        self.size = os.path.getsize(path)
        self.frames = 0
        self.first_ms = self.last_ms = None
        self.kind = "—"                    # BOOT / CONTINUED / ?
        self.header = {}                   # fw, can_bps, rtc, time (источник времени в прошивке)
        self.markers = []                  # все '#'-маркеры (краткие)
        self.dropped = None
        self.closed = False                # есть завершающий маркер
        self.pauses = 0
        self.first_bases = []              # (t - ms) в секундах, первые кадры 0x6B2
        self.last_bases = deque(maxlen=7)  # ...и последние
        self.n6b2 = 0
        self.odo_first = self.odo_last = None
        self.session = 0
        self.start = self.end = None
        self.how = "нет"
        self.notes = []


def scan(fi):
    for line in read_lines(fi.path):
        line = line.strip()
        if not line:
            continue
        if line[0] == "#":
            m = re.search(r"=====\s*(.*?)\s*=====", line)
            tag = m.group(1) if m else line[:40]
            word = tag.split()[0] if tag else ""
            if word == "BOOT" and fi.kind == "—":
                fi.kind = "BOOT"
                for k in HEADER_FIELDS:
                    mm = re.search(r"\b%s=(\S+)" % k, line)
                    if mm:
                        fi.header[k] = mm.group(1)
                mm = re.search(r'\btsrc=(\S+)', line)          # fw >= 2.10.2
                if mm:
                    fi.header["time"] = mm.group(1)
                else:
                    mm = re.search(r'\btime="([^"]*)"', line)   # старый формат: источник времени
                    if mm:
                        fi.header["time"] = mm.group(1)
                mm = re.search(r'\bTime="([^"]*)"', line)       # fw >= 2.10.2: текущее время
                if mm:
                    fi.header["clock"] = mm.group(1)
                    mb = re.match(r"#\s*(\d+)", line)
                    if mb:
                        fi.header["boot_ms"] = mb.group(1)
            elif word == "CONTINUED" and fi.kind == "—":
                fi.kind = "CONTINUED"
            elif word in ("ROTATE", "ACC", "OTA", "CONFIG"):
                fi.closed = True
            elif word == "PAUSE":
                fi.closed = True
                fi.pauses += 1
            mm = re.search(r"dropped=(\d+)", line)
            if mm:
                fi.dropped = int(mm.group(1))
            fi.markers.append(tag)
            continue
        sp = line.split(None, 1)
        try:
            ms = int(sp[0])
        except ValueError:
            continue
        fi.frames += 1
        if fi.first_ms is None:
            fi.first_ms = ms
        fi.last_ms = ms
        if " S 6B2 " in line:
            p = line.split()
            try:
                if p[3].startswith("R"):
                    continue
                data = bytes(int(x, 16) for x in p[4:4 + int(p[3])])
            except (ValueError, IndexError):
                continue
            r = decode_6b2(data)
            if not r or not (YEAR_MIN <= r[1].year <= YEAR_MAX):
                continue
            base = r[1].timestamp() - ms / 1000.0           # время машины в момент millis = 0
            fi.n6b2 += 1
            if len(fi.first_bases) < 7:
                fi.first_bases.append(base)
            fi.last_bases.append(base)
            if fi.odo_first is None:
                fi.odo_first = r[0]
            fi.odo_last = r[0]


def collect(paths):
    """-> список FileInfo в порядке (папка, номер файла); /no-rtc отдельной группой в конце."""
    found = []
    for p in paths:
        if os.path.isfile(p):
            found.append((os.path.basename(os.path.dirname(os.path.abspath(p))), os.path.basename(p), p))
            continue
        for root, _, files in os.walk(p):
            for f in files:
                if NAME_RE.match(f):
                    found.append((os.path.basename(root) or root, f, os.path.join(root, f)))
    def key(t):
        folder, name, _ = t
        m = NAME_RE.match(name)
        return (folder == "no-rtc", folder, int(m.group(1)) if m else 0, name)
    return [FileInfo(*t) for t in sorted(set(found), key=key)]


def fmt(t):
    if not t:
        return "—"
    t = (t + dt.timedelta(milliseconds=500)).replace(microsecond=0)      # округление до секунды
    return t.strftime("%Y-%m-%d %H:%M:%S")


def resolve(files, shift):
    """Время начала/конца каждого файла и сквозные номера сессий."""
    sess = 0
    base = None                              # привязка текущей сессии: epoch-секунды при millis = 0
    prev_last = None
    for fi in files:
        if fi.kind == "BOOT" or fi.kind == "—" or fi.first_ms is None or (
                prev_last is not None and fi.first_ms < prev_last - 1000):
            sess += 1                        # новое включение (millis пошёл заново)
            base = None
        fi.session = sess
        if fi.first_ms is None:
            fi.notes.append("нет кадров")
            continue
        prev_last = fi.last_ms
        if fi.first_bases:
            b0 = statistics.median(fi.first_bases)
            b1 = statistics.median(fi.last_bases)
            fi.start = dt.datetime.fromtimestamp(b0 + fi.first_ms / 1000.0) + shift
            fi.end = dt.datetime.fromtimestamp(b1 + fi.last_ms / 1000.0) + shift
            fi.how = "кадр 0x6B2"
            if abs(b1 - b0) > 5:
                fi.notes.append("время машины и millis разошлись на %.0f с" % abs(b1 - b0))
            base = b0
        elif base is not None:
            fi.start = dt.datetime.fromtimestamp(base + fi.first_ms / 1000.0) + shift
            fi.end = dt.datetime.fromtimestamp(base + fi.last_ms / 1000.0) + shift
            fi.how = "по предыдущему файлу"
        elif fi.header.get("clock") and re.match(r"\d{4}-", fi.header["clock"]):
            t0 = dt.datetime.strptime(fi.header["clock"], "%Y-%m-%d %H:%M:%S")
            off = (fi.first_ms - int(fi.header.get("boot_ms", fi.first_ms))) / 1000.0
            fi.start = t0 + dt.timedelta(seconds=off)
            fi.end = fi.start + dt.timedelta(seconds=(fi.last_ms - fi.first_ms) / 1000.0)
            fi.how = "Time= из BOOT"
        else:
            fi.notes.append("время неизвестно (нет кадров 0x6B2)")
        if not fi.closed:
            fi.notes.append("не закрыт штатно (обрыв питания?)")
        if fi.folder == "no-rtc":
            fi.notes.append("папка no-rtc: прошивка не знала время при открытии")
        elif fi.start and re.match(r"\d{4}-\d{2}-\d{2}$", fi.folder):
            if abs((fi.start.date() - dt.date.fromisoformat(fi.folder)).days) > 1:
                fi.notes.append("дата папки %s не совпадает со временем машины" % fi.folder)
        if fi.pauses:
            fi.notes.append("пауза портала ×%d" % fi.pauses)
        if fi.dropped:
            fi.notes.append("потеряно кадров CAN: %d" % fi.dropped)
    # вторая проходка: если первые файлы сессии без привязки, а следующие с ней — назад ничего не тянем


def dur(a, b):
    if not (a and b):
        return "—"
    s = int(round((b - a).total_seconds()))
    return "%d:%02d:%02d" % (s // 3600, s % 3600 // 60, s % 60)


def write_csv(files, path):
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f, delimiter=";")
        w.writerow(["сессия", "папка", "файл", "размер_КБ", "тип", "начало", "конец", "длительность",
                    "кадров", "пробег_начало_км", "пробег_конец_км", "как_определено_время",
                    "прошивка", "источник_времени_в_прошивке", "замечания"])
        for fi in files:
            w.writerow([fi.session, fi.folder, fi.name, round(fi.size / 1024), fi.kind, fmt(fi.start), fmt(fi.end),
                        dur(fi.start, fi.end), fi.frames, fi.odo_first if fi.odo_first is not None else "",
                        fi.odo_last if fi.odo_last is not None else "", fi.how, fi.header.get("fw", ""),
                        fi.header.get("time", ""), "; ".join(fi.notes)])


def write_md(files, path, tz):
    out = ["# Индекс логов сниффера", "",
           "Время начала взято из кадров 0x6B2 (часы приборки)%s. «Сессия» — одно включение зажигания: "
           "файлы `CONTINUED` продолжают её без сброса millis." % (", сдвиг %+g ч" % tz if tz else ""), ""]
    cur = None
    for fi in files:
        if fi.session != cur:
            cur = fi.session
            grp = [x for x in files if x.session == cur]
            starts = [x.start for x in grp if x.start]
            ends = [x.end for x in grp if x.end]
            out += ["", "## Сессия %d: %s — %s (%s, файлов: %d)" % (
                cur, fmt(min(starts)) if starts else "время неизвестно", fmt(max(ends)) if ends else "—",
                dur(min(starts), max(ends)) if starts and ends else "—", len(grp)), "",
                    "| Файл | Тип | Начало | Конец | Длит. | Кадров | Пробег, км | Замечания |",
                    "|---|---|---|---|---|---|---|---|"]
        odo = "" if fi.odo_first is None else ("%d" % fi.odo_first if fi.odo_first == fi.odo_last
                                                else "%d → %d" % (fi.odo_first, fi.odo_last))
        src = "" if fi.how == "кадр 0x6B2" else (" (%s)" % fi.how if fi.start else "")
        out.append("| %s/%s | %s | %s%s | %s | %s | %d | %s | %s |" % (
            fi.folder, fi.name, fi.kind, fmt(fi.start), src, fmt(fi.end), dur(fi.start, fi.end),
            fi.frames, odo, "; ".join(fi.notes)))
    out += ["", "Файлов: %d. Если «время неизвестно» — в логе не было кадра 0x6B2 (нет трафика или другая шина), "
            "тогда ориентируйтесь на папку с датой." % len(files), ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="папки с логами (или корень SD) и/или файлы")
    ap.add_argument("--tz", type=float, default=0.0, help="сдвиг в часах (машина шлёт UTC)")
    ap.add_argument("--out", help="куда писать log_index.md/.csv")
    a = ap.parse_args()

    files = collect(a.paths)
    if not files:
        print("Логов can_log_*.txt[.lzma|.gz] не найдено.")
        return 1
    for i, fi in enumerate(files, 1):
        print("[%d/%d] %s/%s" % (i, len(files), fi.folder, fi.name), file=sys.stderr)
        try:
            scan(fi)
        except Exception as e:                                  # битый файл не должен ронять индекс
            fi.notes.append("ошибка чтения: %s" % e)
    resolve(files, dt.timedelta(hours=a.tz))

    outdir = a.out or (a.paths[0] if os.path.isdir(a.paths[0]) else os.path.dirname(os.path.abspath(a.paths[0])))
    os.makedirs(outdir, exist_ok=True)
    md, cs = os.path.join(outdir, "log_index.md"), os.path.join(outdir, "log_index.csv")
    write_md(files, md, a.tz)
    write_csv(files, cs)
    print("Готово: %s, %s" % (md, cs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
