#!/usr/bin/env python3
"""
log_unpack.py — распаковать сжатые логи сниффера (can_log_NNNN.txt.lzma / .txt.gz) в .txt.

7-Zip / FAR тоже справятся, но этот скрипт:
  - распаковывает оборванные файлы (без штатного закрытия) до места обрыва,
    не останавливаясь с ошибкой;
  - по желанию склеивает файлы одной папки в один текст по порядку имён
    (удобно для bap_nav_decode.py и других разборщиков).

Использование:
    python log_unpack.py путь [путь ...]            # файлы .lzma/.gz или папки
    python log_unpack.py 2026-09-28 --join          # склеить всё из папки
    python log_unpack.py can_logs.tar               # сначала распакуйте TAR
"""
import argparse
import os
import sys
import lzma
import zlib


EXTS = (".txt.lzma", ".txt.gz", ".txt.xz")


def unpack(path):
    low = path.lower()
    if low.endswith(".gz"):
        d, errors = zlib.decompressobj(16 + zlib.MAX_WBITS), (zlib.error,)
    elif low.endswith(".xz"):
        d, errors = lzma.LZMADecompressor(format=lzma.FORMAT_XZ), (lzma.LZMAError, EOFError)
    else:
        d, errors = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE), (lzma.LZMAError, EOFError)
    out = bytearray()
    ok = True
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 16)
            if not chunk:
                break
            try:
                out += d.decompress(chunk)
            except errors:
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
            files = sorted(f for f in os.listdir(p) if f.lower().endswith(EXTS))
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
                dst = fp[:fp.lower().rindex(".txt") + 4]      # can_log_0001.txt.lzma -> can_log_0001.txt
                with open(dst, "wb") as o:
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
