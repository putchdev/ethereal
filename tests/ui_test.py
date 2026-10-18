#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The pages in host mode: a page button picks what KNOB1..4 edit, a detent is a step (quick turns over a wide range,
four), values stop at their ends, and the knobs reach the sound; then the settings: saved when quiet, SAVE into a user
slot, the SETTINGS page, the chord latch. Reads the app's log ("knob NAME VALUE" and the rest).
Run from anywhere: python3 tests/ui_test.py"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "host" / "ui"
BIN = ROOT / "build" / "host" / "ethereal"
KNOB = re.compile(r"^\s*(\d+) ms  knob (.+) (-?\d+)$")
BTN = {"FX": 2, "ENV": 4, "LFO": 5, "EDIT": 6, "GLO": 7, "HOME": 8, "SAVE": 9, "PLAY": 12}
ENC_SELECT, ENC_PRESETS = 0, 6
failed = []


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DETHEREAL_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", BIN, "firmware/src/ethereal.c"], cwd=ROOT, check=True)


def run_log(name, events, end=2000):
    """events: (ms, 'btn ID' | 'enc ID STEPS' | 'keys MASK' | 'midi ...'); returns the app's log"""
    script = OUT / f"{name}.txt"
    script.write_text("".join(f"{t} {e}\n" for t, e in sorted(events, key=lambda x: x[0])) + f"{end} end\n")
    return subprocess.run([BIN, OUT / name, script], capture_output=True, text=True, check=True).stdout


def run(name, events, end=2000):
    """returns [(name, value)] of the knob log"""
    return [(m[2], int(m[3])) for m in map(KNOB.match, run_log(name, events, end).splitlines()) if m]


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def knob(t, k, steps):
    """KNOB k (1..4): encoders 2..5"""
    return (t, f"enc {k + 1} {steps}")


def pages():
    want = {"HOME": "CUTOFF", "EDIT": "OSC", "ENV": "ATTACK", "LFO": "LFO1", "FX": "CHORUS", "GLO": "SUB WAVE"}
    for page, first in want.items():
        got = run(f"page-{page}", [(100, f"btn {BTN[page]}"), knob(200, 1, 1)])
        check("pages", got and got[0][0] == first, f"{page}: KNOB1 edits {got[0][0] if got else 'nothing'}")


def steps():
    got = run("steps", [(100, f"btn {BTN['EDIT']}"), knob(200, 2, -1), knob(400, 2, -1), knob(420, 2, -1)])
    a = [v for _, v in got]
    check("steps", a[0] - a[1] == 1 and a[1] - a[2] == 1, f"DETUNE (0..25): a detent a step, quick or slow {a}")
    got = run("steps-wide", [knob(200, 1, 1), knob(400, 1, 1), knob(420, 1, 1)])
    a = [v for _, v in got]
    check("steps", a[1] - a[0] == 1 and a[2] - a[1] == 4, f"CUTOFF (0..255): slow 1, quick 4 {a}")


def ends():
    got = run("ends", [(100, f"btn {BTN['FX']}"), knob(200, 2, 50), knob(300, 2, -50)])
    a = [v for _, v in got]
    check("ends", a == [3, 0], f"DELAY's four times stop at their ends {a}")
    got = run("duck-rel", [(100, f"btn {BTN['GLO']}"), knob(200, 4, -300), knob(300, 4, 1), knob(400, 4, 1)])
    a = [v for _, v in got]
    check("ends", a == [0, 1, 2], f"DUCK REL: 1/16, 1/8, then 30 ms {a}")


def reaches_sound():
    """CUTOFF all the way down before a chord: quieter (its LFO and envelope still lift it above 40 Hz)"""
    import array
    import math
    import wave

    def level(name, ev):                        # (turned before the notes: the reverb would carry the old sound)
        run(name, [(300, "midi 0x90 60 110"), (300, "midi 0x90 67 110")] + ev, 3000)
        w = wave.open(str(OUT / f"{name}.wav"))
        x = array.array("h", w.readframes(w.getnframes()))[0::2]
        y = x[int(2.2 * 44100):int(2.9 * 44100)]
        return math.sqrt(sum(v * v for v in y) / len(y))
    a, b = level("cut-open", [knob(100, 1, 60)]), level("cut-shut", [knob(100, 1, -260)])
    check("sound", b < 0.5 * a, f"CUTOFF from the knob: rms {a:.0f} open, {b:.0f} shut")


def saves():
    """a change is saved 2 s later, once nothing sounds; not while a note is held"""
    log = run_log("save-quiet", [knob(100, 1, 1)], 3000)
    t = [int(m[1]) for m in re.finditer(r"(\d+) ms  settings saved", log)]
    check("saves", len(t) == 1 and 2100 <= t[0] < 2400, f"saved once, at {t} ms")
    log = run_log("save-held", [knob(100, 1, 1), (50, "midi 0x90 60 100")], 4000)
    check("saves", "settings saved" not in log, "not while a note sounds")


def save_to():
    """CUTOFF down 10, SAVE, PRESETS to USER2, SAVE; away to MIST and back: the edit came with it"""
    got = run_log("save-to", [knob(100, 1, -10), (300, f"btn {BTN['SAVE']}"), (400, f"enc {ENC_PRESETS} 1"),
                              (500, f"btn {BTN['SAVE']}"), (700, f"enc {ENC_PRESETS} -6"), (900, f"enc {ENC_PRESETS} 6"),
                              knob(1100, 1, 1)])
    saved = re.findall(r"ms  saved (\S+)", got)
    pats = re.findall(r"ms  patch (\S+)", got)
    cut = [int(v) for v in re.findall(r"knob CUTOFF (\d+)", got)]
    check("save-to", saved == ["USER2"], f"saved into {saved}")
    check("save-to", pats == ["MIST", "USER2"], f"PRESETS went to {pats}")
    check("save-to", len(cut) == 2 and cut[1] == cut[0] + 1, f"USER2 came back with its CUTOFF {cut}")
    got = run_log("save-cancel", [(300, f"btn {BTN['SAVE']}"), (400, f"btn {BTN['HOME']}"), (500, f"btn {BTN['SAVE']}"),
                                  (600, f"btn {BTN['FX']}")])
    check("save-to", "saved" not in got.replace("settings saved", ""), "a page button cancels")


def settings_page():
    """SELECT opens SETTINGS; SUB UPTO two keys down: key 10 (D4) plays the pad, not the sub"""
    def what(name, ev):
        log = run_log(name, ev + [(400, f"keys {1 << 10}"), (600, "keys 0")], 1000)
        return ("sub" if re.search(r"ms  sub \d+", log) else "pad" if "-> voice" in log else "none"), log
    a, _ = what("zone-12", [])
    b, log = what("zone-10", [(100, f"enc {ENC_SELECT} 1"), knob(200, 3, -2)])
    check("settings", "knob SUB UPTO 10" in log, "SELECT, then KNOB3 edits SUB UPTO")
    check("settings", a == "sub" and b == "pad", f"key 10 plays the {a} at 12, the {b} at 10")
    log = run_log("ch-change", [(100, "midi 0x90 60 100"), (150, "midi 0x91 40 100"), (200, f"enc {ENC_SELECT} 1"),
                                knob(300, 1, 1)], 20000)
    t = [int(m[1]) for m in re.finditer(r"(\d+) ms  all notes off", log)]
    check("settings", len(t) == 1 and "settings saved" in log, f"a new PAD CH stops the notes ({t} ms), so it can save")


def latch():
    """PLAY latches: a chord let go keeps sounding; the next chord stops it; PLAY again lets go"""
    chord = 1 << 14 | 1 << 18 | 1 << 21
    log = run_log("latch", [(100, f"btn {BTN['PLAY']}"), (200, f"keys {chord}"), (500, "keys 0"),
                            (1000, f"keys {1 << 16}"), (1200, "keys 0")], 1500)
    t = [int(m[1]) for m in re.finditer(r"(\d+) ms  unlatch", log)]
    check("latch", len(t) == 3 and all(990 <= x <= 1010 for x in t), f"the chord stops at the next one: {t} ms")
    log = run_log("latch-off", [(200, f"keys {chord}"), (500, "keys 0"), (1000, f"keys {1 << 16}")], 1500)
    check("latch", "unlatch" not in log, "off: the keys stop their own notes")


def midi_control():
    """Program Change picks a slot; CC 20.. turn the knobs; a CC is heard but not saved, a knob is"""
    got = run_log("pc", [(100, "midi 0xC0 4 0"), (300, "midi 0xC0 6 0"), (500, "midi 0xC0 40 0")])
    check("midi", re.findall(r"ms  patch (\S+)", got) == ["STAB", "USER2"], "PC 4: STAB, 6: USER2, 40: nothing")
    got = run_log("cc-knobs", [(100, "midi 0xB0 20 127"), (200, "midi 0xB0 38 0"), (300, "midi 0xB1 43 127"),
                               (400, "midi 0xB0 74 64")])
    cc = re.findall(r"ms  cc (\d+) (.+) (-?\d+)$", got, re.M)
    want = [("20", "CUTOFF", "255"), ("38", "FEEDBACK", "0"), ("43", "DUCK REL", "257"),
            ("74", "CUTOFF", "129")]
    check("midi", cc == want, f"CC 20 / 38 / 43 (either channel) and 74 set their knobs {cc}")
    got = run_log("cc-unsaved", [(100, "midi 0xB0 74 0"), (200, "midi 0xB0 38 127")], 4000)
    check("midi", "settings saved" not in got, "a CC alone saves nothing")


def sub_velocity():
    """the sub's level follows velocity: 127 against 40, the same note"""
    import array
    import math
    import wave

    def level(name, vel):
        run(name, [(100, f"midi 0x91 36 {vel}")], 1500)
        w = wave.open(str(OUT / f"{name}.wav"))
        x = array.array("h", w.readframes(w.getnframes()))[0::2]
        y = x[int(0.8 * 44100):int(1.3 * 44100)]
        return math.sqrt(sum(v * v for v in y) / len(y))
    a, b = level("subvel-127", 127), level("subvel-40", 40)
    want = (64 + 40 * 192 // 127) / 256
    check("sub-vel", abs(b / a - want) < 0.08, f"velocity 40 is {b / a:.2f} of 127 (want about {want:.2f})")


if __name__ == "__main__":
    build()
    pages()
    steps()
    ends()
    reaches_sound()
    saves()
    save_to()
    settings_page()
    latch()
    midi_control()
    sub_velocity()
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
