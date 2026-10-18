#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write firmware/gen/ethereal_tables.h: the constants the chip cannot compute (no FPU).

  python3 tools/gen_tables.py

Envelope times run 1 ms to 16 s over an index 0..127 (log spaced), as a knob will set them. The envelopes step
every CTRL frames: level += (target - level) * k, k in Q24."""
import math
from pathlib import Path

FS = 44100
CTRL = 32                       # frames per control step (ethereal.c CTRL)
OUT = Path(__file__).resolve().parents[1] / "firmware" / "gen" / "ethereal_tables.h"


def t_ms(i):
    return 16000 ** (i / 127)


def k_q24(tau_s):
    return round((1 - math.exp(-CTRL / (tau_s * FS))) * (1 << 24))


def table(name, ctype, values, per_line=8, comment=""):
    rows = [", ".join(str(v) for v in values[i:i + per_line]) for i in range(0, len(values), per_line)]
    return f"/* {comment} */\nstatic const {ctype} {name}[{len(values)}] = {{\n    " + ",\n    ".join(rows) + "};\n"


def main():
    att = [k_q24(t_ms(i) / 1000 / math.log(1.3 / 0.3)) for i in range(128)]   # from 0 towards 1.3, at 1.0 after t
    dec = [k_q24(t_ms(i) / 1000 / math.log(100)) for i in range(128)]         # 99 % of the way after t (-40 dB)
    det = [round((2 ** (c / 1200) - 1) * (1 << 32)) for c in range(26)]       # ratio - 1 for 0..25 cents, Q32
    # the pad filter runs twice a frame (stable to 12 kHz with integer states): its F at 2 x 44.1 kHz
    cut = [round(2 * math.sin(math.pi * 40 * 300 ** (i / 255) / (2 * FS)) * 32768) for i in range(256)]
    # the master soft clip: straight to 0.75, then a tanh knee to 1.0; |x| 0..2.0 in 1024 steps (Q15 in and out)
    clip = [round(32767 * (x if x < 0.75 else 0.75 + 0.25 * math.tanh((x - 0.75) / 0.25)))
            for x in (i / 512 for i in range(1025))]
    exp2n = [round(2 ** (-i / 256) * 65535) for i in range(256)]               # 2^(-i/256), Q16: reverb decay gains
    tanh = [round(32767 * math.tanh(i / 256)) for i in range(1025)]            # the sub's saturation, |x| 0..4.0
    time_ms = [round(t_ms(i)) for i in range(128)]                               # for the screen
    cut_hz = [round(40 * 300 ** (i / 255)) for i in range(256)]
    text = ("/* SPDX-License-Identifier: GPL-3.0-only */\n/* Written by tools/gen_tables.py: do not edit. */\n"
            f"#define ENV_CTRL {CTRL}u\n\n"
            + table("ATT_K", "uint32_t", att, comment="attack: k (Q24) for time index i, 1 ms * 16000^(i/127)") + "\n"
            + table("DEC_K", "uint32_t", dec, comment="decay / release: k (Q24) to -40 dB in the same times") + "\n"
            + table("DETUNE_Q32", "uint32_t", det, comment="2^(cents/1200) - 1, Q32, for 0..25 cents") + "\n"
            + table("CUT_F", "uint16_t", cut, per_line=12,
                    comment="state-variable filter F (Q15) at 88.2 kHz for cutoff index i: 40 Hz * 300^(i/255)") + "\n"
            + table("CLIP", "uint16_t", clip, per_line=12,
                    comment="soft clip: |x| = i / 512 (0..2.0) -> Q15; linear to 0.75, a tanh knee above") + "\n"
            + table("EXP2N", "uint16_t", exp2n, per_line=12, comment="2^(-i/256), Q16 (65535 = 1)") + "\n"
            + table("TANH", "uint16_t", tanh, per_line=12, comment="tanh(i / 256), Q15: |x| 0..4.0") + "\n"
            + table("TIME_MS", "uint16_t", time_ms, per_line=12, comment="time index i in ms, for the screen") + "\n"
            + table("CUT_HZ", "uint16_t", cut_hz, per_line=12, comment="cutoff index i in Hz, for the screen"))
    OUT.write_text(text)
    print(OUT, "; time index of 1.2 s:", round(127 * math.log(1200) / math.log(16000)),
          "2 s:", round(127 * math.log(2000) / math.log(16000)), "4 s:", round(127 * math.log(4000) / math.log(16000)))


if __name__ == "__main__":
    main()
