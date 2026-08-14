#!/usr/bin/env python3
"""build_beatmap.py — deterministic song-position -> scene-tick beat-map builder.

Part of the chase music-sync foundation (Stage S0, docs/CHASE_UPGRADE_PLAN.md).
PLACEMENT-AGNOSTIC scaffolding: it takes an ARBITRARY song and an ARBITRARY
start position as parameters. Chase has no slot in the track yet (it never
shipped in 1998), so this does NOT answer "where chase sits" or its pacing —
that is future work once the user decides chase's music. This just produces the
infra: a table mapping musical position (order:row) to a scene-tick offset.

Scene-ticks are the demo's Timer unit = CENTISECONDS (10 ms). CHASE.CPP advances
Timer at 100 ticks/s and CurFrame = 1 + Timer, so a beat-map tick is directly a
Timer value the engine's ChaseEvents loader resolves against.

Why a SCAN and not a constant BPM: MOD/XM change tempo (BPM) and speed
(ticks/row) mid-song via the Fxx effect, so the ms-per-row is not constant. This
walks the pattern order from the start position, tracks (speed, bpm), applies
Fxx, and accumulates time row by row.

    tick_ms   = 2500 / bpm            # one tracker tick, standard XM/ProTracker
    row_ms    = speed * tick_ms       # `speed` ticks per row
    row_centi = row_ms / 10           # scene-ticks (centiseconds)

Coverage / honesty:
  * Handles Fxx set-speed (param < 0x20) and set-BPM (param >= 0x20) — the
    effects that change TIME.
  * Advances linearly through the order table (order 0..song_length within the
    chosen range), row by row. Dxx pattern-break / Bxx position-jump REORDER
    playback; they are reported (--warn-jumps) but NOT followed — for a
    placement-agnostic tick map that is a documented simplification. When
    chase's real placement is chosen, validate against Modplayer_GetPosition
    (Stage S0.1) which reflects true playback advance including jumps.
  * EEx pattern-delay (row-repeat) is not modelled.

Deterministic: output is a pure function of the file bytes + the CLI range.

Usage:
    tools/build_beatmap.py SONG.XM [--start-order N] [--start-row N]
        [--orders N] [--out FILE]

Output format (see also DEMO/ChaseEvents.cpp Beatmap_Load):
    # comment lines start with '#'
    # columns: order row centitick bpm speed
    0 0 0 125 6
    0 1 3 125 6
    ...
"""
import argparse
import struct
import sys


def read_xm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:17] != b"Extended Module: ":
        raise ValueError("not an XM file (bad ID header): %r" % data[:17])
    header_size = struct.unpack_from("<I", data, 60)[0]
    (song_length, restart, num_channels, num_patterns, num_instruments,
     flags, default_speed, default_bpm) = struct.unpack_from("<HHHHHHHH", data, 64)
    order_table = list(data[80:80 + 256])[:song_length]

    # Patterns follow the header (which starts at offset 60).
    patterns = []
    off = 60 + header_size
    for _ in range(num_patterns):
        pat_hdr_len, packing, rows, packed_size = struct.unpack_from("<IBHH", data, off)
        pdata = data[off + pat_hdr_len: off + pat_hdr_len + packed_size]
        patterns.append((rows, _unpack_pattern(pdata, rows, num_channels)))
        off += pat_hdr_len + packed_size

    return {
        "name": data[17:37].rstrip(b"\x00\x20").decode("latin1", "replace"),
        "song_length": song_length,
        "num_channels": num_channels,
        "num_patterns": num_patterns,
        "default_speed": default_speed,
        "default_bpm": default_bpm,
        "order_table": order_table,
        "patterns": patterns,
    }


def _unpack_pattern(pdata, rows, channels):
    """Return rows x channels list of (effect_type, effect_param)."""
    out = [[(0, 0)] * channels for _ in range(rows)]
    if not pdata:
        return out  # empty pattern = all-empty rows
    i = 0
    n = len(pdata)
    for r in range(rows):
        for c in range(channels):
            if i >= n:
                break
            b = pdata[i]; i += 1
            if b & 0x80:
                if b & 0x01: i += 1               # note
                if b & 0x02: i += 1               # instrument
                if b & 0x04: i += 1               # volume
                eff = 0
                if b & 0x08:
                    eff = pdata[i]; i += 1        # effect type
                par = 0
                if b & 0x10:
                    par = pdata[i]; i += 1        # effect param
            else:
                # b is the note; next 4 bytes are inst, vol, eff, par
                i += 2                            # instrument, volume
                eff = pdata[i]; i += 1
                par = pdata[i]; i += 1
            out[r][c] = (eff, par)
    return out


def scan(song, start_order, start_row, max_orders, warn_jumps):
    speed = song["default_speed"] or 6
    bpm = song["default_bpm"] or 125
    rows_seen = []
    centi = 0.0
    end_order = song["song_length"]
    if max_orders:
        end_order = min(end_order, start_order + max_orders)

    for order in range(start_order, end_order):
        pat_idx = song["order_table"][order]
        if pat_idx >= song["num_patterns"]:
            continue
        rows, pat = song["patterns"][pat_idx]
        r0 = start_row if order == start_order else 0
        for row in range(r0, rows):
            rows_seen.append((order, row, int(round(centi)), bpm, speed))
            # Apply this row's tempo/speed effects (last channel wins, as
            # trackers apply left-to-right within a row).
            for (eff, par) in pat[row]:
                if eff == 0x0F:                   # Fxx: set speed / BPM
                    if par < 0x20:
                        if par > 0:
                            speed = par
                    else:
                        bpm = par
                elif warn_jumps and eff in (0x0B, 0x0D):
                    sys.stderr.write(
                        "[beatmap] note: order %d row %d has a %s effect "
                        "(param %02X) — playback would jump; scan advances "
                        "linearly (documented simplification)\n"
                        % (order, row, "Bxx" if eff == 0x0B else "Dxx", par))
            tick_ms = 2500.0 / bpm
            centi += (speed * tick_ms) / 10.0
    return rows_seen, centi


def main():
    ap = argparse.ArgumentParser(description="song position -> scene-tick beat-map")
    ap.add_argument("song")
    ap.add_argument("--start-order", type=int, default=0)
    ap.add_argument("--start-row", type=int, default=0)
    ap.add_argument("--orders", type=int, default=0,
                    help="limit the scan to N orders from start (0 = to song end)")
    ap.add_argument("--out", default="-", help="output file ('-' = stdout)")
    ap.add_argument("--warn-jumps", action="store_true",
                    help="note Bxx/Dxx position effects on stderr")
    args = ap.parse_args()

    song = read_xm(args.song)
    rows_seen, total_centi = scan(song, args.start_order, args.start_row,
                                  args.orders, args.warn_jumps)

    lines = []
    lines.append("# chase beatmap v1  (Stage S0 scaffolding — placement-agnostic)")
    lines.append("# song=%s  channels=%d  orders=%d  patterns=%d"
                 % (song["name"] or "?", song["num_channels"],
                    song["song_length"], song["num_patterns"]))
    lines.append("# start_order=%d start_row=%d  default_speed=%d default_bpm=%d"
                 % (args.start_order, args.start_row,
                    song["default_speed"], song["default_bpm"]))
    lines.append("# tick unit = centiseconds (demo Timer, 100/s; CurFrame=1+tick)")
    lines.append("# scanned %d rows, span %.0f centiticks (%.1f s)"
                 % (len(rows_seen), total_centi, total_centi / 100.0))
    lines.append("# columns: order row centitick bpm speed")
    for (order, row, tick, bpm, speed) in rows_seen:
        lines.append("%d %d %d %d %d" % (order, row, tick, bpm, speed))
    text = "\n".join(lines) + "\n"

    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w") as f:
            f.write(text)
        sys.stderr.write("[beatmap] wrote %s (%d rows, %.1f s)\n"
                         % (args.out, len(rows_seen), total_centi / 100.0))


if __name__ == "__main__":
    main()
