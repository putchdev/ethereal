#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The sub in host mode: pitch, mono and dry, glide (legato only), last-note priority, the retrigger fade, CC 5,
the keyboard split. Builds build/host/ethereal, writes a script per case, reads its log and WAV.
Run from anywhere: python3 tests/sub_test.py"""
import array
import re
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "host" / "sub"
BIN = ROOT / "build" / "host" / "ethereal"
FS = 44100
failed = []


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", BIN, "firmware/src/ethereal.c"], cwd=ROOT, check=True)


def run(name, events, end):
    """events: (ms, 'midi ST D1 D2' | 'keys MASK' | 'btn ID'); returns (left, right, the app's log)"""
    script = OUT / f"{name}.txt"
    script.write_text("".join(f"{t} {e}\n" for t, e in sorted(events, key=lambda x: x[0])) + f"{end} end\n")
    log = subprocess.run([BIN, OUT / name, script], capture_output=True, text=True, check=True).stdout
    w = wave.open(str(OUT / f"{name}.wav"))
    a = array.array("h", w.readframes(w.getnframes()))
    return a[0::2], a[1::2], log


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def on(t, n, ch=2):
    return (t, f"midi {0x8F + ch:#x} {n} 100")


def off(t, n, ch=2):
    return (t, f"midi {0x7F + ch:#x} {n} 0")


def hz(note):
    return 440 * 2 ** ((note - 69) / 12)


def freq(x, t0, t1):
    """from the rising zero crossings between t0 and t1 (s), interpolated"""
    a, b = int(t0 * FS), int(t1 * FS)
    ups = [i + x[i] / (x[i] - x[i + 1]) for i in range(a, b) if x[i] < 0 <= x[i + 1]]
    return (len(ups) - 1) * FS / (ups[-1] - ups[0]) if len(ups) > 1 else 0


def near(f, want, pct=0.5):
    return abs(f - want) <= want * pct / 100


def pitch_and_dry():
    left, right, _ = run("pitch", [on(100, 40), off(1500, 40)], 3000)
    f = freq(left, 0.5, 1.4)
    check("pitch", near(f, hz(40)), f"E2 on channel 2 is {f:.2f} Hz ({hz(40):.2f})")
    check("mono", left == right, "the same in both sides")
    tail = max(abs(v) for v in left[int(2.3 * FS):])
    check("dry", tail == 0, f"silent 0.8 s after the release: no FX on it (largest sample {tail})")


def glide():
    left, _, log = run("glide", [on(100, 40), on(1000, 47), off(2000, 47), off(2000, 40), on(2300, 47), off(3000, 47)], 3500)
    mid, end, fresh = freq(left, 1.005, 1.04), freq(left, 1.4, 1.9), freq(left, 2.32, 2.6)
    check("glide", hz(40) * 1.03 < mid < hz(47) * 0.97, f"legato: on its way, {mid:.1f} Hz in the first 40 ms")
    check("glide", near(end, hz(47)), f"legato: there, {end:.2f} Hz ({hz(47):.2f})")
    check("glide", near(fresh, hz(47)), f"not legato: no glide, {fresh:.2f} Hz at once")
    check("glide", "sub 47 (legato)" in log and log.count("sub 47\n") == 1, "legato only while a note is held")


def last_note():
    left, _, _ = run("last-note", [on(100, 40), on(600, 47), off(1200, 47), off(2500, 40)], 3000)
    f = freq(left, 1.7, 2.4)
    check("last-note", near(f, hz(40)), f"back to the held E2 when B2 goes: {f:.2f} Hz")


def cc5():
    left, _, _ = run("cc5", [(50, "midi 0xB1 5 0"), on(100, 40), on(1000, 47), off(2000, 47), off(2000, 40)], 2500)
    f = freq(left, 1.02, 1.06)                  # (the WAV runs 11.6 ms behind the script, as the FM-1's output)
    check("cc5", near(f, hz(47), 1), f"CC 5 at 0: no glide, {f:.1f} Hz at once")


def no_click():
    """a new note while the last still sounds or releases: the old one fades out first, the new starts at zero"""
    left, _, _ = run("retrigger", [on(100, 36), off(800, 36), on(830, 43), off(1500, 43), on(1500, 36), off(2200, 36)], 2600)
    steps = [abs(left[i + 1] - left[i]) for i in range(len(left) - 1)]
    big = max(steps)
    normal = max(steps[int(0.4 * FS):int(0.7 * FS)])
    check("no-click", big < 2 * normal, f"largest step {big}, held note's own {normal}")


def split():
    _, _, log = run("split", [(100, "keys 0x1"), (300, "keys 0"), (400, "keys 0x1000"), (600, "keys 0"),
                              (700, "btn 3"), (800, "keys 0x1"), (1000, "keys 0")], 1500)
    check("split", "sub 29" in log, "split: F3 key plays the sub, two octaves down (F1, note 29)")
    check("split", "note 65 ->" in log, "split: F4 key plays the pad (note 65)")
    check("split", "split 0" in log and "note 53 ->" in log, "SEL: split off, F3 plays the pad (note 53)")


if __name__ == "__main__":
    build()
    pitch_and_dry()
    glide()
    last_note()
    cc5()
    no_click()
    split()
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
