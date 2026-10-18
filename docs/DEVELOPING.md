# Developing Ethereal

The app is `firmware/src/ethereal.c`; everything under it (boot, updates, the USB rescue, USB-MIDI, USB audio, the
panel, the screen, settings) is Dinghy's platform, described in [API.md](API.md).

## Set up once

Linux on x86-64 (or macOS with Docker), Python 3 with Pillow, the JieLi toolchain and the JieLi AC79 SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                      # JieLi's clang for the chip, to ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
```

`JIELI_TOOLCHAIN` and `AC79_SDK` point elsewhere if your copies live somewhere else.

## Host mode

```sh
tests/run_host.sh                           # -> build/host/ethereal.wav and screen PNGs
```

The same `ethereal.c` runs on your computer: the sound as a WAV, the screen as images. A script file drives it
(keys, buttons, knobs, MIDI over time; see `tests/host.c`), and a frame rate as a third argument writes the screen
as a film.

- `python3 tests/clock_test.py` checks the MIDI clock: tempo, jitter, a lost clock, Start and a tempo change.
- `python3 tests/pad_test.py` checks the pad: voice stealing and its fade, sustain, CC 74.
- `python3 tests/sub_test.py` checks the sub: pitch, mono and dry, glide, last-note priority, the split.
- `python3 tests/ui_test.py` checks the pages and knobs, saving, the user slots, the settings page, the latch,
  Program Change, the CC map and the sub's velocity.
- `python3 tests/mod_test.py` checks the duck's timing at 140, 174 and 200 BPM, Stop, and the LFOs' periods and phase.
- `python3 tests/fx_test.py` checks the FX: chorus width, delay times, reverb decay, the clip, CC 91 and CC 93.
- `tests/run_host.sh tests/patches.txt` plays the patches in turn.

## Onto the FM-1

```sh
./build.sh                                         # -> build/ethereal.fwsc
python3 tools/fm1_install.py build/ethereal.fwsc   # over USB; asks before it writes
python3 tools/fm1_install.py --info                # what runs on the connected FM-1
./build.sh --release 0.1                           # -> build/ethereal-0.1.fwsc, identity FM-1_941
```

`./build.sh` also checks the chip's rules ([RULES.md](RULES.md)): no float, no 64-bit division, register access
only under `hal/`, and the memory budget. If a build will not start, hold **OCT-** while switching on for the USB
rescue, then install a working build.

## On the bench

`python3 tests/device_test.py` runs on the FM-1 itself over USB: notes in over its USB-MIDI, the sound back over its
USB audio. It checks the sub and, with `./build.sh --diag` installed, the clock lock from what the FM-1 reports, from a
clock over USB and from a Tako sampler on TRS (`TAKO=` its repo, if not beside this one).
