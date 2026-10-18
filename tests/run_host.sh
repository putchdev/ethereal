#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/ethereal.c on this computer (tests/host.c): build/host/ethereal.wav and the screens as PNG.
# Extra arguments: a script file (see tests/host.c).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wno-unused-function -DETHEREAL_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/src -Ifirmware/gen -o build/host/ethereal firmware/src/ethereal.c
build/host/ethereal build/host/ethereal "$@"
python3 - <<'PY'
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob("ethereal-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
