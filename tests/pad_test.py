#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The pad in host mode: voice allocation and stealing, the steal fade, sustain, CC 74. Builds build/host/ethereal,
writes a script per case, reads the app's log (tests/host.c) and its WAV. Run from anywhere: python3 tests/pad_test.py"""
import array
import math
import re
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "host" / "pad"
BIN = ROOT / "build" / "host" / "ethereal"
NOTE = re.compile(r"^\s*(\d+) ms  note (\d+) -> voice (\d) \((\w+)\)$")
FADED = re.compile(r"^\s*(\d+) ms  voice (\d) faded$")
FS = 44100
failed = []


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", BIN, "firmware/src/ethereal.c"], cwd=ROOT, check=True)


def run(name, events, end, patch=0):
    """events: (ms, 'midi ST D1 D2'); returns (notes, fades, left channel)"""
    ev = ([(10, f"enc 6 {patch}")] if patch else []) + events
    script = OUT / f"{name}.txt"
    script.write_text("".join(f"{t} {e}\n" for t, e in sorted(ev, key=lambda x: x[0])) + f"{end} end\n")
    out = subprocess.run([BIN, OUT / name, script], capture_output=True, text=True, check=True).stdout
    notes = [(int(m[1]), int(m[2]), int(m[3]), m[4]) for m in map(NOTE.match, out.splitlines()) if m]
    fades = [(int(m[1]), int(m[2])) for m in map(FADED.match, out.splitlines()) if m]
    w = wave.open(str(OUT / f"{name}.wav"))
    return notes, fades, array.array("h", w.readframes(w.getnframes()))[0::2]


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def on(t, n, v=100):
    return (t, f"midi 0x90 {n} {v}")


def off(t, n):
    return (t, f"midi 0x80 {n} 0")


def rms_db(x):
    return 20 * math.log10(max(math.sqrt(sum(v * v for v in x) / len(x)), 1) / 32768)


def seg(x, t0, t1):
    return x[int(t0 * FS):int(t1 * FS)]


def stealing():
    name = "steal"
    ev = [on(100 + 10 * i, 60 + i) for i in range(6)] + [on(1000, 70)]
    notes, fades, _ = run(name, ev, 1500)
    six = notes[:6]
    check(name, [w for *_, w in six] == ["free"] * 6 and len({v for _, _, v, _ in six}) == 6, "six notes take six voices")
    t, n, v, why = notes[6]
    check(name, why == "held" and v == six[0][2], f"a seventh takes the oldest held note's voice ({why}, voice {v})")
    fade = next((ft for ft, fv in fades if fv == v), None)
    # (host mode renders 5.8 ms blocks and logs the block's time: the 2.2 ms fade ends within the next one)
    check(name, fade is not None and fade - t <= 6, f"which fades out first, within a block ({fade and fade - t} ms)")


def released_first():
    name = "released"
    ev = [on(100 + 10 * i, 60 + i) for i in range(6)] + [off(2000, 61), off(3500, 63), on(4000, 72)]
    notes, _, _ = run(name, ev, 4500)
    v61 = next(v for _, n, v, _ in notes if n == 61)
    t, n, v, why = notes[-1]
    check(name, why == "released" and v == v61, f"a released voice before a held one, the quietest ({why}, voice {v})")


def again():
    name = "again"
    notes, _, _ = run(name, [on(100, 60), on(110, 64), off(500, 60), on(800, 60)], 1200)
    check(name, notes[-1][3] == "again" and notes[-1][2] == notes[0][2], "the same note takes its own voice again")


def sustain():
    name = "sustain"
    ev = [(100, "midi 0xB0 64 127"), on(200, 60), off(1500, 60), (6000, "midi 0xB0 64 0")]
    _, _, x = run(name, ev, 13000, patch=3)     # KEYS: 1.2 s release
    held, after = rms_db(seg(x, 5.0, 5.5)), rms_db(seg(x, 9.0, 9.5))
    check(name, held > -40, f"held by the pedal after its note-off ({held:.1f} dBFS at 5 s)")
    check(name, after < held - 30, f"released when the pedal comes up ({after:.1f} dBFS at 9 s)")


def cutoff():
    name = "cc74"
    ev = [on(100, 48), on(100, 55), on(100, 60), (3000, "midi 0xB0 74 10"), (5000, "midi 0xB1 74 120")]
    _, _, x = run(name, ev, 7000)

    def bright(t0):
        y = seg(x, t0, t0 + 0.5)
        d = [y[i + 1] - y[i] for i in range(len(y) - 1)]
        return 20 * math.log10(math.sqrt(sum(v * v for v in d) / len(d)) / math.sqrt(sum(v * v for v in y) / len(y)))
    lo, hi = bright(4.0), bright(6.0)
    check(name, hi - lo > 15, f"CC 74 opens the filter, on channel 2 too ({lo:.1f} -> {hi:.1f} dB high-frequency share)")


def no_click():
    """the same new note on a stolen voice and on a free one: the steal must not add a jump (KEYS: triangles and a
    sine, smooth, so a click would show)"""
    def jump(n_held):
        ev = [on(100 + 10 * i, 60 + i) for i in range(n_held)] + [on(2000, 84, 127)]
        notes, _, x = run(f"click-{n_held}", ev, 2600, patch=3)
        t = notes[-1][0] / 1000
        y = seg(x, t - 0.005, t + 0.03)
        return notes[-1][3], max(abs(y[i + 2] - 2 * y[i + 1] + y[i]) for i in range(len(y) - 2))
    (w6, stolen), (w5, free) = jump(6), jump(5)
    check("click", w6 == "held" and w5 == "free" and stolen <= free * 1.2,
          f"no click where a voice is stolen ({stolen}, against {free} on a free voice)")


if __name__ == "__main__":
    build()
    stealing()
    released_first()
    again()
    sustain()
    cutoff()
    no_click()
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
