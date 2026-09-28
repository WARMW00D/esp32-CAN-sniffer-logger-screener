#!/usr/bin/env python3
"""
Декодер BAP Navigation_SD (LSG 0x32) из логов сниффера ESP32/TWAI.

Формат строки лога (как в прошивке сниффера):
    <millis> <S|X> <ID hex> [R]<DLC> <b0> <b1> ...
    123456 X 17333210 8 80 0A 4C 92 00 00 01 2C

Использование:
    python3 bap_nav_decode.py log.txt                 # только Navigation_SD
    python3 bap_nav_decode.py log.txt --all-bap       # все LSG (сырые заголовки)
    python3 bap_nav_decode.py log.txt --endian little # если расстояния выглядят бредом

Кадрирование BAP — по реверсу сообщества (revag-bap), не по официальному документу.
Поля функций — по Function Catalogue Navigation_SD v2.80.
Порядок байт многобайтных чисел (Long) не подтверждён -> см. сводку в конце вывода.
"""
import argparse
import re
import sys
from collections import defaultdict

BAP_BASE = 0x17330000          # 0x1733 LL xx : LL = LSG ID, xx: 0n = ASG, 1n = FSG
NAV_LSG = 0x32

FCT_NAMES = {
    0x01: "GetAll", 0x02: "BAP_Config", 0x03: "FunctionList", 0x04: "HeartBeat",
    0x0F: "FSG_OperationState", 0x10: "CompassInfo", 0x11: "RG_Status",
    0x12: "DistanceToNextManeuver", 0x13: "CurrentPositionInfo", 0x14: "TurnToInfo",
    0x15: "DistanceToDestination", 0x16: "TimeToDestination", 0x17: "ManeuverDescriptor",
    0x18: "LaneGuidance", 0x19: "TMCinfo", 0x24: "VoiceGuidance", 0x26: "InfoStates",
    0x27: "ActiveRgType", 0x2E: "DestinationInfo", 0x2F: "Altitude",
    0x37: "ManeuverState", 0x3C: "DistanceToDestinationExtended", 0x3D: "LaneGuidance2",
}

UNITS = {0: "m", 1: "km", 2: "yd", 3: "ft", 4: "mi", 5: "1/4mi", 0xFF: "n/a"}

MAIN_ELEMENT = {
    0x00: "NoSymbol", 0x01: "NoInfo", 0x02: "DirectionToDestination", 0x03: "Arrived",
    0x04: "NearDestination", 0x05: "ArrivedDestinationOffmap", 0x06: "OffRoad",
    0x07: "OffMap", 0x08: "NoRoute", 0x09: "CalcRoute", 0x0A: "RecalcRoute",
    0x0B: "FollowStreet", 0x0C: "ChangeLane", 0x0D: "Turn", 0x0E: "TurnOnMainroad",
    0x0F: "ExitRight", 0x10: "ExitLeft", 0x11: "ServiceRoadRight", 0x12: "ServiceRoadLeft",
    0x13: "Fork-2", 0x14: "Fork-3", 0x15: "RoundaboutTrsRight", 0x16: "RoundaboutTrsLeft",
    0x17: "SquareTrsRight", 0x18: "SquareTrsLeft", 0x19: "Uturn",
    0x1A: "ExitRoundaboutTrsRight", 0x1B: "ExitRoundaboutTrsLeft", 0x1C: "PrepareTurn",
    0x1D: "PrepareRoundabout", 0x1E: "PrepareSquare", 0x1F: "PrepareUTurn",
    0x22: "MichiganTurn", 0x23: "DoubleTurn", 0x26: "DirectionToWaypoint",
    0x27: "ReducedGuidance", 0x28: "LeaveParking", 0x29: "MainRoadRight",
    0x2A: "MainRoadLeft", 0x2B: "DirectionToEntrypoint", 0x2C: "TakeFerry",
    0x2D: "TakeCarTrain", 0x2E: "UnderpassRight", 0x2F: "UnderpassLeft",
    0x30: "OverpassRight", 0x31: "OverpassLeft", 0x32: "TollGate", 0x33: "TollGate-ETC",
}

LINE_RE = re.compile(r"^\s*(\d+)\s+([SX])\s+([0-9A-Fa-f]+)\s+(R?)(\d)\s*((?:[0-9A-Fa-f]{2}\s*)*)$")


def u32(b, endian):
    return int.from_bytes(bytes(b[:4]), endian)


def direction_str(d):
    deg = d * 360 / 256
    word = ["прямо", "налево", "назад", "направо"][round(deg / 90) % 4]
    return f"{deg:.0f}°({word})"


def bap_string(p, i):
    """1 байт длины + UTF-8. Возвращает (строка, новый индекс)."""
    if i >= len(p):
        return "", i
    n = p[i]
    s = bytes(p[i + 1:i + 1 + n]).decode("utf-8", errors="replace")
    return s, i + 1 + n


class Stats:
    def __init__(self):
        self.dist = defaultdict(list)   # fct -> [(be, le)]


def decode_nav(fct, p, endian, stats):
    try:
        if fct == 0x11 and len(p) >= 1:
            return {0: "RG не активно", 1: "RG активно", 2: "RG приостановлено"}.get(p[0], f"0x{p[0]:02X}")

        if fct == 0x12 and len(p) >= 8:
            stats.dist[fct].append((u32(p, "big"), u32(p, "little")))
            dist = u32(p, endian) / 10
            valid = "OK" if p[7] & 1 else "invalid"
            bar = f"{p[6]}%" if p[5] == 1 else "off"
            return f"до манёвра {dist:g} {UNITS.get(p[4], p[4])}, бар {bar}, {valid}"

        if fct == 0x15 and len(p) >= 6:
            stats.dist[fct].append((u32(p, "big"), u32(p, "little")))
            dist = u32(p, endian) / 10
            kind = "до промежуточной" if (p[5] >> 4) & 1 else "до цели"
            valid = "OK" if p[5] & 1 else "invalid"
            return f"{kind} {dist:g} {UNITS.get(p[4], p[4])}, {valid}"

        if fct == 0x16 and len(p) >= 7:
            ttype = {0: "в пути", 1: "прибытие", 2: "прибытие (др. пояс)"}.get(p[0] >> 4, f"тип{p[0] >> 4}")
            mi, hr, v = p[1], p[2], p[6]
            s = f"{ttype} {hr if v & 4 else '--'}:{mi:02d}" if v & 2 else f"{ttype} --:--"
            if p[0] >> 4 in (1, 2) and v & 0x38:
                s += f"  ({p[3]:02d}.{p[4]:02d}.{2000 + p[5]})"
            return s

        if fct == 0x17:
            out, i = [], 0
            for n in range(3):
                if i + 3 > len(p):
                    break
                me, d, z = p[i], p[i + 1], p[i + 2]
                i += 3
                side = []
                if i < len(p):
                    ln = p[i]
                    side = list(p[i + 1:i + 1 + ln])
                    i += 1 + ln
                zs = {1: " ↑", 2: " ↓"}.get(z, "")
                sd = f" боковые={['%.0f°' % (x * 360 / 256) for x in side]}" if side else ""
                out.append(f"M{n + 1}: {MAIN_ELEMENT.get(me, f'0x{me:02X}')} {direction_str(d)}{zs}{sd}")
            return " | ".join(out)

        if fct in (0x13, 0x14):
            s1, i = bap_string(p, 0)
            if fct == 0x14:
                s2, _ = bap_string(p, i)
                return f"улица='{s1}' направление='{s2}'"
            return f"позиция='{s1}'"
    except Exception as e:  # noqa
        return f"ошибка разбора: {e}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--endian", choices=["big", "little"], default="big")
    ap.add_argument("--all-bap", action="store_true", help="показывать все LSG, не только 0x32")
    ap.add_argument("--raw", action="store_true", help="печатать сырой payload")
    args = ap.parse_args()

    buffers = {}          # (can_id, channel) -> dict(hdr, need, data, t)
    stats = Stats()
    last = {}             # (lsg, fct, dir) -> последний текст, чтобы не спамить повторами

    def emit(t, can_id, hdr, payload):
        opcode = (hdr >> 12) & 0x7
        lsg = (hdr >> 6) & 0x3F
        fct = hdr & 0x3F
        direction = "FSG→" if (can_id & 0x10) else "ASG→"
        if lsg != NAV_LSG and not args.all_bap:
            return
        name = FCT_NAMES.get(fct, f"fct0x{fct:02X}") if lsg == NAV_LSG else f"fct0x{fct:02X}"
        text = decode_nav(fct, payload, args.endian, stats) if lsg == NAV_LSG else None
        raw = " ".join(f"{b:02X}" for b in payload)
        key = (lsg, fct, direction)
        line = text if text is not None else raw
        if last.get(key) == line and not args.raw:
            return
        last[key] = line
        extra = f"  [{raw}]" if args.raw and text is not None else ""
        print(f"{t:>10} {can_id:08X} {direction} LSG0x{lsg:02X} op{opcode} {name:<24} {line}{extra}")

    with open(args.log, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = LINE_RE.match(ln)
            if not m or m.group(2) != "X" or m.group(4):
                continue
            t = int(m.group(1))
            can_id = int(m.group(3), 16)
            if (can_id & 0xFFFF0000) != BAP_BASE:
                continue
            if not args.all_bap and ((can_id >> 8) & 0xFF) != NAV_LSG:
                continue
            data = [int(x, 16) for x in m.group(6).split()][:int(m.group(5))]
            if len(data) < 2:
                continue
            b0 = data[0]
            if not b0 & 0x80:                      # одиночный кадр
                emit(t, can_id, (data[0] << 8) | data[1], data[2:])
                continue
            ch = (b0 >> 4) & 0x3
            key = (can_id, ch)
            if not b0 & 0x40:                      # старт сегментированного сообщения
                if len(data) < 4:
                    continue
                need = ((b0 & 0x0F) << 8) | data[1]
                buffers[key] = {"hdr": (data[2] << 8) | data[3], "need": need,
                                "data": data[4:], "t": t, "seq": 0}
            else:                                  # продолжение
                buf = buffers.get(key)
                if not buf:
                    continue
                seq = b0 & 0x0F
                if seq != (buf["seq"] & 0x0F):
                    # потерян кадр — выбрасываем сообщение
                    del buffers[key]
                    continue
                buf["seq"] += 1
                buf["data"] += data[1:]
            buf = buffers.get(key)
            if buf and len(buf["data"]) >= buf["need"]:
                emit(buf["t"], can_id, buf["hdr"], buf["data"][:buf["need"]])
                del buffers[key]

    if stats.dist:
        print("\n=== Проверка порядка байт (Long) ===", file=sys.stderr)
        for fct, vals in stats.dist.items():
            be = [v[0] for v in vals]
            le = [v[1] for v in vals]
            print(f"{FCT_NAMES[fct]}: big  min={min(be)} max={max(be)}", file=sys.stderr)
            print(f"{' ' * len(FCT_NAMES[fct])}  little min={min(le)} max={max(le)}", file=sys.stderr)
        print("Правильный — тот, где значения разумные (x0.1) и плавно убывают при езде.",
              file=sys.stderr)


if __name__ == "__main__":
    main()
