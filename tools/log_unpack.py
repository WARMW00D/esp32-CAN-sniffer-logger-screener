#!/usr/bin/env python3
"""
log_unpack.py — распаковать сжатые логи сниффера (can_log_NNNN.txt.gz) в .txt.

Обычный gzip/7-Zip тоже справится, но этот скрипт:
  - распаковывает оборванные файлы (без штатного закрытия) до места обрыва,
    не останавливаясь с ошибкой;
  - по желанию склеивает файлы одной папки в один текст по порядку имён
    (удобно для bap_nav_decode.py и других разборщиков).

Использование:
    python log_unpack.py путь [путь ...]            # файлы .gz или папки
    python log_unpack.py 2026-09-28 --join          # склеить всё из папки
    python log_unpack.py can_logs.tar               # сначала распакуйте TAR
"""
import argparse
import os
import sys
import zlib


def unpack(path):
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    out = bytearray()
    ok = True
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 16)
            if not chunk:
                break
            try:
                out += d.decompress(chunk)
            except zlib.error:
                ok = False
                break
    complete = ok and d.eof
    return bytes(out), complete


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--join", action="store_true", help="склеить файлы каждой папки в <папка>/all.txt")
    a = ap.parse_args()

    groups = {}
    for p in a.paths:
        if os.path.isdir(p):
            files = sorted(f for f in os.listdir(p) if f.endswith(".txt.gz"))
            groups[p] = [os.path.join(p, f) for f in files]
        else:
            groups.setdefault(os.path.dirname(p) or ".", []).append(p)

    for folder, files in groups.items():
        joined = []
        for fp in files:
            data, complete = unpack(fp)
            status = "OK" if complete else "оборван — распаковано до места обрыва"
            print(f"{fp}: {len(data)} байт, {status}")
            if a.join:
                joined.append(data)
            else:
                with open(fp[:-3], "wb") as o:      # can_log_0001.txt.gz -> can_log_0001.txt
                    o.write(data)
        if a.join and joined:
            dst = os.path.join(folder, "all.txt")
            with open(dst, "wb") as o:
                for d in joined:
                    o.write(d)
            print(f"-> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
