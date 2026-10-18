#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The clock's modulation: in host mode the duck on each beat (140, 174, 200 BPM) and none after Stop; through
tests/fx_unit.c the LFOs' periods and phase from Start, and the duck's shape, release and rate.
Run from anywhere: python3 tests/mod_test.py"""
import array
import math
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "host" / "mod"
BIN = ROOT / "build" / "host" / "ethereal"
FS = 44100
failed = []
PATCH = {"MIST": 0, "GLASS": 1, "HALO": 2, "KEYS": 3}


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", BIN, "firmware/src/ethereal.c"], cwd=ROOT, check=True)


def run(name, events, end, patch="MIST"):
    ev = ([(10, f"enc 6 {PATCH[patch]}")] if PATCH[patch] else []) + events
    script = OUT / f"{name}.txt"
    script.write_text("".join(f"{t} {e}\n" for t, e in sorted(ev, key=lambda x: x[0])) + f"{end} end\n")
    subprocess.run([BIN, OUT / name, script], capture_output=True, text=True, check=True)
    w = wave.open(str(OUT / f"{name}.wav"))
    return array.array("h", w.readframes(w.getnframes()))[0::2]


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def clock(bpm, t0, t1, start=None):
    """ticks from t0 to t1 ms; Start just before the tick at `start` (that tick is the downbeat)"""
    p = 60000 / bpm / 24
    ev = [(round(t0 + i * p), "midi 0xF8 0 0") for i in range(int((t1 - t0) / p))]
    if start is not None:
        ev.append((start - 1, "midi 0xFA 0 0"))
    return ev


def env(x, win):
    """rms in windows of `win` frames"""
    return [math.sqrt(sum(v * v for v in x[i:i + win]) / win) for i in range(0, len(x) - win, win)]


QUIET_LFO = (60, "midi 0xB0 1 0")              # LFO 1 off: the cutoff's swing would hide what is measured


def downbeat(bpm, t0, start):
    """the ms of the first tick at or after Start (as clock() schedules them)"""
    p = 60000 / bpm / 24
    return next(round(t0 + i * p) for i in range(10000) if round(t0 + i * p) >= start)


def dips(e, t0, t1):
    """ms (1 ms windows) where the level falls by over 3/4 within 4 ms: a duck, not the oscillators' slow beating"""
    out = []
    for i in range(t0, t1):
        if e[i + 4] < 0.25 * e[i] and (not out or i - out[-1] > 20):
            out.append(i)
    return out


def duck():
    """one held note on MIST, CC 94 full, against the same without the duck (their ratio is the duck's gain alone):
    it falls at each beat, to under a tenth"""
    for bpm in (140, 174, 200):
        beat = 60000 / bpm
        ev = clock(bpm, 200, 8000, 1000) + [QUIET_LFO, (100, "midi 0x90 60 100")]
        dry = run(f"duck-{bpm}-off", ev, 8000)
        wet = run(f"duck-{bpm}", ev + [(100, "midi 0xB0 94 127")], 8000)
        g = [w / d if d else 1 for w, d in zip(env(wet, 11), env(dry, 11))]
        d0 = downbeat(bpm, 200, 1000)
        late, low = [], []
        ms = 11 * 1000 / FS                     # a window
        for b in range(1, 17):
            t = d0 + b * beat
            i0 = round(t / ms)
            late.append(next(i for i in range(i0 - 80, i0 + 80) if g[i] < 0.5) * ms - t)
            low.append(min(g[i0:i0 + 80]))
        check(f"duck-{bpm}", all(-6 <= t <= 6 for t in late) and max(late) - min(late) <= 2,
              f"half way down {min(late):+.1f}..{max(late):+.1f} ms from each beat (16 beats)")
        check(f"duck-{bpm}", max(low) < 0.1, f"down to {max(low):.2f}")


def duck_stop():
    """after Stop the clock still runs (as a sequencer's does): no more ducks"""
    ev = clock(174, 200, 6000, 1000) + [QUIET_LFO, (3000, "midi 0xFC 0 0"), (100, "midi 0xB0 94 127"),
                                         (100, "midi 0x90 60 100")]
    e = env(run("duck-stop", ev, 6000), 44)
    before, after = dips(e, 1500, 2900), dips(e, 3100, 5900)
    check("duck-stop", len(before) >= 3 and not after, f"{len(before)} ducks in the 1.4 s before Stop, {len(after)} after")


def unit(*args):
    out = subprocess.run([ROOT / "build" / "host" / "fx_unit", *map(str, args)], capture_output=True, text=True,
                         check=True).stdout
    return [list(map(int, ln.split())) for ln in out.splitlines() if "ms" not in ln]


def cross_up(rows, col, t_from=0):
    """frames where a column crosses its middle going up"""
    mid = (max(r[col] for r in rows) + min(r[col] for r in rows)) / 2
    return [b[0] for a, b in zip(rows, rows[1:]) if a[col] < mid <= b[col] and b[0] >= t_from]


def lfos():
    """the LFOs straight from mod_block at 174 BPM: periods from the bar, phase from Start"""
    bar = 4 * 60 * FS / 174
    for patch, name, col, per in ((0, "MIST LFO 1 (2 bars)", 1, 2 * bar), (2, "HALO LFO 1 (4 bars)", 1, 4 * bar),
                                  (1, "GLASS LFO 2 (1/16)", 2, bar / 16), (2, "HALO LFO 2 (1/8T)", 2, bar / 12),
                                  (3, "KEYS LFO 2 (1/8)", 2, bar / 8)):
        rows = unit("mod", patch, 174, 0)
        ups = cross_up(rows, col)
        got = (ups[-1] - ups[0]) / (len(ups) - 1)
        check("lfo", abs(got - per) < 0.01 * per, f"{name}: a cycle in {got:.0f} frames ({per:.0f})")
        phase = ((ups[0] + 256) % per) / per   # (made a block ahead)
        check("lfo", min(phase, 1 - phase) < 0.02, f"{name}: in step with Start, rising at it ({phase:.2f} of a cycle)")


def duck_unit():
    """the duck's gain: down at each beat in 2.9 ms, back by 40 dB after its release; every other beat; not stopped"""
    beat = 60 * FS / 174
    rows = unit("mod", 0, 174, 1)
    lows = [i for i in range(1, len(rows) - 1) if rows[i][3] < rows[i - 1][3] and rows[i][3] <= rows[i + 1][3]]
    t_low = [rows[i][0] for i in lows]
    gaps = [b - a for a, b in zip(t_low[1:], t_low[2:])]   # (the downbeat's comes a block late: no time to lead it)
    check("duck", len(t_low) >= 60 and all(abs(g - beat) < 40 for g in gaps), f"{len(t_low)} ducks, a beat apart ({min(gaps)}..{max(gaps)} frames, {beat:.0f})")
    check("duck", t_low[0] <= 5 * 32, f"the first at the downbeat (its low at frame {t_low[0]})")
    check("duck", max(rows[i][3] for i in lows) < 400, f"down to {max(rows[i][3] for i in lows)} of 32768 at full depth")
    rel = 30 + 90 * 270 // 255
    i = lows[4]
    back = next(r[0] for r in rows[i:] if 32768 - r[3] < 328) - rows[i][0]
    check("duck", abs(back - rel * 44.1) < 0.15 * rel * 44.1, f"99 % back {back / 44.1:.0f} ms after the low ({rel} ms set)")
    sub = rows[i][4]
    check("duck", abs((32768 - sub) - (32768 - rows[i][3]) / 2) < 400, f"the sub, at half share, down to {sub}")
    rows2 = unit("mod", 0, 174, 1, 2, 0)
    lows2 = [rows2[i][0] for i in range(1, len(rows2) - 1) if rows2[i][3] < rows2[i - 1][3] and rows2[i][3] <= rows2[i + 1][3]]
    gaps2 = [b - a for a, b in zip(lows2[1:], lows2[2:])]
    check("duck", all(abs(g - 2 * beat) < 40 for g in gaps2), f"every 2 beats: {min(gaps2)}..{max(gaps2)} frames apart ({2 * beat:.0f})")
    rows3 = unit("mod", 0, 174, 1, 1, 1)
    i3 = [i for i in range(1, len(rows3) - 1) if rows3[i][3] < rows3[i - 1][3] and rows3[i][3] <= rows3[i + 1][3]][4]
    back3 = next(r[0] for r in rows3[i3:] if 32768 - r[3] < 328) - rows3[i3][0]
    check("duck", abs(back3 - beat / 4) < 0.15 * beat / 4, f"release on the 1/16: back in {back3} frames ({beat / 4:.0f})")
    stopped = unit("mod", 0, 174, 0)
    check("duck", all(r[3] == 32768 for r in stopped[8:]), "transport stopped: no duck")


if __name__ == "__main__":
    build()
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", ROOT / "build" / "host" / "fx_unit", "tests/fx_unit.c"],
                   cwd=ROOT, check=True)
    duck()
    duck_stop()
    duck_unit()
    lfos()
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
