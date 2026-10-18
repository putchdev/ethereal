#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""MIDI clock in host mode: tempo, phase, lost clock, Start. Builds build/host/ethereal, writes a script per case,
reads the beats the app logs (tests/host.c). Run from anywhere: python3 tests/clock_test.py"""
import random
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "host" / "clock"
BIN = ROOT / "build" / "host" / "ethereal"
BEAT = re.compile(r"^\s*(\d+) ms  beat (\d)  (EXT|INT) (\d+)\.(\d)  bar (\d+)$")
STATE = re.compile(r"^\s*(\d+) ms  clock (EXT|INT)$")
failed = []


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", BIN, "firmware/src/ethereal.c"], cwd=ROOT, check=True)


def run(name, events, end):
    """events: (ms, 'midi ST D1 D2'); returns (beats, states)"""
    script = OUT / f"{name}.txt"
    script.write_text("".join(f"{t} {e}\n" for t, e in sorted(events, key=lambda x: x[0])) + f"{end} end\n")
    out = subprocess.run([BIN, OUT / name, script], capture_output=True, text=True, check=True).stdout
    beats = [(int(m[1]), int(m[2]), m[3], int(m[4]) * 10 + int(m[5]), int(m[6]) / 100)
             for m in map(BEAT.match, out.splitlines()) if m]
    states = [(int(m[1]), m[2]) for m in map(STATE.match, out.splitlines()) if m]
    return beats, states


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def ticks(bpm, t0, n, jitter=0.0, rng=None):
    """n clock ticks from t0 (ms), nominal times and the script's integer ms"""
    p = 60000 / bpm / 24
    nom = [t0 + i * p for i in range(n)]
    return nom, [max(0, round(t + (rng.uniform(-jitter, jitter) if jitter else 0))) for t in nom]


def phase_err_ms(beats, t0, bpm, since):
    """worst gap between the logged bar phase and the true one, in ms (ticks from t0, tick 0 the downbeat)"""
    p = 60000 / bpm / 24
    worst = 0.0
    for t, _, _, _, bar in beats:
        if t < since:
            continue
        d = (bar - (t - t0) / p) % 96
        d = d - 96 if d > 48 else d
        worst = max(worst, abs(d * p))
    return worst


def steady(bpm, jitter, seed):
    rng = random.Random(seed)
    name = f"steady-{bpm}-j{jitter}"
    nom, ms = ticks(bpm, 200, int(12000 / (60000 / bpm / 24)), jitter, rng)
    ev = [(150, "midi 0xFA 0 0")] + [(t, "midi 0xF8 0 0") for t in ms]
    beats, _ = run(name, ev, ms[-1] + 1)
    late = [b for b in beats if b[0] >= 3000]
    bpms = [b[3] / 10 for b in late]
    tol = 0.5 if jitter else 0.2
    check(name, late and all(b[2] == "EXT" for b in late), "EXT throughout")
    check(name, bpms and max(abs(x - bpm) for x in bpms) <= tol,
          f"BPM {min(bpms):.1f}..{max(bpms):.1f} within +-{tol}")
    err = phase_err_ms(beats, nom[0], bpm, 1000)
    check(name, err <= (10 if jitter else 8), f"phase within {err:.1f} ms of the ticks")


def late_sender(bpm, late):
    """clocks sent from a busy main loop (the Tako: passes of 7.5 to 17 ms), each 0..late ms after its time"""
    rng = random.Random(bpm)
    name = f"late-{bpm}-{late}"
    nom, _ = ticks(bpm, 200, int(15000 / (60000 / bpm / 24)))
    ms = [round(t + rng.uniform(0, late)) for t in nom]
    beats, states = run(name, [(150, "midi 0xFA 0 0")] + [(t, "midi 0xF8 0 0") for t in ms], ms[-1] + 1)
    bpms = [b[3] / 10 for b in beats if b[0] >= 3000]
    check(name, [s for _, s in states] == ["EXT"], "EXT throughout")
    check(name, bpms and max(abs(x - bpm) for x in bpms) <= 1.0, f"BPM {min(bpms):.1f}..{max(bpms):.1f} within +-1")
    err = phase_err_ms(beats, nom[0] + late / 2, bpm, 3000)
    check(name, err <= 12, f"phase within {err:.1f} ms of the ticks, on average {late / 2:.1f} ms late")


def lost(offset):
    bpm, name = 174, f"lost-offset{offset}"
    p = 60000 / bpm / 24
    nom, ms = ticks(bpm, 200, 96 * 4 + 40)    # stops mid-bar
    gap = 2000
    k0 = len(nom) + round(gap / p)            # the grid goes on while the clock is away
    nom2 = [nom[0] + (k0 + i) * p + offset for i in range(96 * 3)]
    ev = ([(150, "midi 0xFA 0 0")] + [(t, "midi 0xF8 0 0") for t in ms] +
          [(round(t), "midi 0xF8 0 0") for t in nom2])
    beats, states = run(name, ev, round(nom2[-1]) + 1)
    t_int = next((t for t, s in states if s == "INT"), None)
    check(name, t_int is not None and 250 <= t_int - ms[-1] <= 260, f"INT {t_int and t_int - ms[-1]} ms after the last tick")
    held = [b[3] / 10 for b in beats if t_int and t_int < b[0] < nom2[0]]
    check(name, held and all(abs(x - bpm) <= 0.2 for x in held), "free-runs at the last tempo")
    t_ext = next((t for t, s in states if s == "EXT" and t > ms[-1]), None)
    check(name, t_ext is not None and t_ext - round(nom2[0]) <= 1, "EXT again at the first returning tick")
    gap_err = phase_err_ms([b for b in beats if b[0] < nom2[0]], nom[0], bpm, 1000)
    check(name, gap_err <= 8, f"phase kept through the gap ({gap_err:.1f} ms)")
    err = phase_err_ms(beats, nom[0] + offset, bpm, nom2[0] + 24 * p)
    check(name, err <= 8, f"locked again within a beat ({err:.1f} ms)")


def start_resets():
    bpm, name = 174, "start"
    nom, ms = ticks(bpm, 1033, 96 * 3)        # 1 s free-running first, then Start somewhere in its bar
    ev = [(1030, "midi 0xFA 0 0")] + [(t, "midi 0xF8 0 0") for t in ms]
    beats, _ = run(name, ev, ms[-1] + 1)
    first = next((b for b in beats if b[0] >= ms[0]), None)
    check(name, first is not None and first[1] == 0 and first[0] - ms[0] <= 1, "the first tick after Start is beat 0")
    err = phase_err_ms(beats, nom[0], bpm, ms[0] + 1)
    check(name, err <= 8, f"phase from the downbeat ({err:.1f} ms)")


def tempo_change():
    name = "tempo-change"
    nom, ms = ticks(174, 200, 96 * 4)
    p2 = 60000 / 140 / 24
    nom2 = [nom[-1] + (i + 1) * p2 for i in range(96 * 3)]
    ev = [(150, "midi 0xFA 0 0")] + [(t, "midi 0xF8 0 0") for t in ms + [round(t) for t in nom2]]
    beats, _ = run(name, ev, round(nom2[-1]) + 1)
    after = [b[3] / 10 for b in beats if b[0] >= nom2[0] + 2 * 24 * p2]
    check(name, after and all(abs(x - 140) <= 0.5 for x in after), "140 BPM within two beats of the change")


if __name__ == "__main__":
    build()
    for bpm in (140, 174, 200):
        steady(bpm, 0, bpm)
        steady(bpm, 2, bpm)
        steady(bpm, 4, bpm)                   # the Tako sends each clock up to a block (2.9 ms) and a loop pass late
    for bpm in (140, 174, 200):
        late_sender(bpm, 17)
    lost(0)
    lost(5)
    start_resets()
    tempo_change()
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
