#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The pad FX in host mode: chorus, delay, reverb and the master clip. Builds build/host/ethereal, writes a script per
case, reads its WAV. Run from anywhere: python3 tests/fx_test.py"""
import array
import math
import re
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "host" / "fx"
BIN = ROOT / "build" / "host" / "ethereal"
FS = 44100
failed = []


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", BIN, "firmware/src/ethereal.c"], cwd=ROOT, check=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", ROOT / "build" / "host" / "fx_unit", "tests/fx_unit.c"],
                   cwd=ROOT, check=True)


def run(name, events, end, patch=0):
    """events: (ms, 'midi ST D1 D2'); returns (left, right, the app's log)"""
    ev = ([(10, f"enc 6 {patch}")] if patch else []) + events
    script = OUT / f"{name}.txt"
    script.write_text("".join(f"{t} {e}\n" for t, e in sorted(ev, key=lambda x: x[0])) + f"{end} end\n")
    log = subprocess.run([BIN, OUT / name, script], capture_output=True, text=True, check=True).stdout
    w = wave.open(str(OUT / f"{name}.wav"))
    a = array.array("h", w.readframes(w.getnframes()))
    return a[0::2], a[1::2], log


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def on(t, n, v=100):
    return (t, f"midi 0x90 {n} {v}")


def off(t, n):
    return (t, f"midi 0x80 {n} 0")


def seg(x, t0, t1):
    return x[int(t0 * FS):int(t1 * FS)]


def corr(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    return num / math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))


def chorus():
    """one voice is panned, not spread: without the chorus L and R are copies (correlation 1)"""
    for p, name in enumerate(["MIST", "GLASS", "HALO", "KEYS", "STAB"]):
        left, right, _ = run(f"chorus-{name}", [on(100, 60)], 4000, patch=p)
        c = corr(seg(left, 2.5, 3.5), seg(right, 2.5, 3.5))
        check(f"chorus-{name}", c < 0.9, f"spreads one voice across the sides (L/R correlation {c:.2f})")


def no_overload():
    """six loud notes on the loudest patch: the clip holds the peak, nothing wraps round"""
    left, right, _ = run("overload", [on(100 + 5 * i, 48 + 5 * i, 127) for i in range(6)], 4000, patch=2)
    peak = max(max(abs(v) for v in left), max(abs(v) for v in right))
    jump = max(abs(left[i + 1] - left[i]) for i in range(len(left) - 1))
    check("overload", peak <= 32767 and jump < 30000, f"peak {peak}, largest step {jump}")


def delay_time():
    """GLASS's 1/4 against an external clock; at 140 BPM a 1/4 (429 ms) is longer than the line: half of it"""
    for bpm in (140, 174, 200):
        p = 60000 / bpm / 24
        ticks = [(200 + round(i * p), "midi 0xF8 0 0") for i in range(int(4000 / p))]
        _, _, log = run(f"delay-{bpm}", ticks, 4200, patch=1)
        got = int(re.findall(r"delay (\d+) frames", log)[-1])
        want = 24 * p * FS / 1000
        if want > 17998:
            want /= 2
        check(f"delay-{bpm}", abs(got - want) <= 3, f"GLASS's 1/4 is {got} frames ({want:.0f} at {bpm} BPM)")


def unit(*args):
    """tests/fx_unit.c: an impulse into one effect; rows of numbers"""
    out = subprocess.run([ROOT / "build" / "host" / "fx_unit", *map(str, args)], capture_output=True, text=True,
                         check=True).stdout
    return [list(map(int, ln.split())) for ln in out.splitlines() if "ms" not in ln]   # (not the app's log)


def echoes():
    """an impulse, KEYS (a dotted 1/8 at 174 BPM: 11405 frames): left echo, then right, then left, each quieter"""
    rows = unit("delay", 3)
    d = 18 * 60000 / 174 / 24 * FS / 1000

    def peak(ch, t0, t1):
        r = max((r for r in rows if t0 <= r[0] < t1), key=lambda r: abs(r[ch]))
        return r[0], abs(r[ch])
    (l1, a1), (r2, a2), (l3, a3) = peak(1, 0, 1.5 * d), peak(2, 1.5 * d, 2.5 * d), peak(1, 2.5 * d, 3.5 * d)
    check("echoes", abs(l1 - d) <= 2 and abs(r2 - 2 * d) <= 4 and abs(l3 - 3 * d) <= 6,
          f"left at {l1}, right at {r2}, left at {l3} frames ({d:.0f}, {2 * d:.0f}, {3 * d:.0f})")
    check("echoes", a1 > a2 > a3 > 0, f"each quieter: {a1}, {a2}, {a3}")


def reverb():
    """a 23 ms burst of noise into each patch's reverb: the tail's -60 dB time (from its -5 to -35 dB, Schroeder),
    to silence, and wide; within 15 % (the burst is a sample of noise, and the damping shortens the top)"""
    for p, name in enumerate(["MIST", "GLASS", "HALO", "KEYS", "STAB"]):
        t10, *rows = unit("reverb", p)
        e = [r[0] + r[1] for r in rows]
        tail = [sum(e[i:]) for i in range(len(e))]
        db = [10 * math.log10(t / tail[0]) if t else -999 for t in tail]
        at = lambda lvl: next(i for i, d in enumerate(db) if d < lvl) * 256 / FS
        t60 = 2 * (at(-35) - at(-5))
        want = t10[0] / 10
        check(f"reverb-{name}", 0.85 * want <= t60 <= 1.15 * want, f"decays in {t60:.1f} s (set to {want:.1f} s)")
        check(f"reverb-{name}", e[-1] == 0, "ends in silence, no limit cycle")
        c = sum(r[2] for r in rows[20:400]) / math.sqrt(sum(r[0] for r in rows[20:400]) * sum(r[1] for r in rows[20:400]))
        check(f"reverb-{name}", abs(c) < 0.3, f"wide (L/R correlation {c:.2f})")
        peak, clipped = map(int, unit("loud", p)[0])
        check(f"reverb-{name}", clipped == 0 and peak < 16000, f"full-scale noise in: return peak {peak}, lines held {clipped}")


def ccs():
    """CC 91 (reverb send) and CC 93 (chorus depth), on GLASS (its 3 s release is still in the dry tail)"""
    def tail(cc91):
        left, right, _ = run(f"cc91-{cc91}", [(50, f"midi 0xB0 91 {cc91}"), on(100, 60), off(1500, 60)], 5000, patch=1)
        y = seg(left, 3.5, 4.5)
        return math.sqrt(sum(v * v for v in y) / len(y))
    dry, wet = tail(0), tail(127)
    check("cc91", wet > 3 * dry, f"the reverb send: the tail 2 s after release, rms {dry:.0f} at 0, {wet:.0f} at 127")

    def wobble(cc93):
        left, right, _ = run(f"cc93-{cc93}", [(50, f"midi 0xB0 93 {cc93}"), on(100, 60)], 4000, patch=1)
        return corr(seg(left, 2.5, 3.5), seg(right, 2.5, 3.5))
    c0, c1 = wobble(0), wobble(127)
    check("cc93", c1 < c0, f"the chorus depth: L/R correlation {c0:.2f} at 0, {c1:.2f} at 127")


if __name__ == "__main__":
    build()
    chorus()
    no_overload()
    delay_time()
    echoes()
    reverb()
    ccs()
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
