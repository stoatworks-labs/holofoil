"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. Every render is 150 frames (2.5 s at
60 fps) of the harness's card at 120 BPM, tipping as the defaults have it.
Pattern Scale and Facet Tilt act only on a pattern of cells (Cracked Ice
here); Threshold only where the foil follows the ink (Bright Ink); Base only
under some Ink, which the defaults have; Motion Depth, Rate and Sync only
while the card moves, which the defaults have. An option parameter reads back
0..1 whatever its count (the fleet's trap), so options are set here by
element index. A control whose ends differ by less than `--floor` (mean 8-bit
difference per channel) is reported as barely alive: vectrix's lesson.

Every render is 320x180.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
honesty check: what goes dead there is the list of controls that only act
with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

CELLS = ["Pattern=3"]
INK = ["Foil Area=3"]

# name -> (low settings, high settings, context settings)
SWEEP = {
    "Foil Area": (["Foil Area=0"], ["Foil Area=2"], []),
    "Pattern": (["Pattern=0"], ["Pattern=3"], []),
    "Tilt X": (["Tilt X=0.4"], ["Tilt X=0.6"], []),
    "Tilt Y": (["Tilt Y=0.4"], ["Tilt Y=0.6"], []),
    "Motion": (["Motion=0"], ["Motion=1"], []),
    "Motion Depth": (["Motion Depth=0.2"], ["Motion Depth=0.5"], []),
    "Motion Rate": (["Motion Rate=0.3"], ["Motion Rate=0.6"], []),
    "Motion Sync": (["Motion Sync=0"], ["Motion Sync=1"], []),
    "Viewer Distance": (["Viewer Distance=0.3"], ["Viewer Distance=0.8"], []),
    "Lamp X": (["Lamp X=0.4"], ["Lamp X=0.6"], []),
    "Lamp Y": (["Lamp Y=0.6"], ["Lamp Y=0.8"], []),
    "Lamp Distance": (["Lamp Distance=0.3"], ["Lamp Distance=0.9"], []),
    "Lamp Size": (["Lamp Size=0.3"], ["Lamp Size=0.7"], []),
    "Lamp Length": (["Lamp Length=0"], ["Lamp Length=1"], []),
    "Lamp Brightness": (["Lamp Brightness=0.2"], ["Lamp Brightness=0.6"], []),
    "Pitch": (["Pitch=0.2"], ["Pitch=0.5"], []),
    "Grating Angle": (["Grating Angle=0.3"], ["Grating Angle=0.6"], []),
    "Groove Depth": (["Groove Depth=0.1"], ["Groove Depth=0.6"], []),
    "Pattern Scale": (["Pattern Scale=0.3"], ["Pattern Scale=0.7"], CELLS),
    "Facet Tilt": (["Facet Tilt=0"], ["Facet Tilt=0.6"], CELLS),
    "Roughness": (["Roughness=0"], ["Roughness=0.6"], []),
    "Ink": (["Ink=0"], ["Ink=1"], []),
    "Base": (["Base=0"], ["Base=1"], []),
    "Window X": (["Window X=0.4"], ["Window X=0.6"], []),
    "Window Y": (["Window Y=0.5"], ["Window Y=0.7"], []),
    "Window Width": (["Window Width=0.5"], ["Window Width=0.8"], []),
    "Window Height": (["Window Height=0.3"], ["Window Height=0.5"], []),
    "Corner": (["Corner=0"], ["Corner=1"], []),
    "Softness": (["Softness=0"], ["Softness=0.5"], []),
    "Threshold": (["Threshold=0.3"], ["Threshold=0.7"], INK),
    "Mix": (["Mix=0"], ["Mix=1"], []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
# Unpublished, the block has the text line and Support the work alone.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for hftest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(hftest, out, settings):
    args = [str(hftest), "--out", str(out), "--size", "320x180", "--frames", "150"]
    for setting in settings:
        args += list(setting) if isinstance(setting, list) else ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hftest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(hftest):
    result = subprocess.run([str(hftest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hftest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(hftest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(hftest, scratch / f"{tag}-a.png", context + low)
    after = render(hftest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    hftest = build / "hftest"
    if not hftest.exists():
        print(f"{hftest} not found", file=sys.stderr)
        return 1

    declared = parameters(hftest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(hftest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
