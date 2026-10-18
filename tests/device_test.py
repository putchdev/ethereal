#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""On the FM-1 itself, Ethereal installed: notes go in over its USB-MIDI, the sound comes back over its USB audio,
and a Tako sampler on TRS sends the clock (its sequencer's PLAY and STOP only: nothing in its project changes).

  python3 tests/device_test.py              (TAKO=path/to/tako-sampler if it is not beside this repo)

Checks the sub (pitch, mono, dry). With the diagnostic build (./build.sh --diag) installed, the clock lock too, from
what the FM-1 reports at each downbeat: its milliseconds against this computer's, each tick's stamp, the bar phase.
First with a clock from this computer over USB, then the Tako's over TRS if its project has a pattern. (The sound
alone cannot time the lock: the pad's detuned oscillators beat, and their nulls look like ducks.)"""
import array
import json
import math
import os
import re
import subprocess
import sys
import time
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "device"
TAKO = Path(os.environ.get("TAKO", ROOT.parent / "tako-sampler"))
FS = 44100
failed = []


def check(case, ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'}  {case}: {what}")
    if not ok:
        failed.append(case)


def fm1_ports():
    """(amidi port, arecord device) of the FM-1 ("Felucca")"""
    midi = re.search(r"(hw:\d+,\d+,\d+)\s+Felucca", subprocess.run(["amidi", "-l"], capture_output=True, text=True).stdout)
    card = re.search(r"card (\d+): Felucca", subprocess.run(["arecord", "-l"], capture_output=True, text=True).stdout)
    if not midi or not card:
        sys.exit("no FM-1 on USB (amidi -l / arecord -l show no Felucca)")
    return midi[1], f"hw:{card[1]},0"


MIDI, AUDIO = fm1_ports()


def send(*b):
    subprocess.run(["amidi", "-p", MIDI, "-S", " ".join(f"{x:02X}" for x in b)], check=True)


def tako(req):
    out = subprocess.run([sys.executable, TAKO / "tools" / "tako_agent.py", "call", json.dumps(req)],
                         capture_output=True, text=True, timeout=30).stdout
    reply = json.loads(out.strip().splitlines()[-1])
    if not reply.get("ok"):
        sys.exit(f"tako {req}: {reply}")
    return reply


def record(name, seconds, steps):
    """record `seconds` while running steps: [(at s, function)]; returns left, right"""
    path = OUT / f"{name}.wav"
    rec = subprocess.Popen(["arecord", "-q", "-D", AUDIO, "-f", "S16_LE", "-c", "2", "-r", str(FS), "-d", str(seconds),
                            path])
    t0 = time.monotonic()
    for at, fn in steps:
        time.sleep(max(0.0, at - (time.monotonic() - t0)))
        fn()
    rec.wait()
    w = wave.open(str(path))
    a = array.array("h", w.readframes(w.getnframes()))
    return a[0::2], a[1::2]


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def sub():
    left, right = record("sub", 4, [(0.5, lambda: send(0x91, 40, 100)), (2.5, lambda: send(0x81, 40, 0))])
    a, b = int(1.2 * FS), int(2.3 * FS)
    ups = [i + left[i] / (left[i] - left[i + 1]) for i in range(a, b) if left[i] < 0 <= left[i + 1]]
    f = (len(ups) - 1) * FS / (ups[-1] - ups[0]) if len(ups) > 1 else 0
    want = 440 * 2 ** ((40 - 69) / 12)
    check("sub", abs(f - want) < want * 0.005, f"E2 on channel 2 plays {f:.2f} Hz ({want:.2f})")
    diff = max(abs(l - r) for l, r in zip(left[a:b], right[a:b]))
    peak = max(map(abs, left[a:b]))
    check("sub", diff <= peak / 1000, f"mono: left and right differ by {diff} at most (peak {peak})")
    tail = rms(left[int(3.3 * FS):int(3.9 * FS)])
    check("sub", tail < 0.01 * rms(left[a:b]), f"dry: {tail:.1f} rms 0.8 s after the release, no FX tail")


def clock_lock(source="usb"):
    """30 s of clock: exact 170 BPM from this computer over USB, or the Tako's over TRS (its PLAY and STOP); the
    diagnostic build reports each downbeat on channel 16"""
    import rtmidi
    pick = lambda m: next(i for i, p in enumerate(m.get_ports()) if "Felucca" in p)
    o, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
    o.open_port(pick(o))
    inp.open_port(pick(inp))
    got, cur = [], {}

    def cb(msg, _):
        m = msg[0]
        if m[0] == 0xBF and 102 <= m[1] <= 110:
            cur[m[1]] = m[2]
            if m[1] == 110:
                got.append((time.monotonic(), dict(cur)))
    inp.set_callback(cb)
    sent = []
    if source == "usb":
        bpm = 170
        per = 60 / bpm / 24
        start = time.monotonic() + 0.3
        o.send_message([0xFA])
        for k in range(24 * 85):
            t = start + k * per
            while time.monotonic() < t:
                pass
            o.send_message([0xF8])
            sent.append(t)
        o.send_message([0xFC])
    else:
        bpm = tako({"cmd": "status"})["tempo"]
        tako({"cmd": "play", "pattern": 0})
        time.sleep(30)
        tako({"cmd": "stop"})
    time.sleep(0.3)
    if len(got) < 10:
        print("  skip  clock: no reports (install build/ethereal-diag.fwsc to check the lock)")
        return

    def unwrap(v, mod):
        out, base = [v[0]], 0
        for a, b in zip(v, v[1:]):
            base += mod if b < a else 0
            out.append(b + base)
        return out
    S = unwrap([c[102] << 7 | c[103] for _, c in got], 16384)
    ph = [(c[109] << 7 | c[110]) / 16384 for _, c in got]
    j0, name = 1, f"clock-{source}"             # (the first downbeat is the one Start set)
    bar_ms = 4 * 60000 / bpm
    if sent:                                    # against the send times (a report's trip back here can be held up)
        last = max(j for j in range(len(got)) if 96 * j < len(sent))
        late = [(S[j] - S[j0]) - (sent[96 * j] - sent[96 * j0]) * 1000 for j in range(j0, last + 1)]
        ms_rate = (S[last] - S[j0]) / ((sent[96 * last] - sent[96 * j0]) * 1000)
        check(name, abs(ms_rate - 1) < 0.001, f"the FM-1's ms keep this computer's time ({(ms_rate - 1) * 100:+.3f} %)")
        check(name, max(map(abs, late)) < 3, f"each downbeat stamped within {max(map(abs, late)):.1f} ms of when sent")
    else:                                       # the Tako's: against its tempo, from the first downbeat
        late = [(S[j] - S[j0]) - (j - j0) * bar_ms for j in range(j0, len(got))]
        check(name, max(map(abs, late)) < 3, f"each downbeat stamped within {max(map(abs, late)):.1f} ms of {bpm} BPM's")
    off = [min(p, 1 - p) * bar_ms for p in ph[j0:]]
    check(name, max(off) < 7, f"the bar phase at each downbeat within {max(off):.1f} ms of the bar line (a block is 5.8)")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    sub()
    clock_lock("usb")
    if any(pt["notes"] for pt in tako({"cmd": "patterns"})["patterns"]):
        clock_lock("tako")
    else:
        print("  skip  clock-tako: the Tako's project has no pattern to play")
    print("failed: " + ", ".join(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
