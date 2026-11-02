"""The browser demo against the plugin: the same frames, two renderers, one picture.

    python3 demo/tools/crosscheck/crosscheck.py [--hftest build-verify/hftest] [--size 640x360] [--keep DIR]

Each case below is rendered twice, over the same 60 frames a sixtieth of a
second apart:

  - by the PLUGIN: `hftest --pipe`, the real plugin class in a GL 4.1 context
    (build it first; tools/verify.sh leaves one in build-verify/), with no beat
    sent (--bpm 0), so Motion Sync's Beat and Bar run on its own 120 BPM as the
    page's do;
  - by the PAGE: demo/ served from here and loaded in headless Chrome, through
    window.holofoilDemo.renderCase, which makes a fresh instance and runs the
    same frames on the same clock.

The frames are the harness's own moving card, taken from the plugin at Mix 0
(which hands its clip back to the bit), and fed to both. Both pictures are the
last frame, RGBA8, and they are compared pixel by pixel. A case passes when
99.9% of its pixels are within one level of 255 of the plugin's AND none is
more than four off: rounding between two GL stacks is a level here and there,
a port bug is a band or a cell in the wrong place. "lit" is the share of the
plugin's pixels more than eight levels from the card, so a case that shows no
foil cannot pass for one that matched; "same" is the share identical to the
bit.

The page's GPU half is the plugin's GLSL (check_shaders.py), so what this
mostly tests is the PORT: every mapping, the eye, the lamp and its spread in
the card's frame, the motion's phase, the colour matrix and the uniforms. A
difference is a port bug, or a difference between GL 4.1 on this machine and
WebGL2 through ANGLE on the same GPU. Two negative controls show the
comparison can see a little: the default motion one frame short, and a held
card tipped 0.08 degrees further than the plugin's.

The page's table is RGBA32F where the browser can filter 32-bit floats and
RGBA16F where it cannot. The last line renders the defaults and the held holo
with the half-float table forced, and says how far that is from the plugin: a
measurement of the fallback, not a pass or fail.

It also checks the page's control table against `hftest --list`: every name,
kind and default, in order.

Not run by tools/verify.sh: it needs a GPU and Chrome. Exit 1 if a case falls
under its threshold, a negative control is not caught, or a control differs.
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, "..", ".."))
REPO = os.path.abspath(os.path.join(DEMO, ".."))
FRAMES, FPS = 60, 60
sys.dont_write_bytecode = True

HELD5 = {"Motion": "Hold", "Tilt Y": "0.5625"}
# name, settings as `hftest --set` takes them: an option by its element's name,
# anything else as the host's 0..1. The finishes are the README's, at the
# numbers tools/figures.py renders them with.
CASES = [
    ("holo-defaults", {}),
    ("holo-held-5", dict(HELD5)),
    ("holo-held-10", {"Motion": "Hold", "Tilt Y": "0.625"}),
    ("reverse-held-5", {"Foil Area": "Around Art", **HELD5}),
    ("full-sparkle", {"Foil Area": "Whole Card", "Pattern": "Sparkle", "Facet Tilt": "0.4", "Roughness": "0.2"}),
    ("cosmos-held-5", {"Pattern": "Cosmos", **HELD5}),
    ("cracked-ice", {"Pattern": "Cracked Ice", "Facet Tilt": "0.4"}),
    ("square-crosshatch", {"Foil Area": "Whole Card", "Pattern": "Crosshatch", "Pattern Scale": "0.75",
                           "Grating Angle": "0.25", "Motion": "Hold", "Tilt Y": "0.625"}),
    ("cold-foil", {"Foil Area": "Bright Ink", "Groove Depth": "0.15", "Lamp Y": "0.5", "Lamp Size": "0.7", **HELD5}),
    ("etched", {"Foil Area": "Bright Ink", "Pattern": "Sparkle", "Roughness": "0.4", "Groove Depth": "0.25",
                "Lamp Y": "0.5", "Lamp Size": "0.7"}),
    ("orbit-beat-whole", {"Foil Area": "Whole Card", "Motion": "Lamp Orbit", "Motion Sync": "Beat", "Motion Rate": "0.7"}),
    ("near-swing-dark-ink", {"Foil Area": "Dark Ink", "Motion": "Swing", "Viewer Distance": "0.1", "Lamp Distance": "0.2",
                             "Grating Angle": "0.3", "Softness": "0.3", "Ink": "0.2", "Pitch": "0.7",
                             "Lamp Brightness": "0.8"}),
    ("circle-window-mix", {"Motion": "Circle", "Motion Depth": "0.6", "Window X": "0.4", "Window Y": "0.5",
                           "Window Width": "0.5", "Window Height": "0.6", "Corner": "0.3", "Softness": "0.2",
                           "Mix": "0.6", "Lamp Length": "0", "Lamp Size": "0.6", "Base": "0.2"}),
    ("alpha-hatch-magenta", {"Foil Area": "Clip Alpha", "Pattern": "Crosshatch", "Motion": "Hold", "Tilt X": "0.6",
                             "Grating Angle": "0", "Groove Depth": "0.55", "Lamp X": "0.45"}),
]
# The negative controls: (name, the case whose plugin picture it is compared
# with, the page's settings, the page's frame count).
NEGATIVES = [
    ("late", "holo-defaults", {}, FRAMES - 1),
    ("tipped", "holo-held-5", {"Motion": "Hold", "Tilt Y": "0.5635"}, FRAMES),
]
# The half-float table, measured: (name, the case it is compared with).
HALF = [("half-holo-defaults", "holo-defaults"), ("half-full-sparkle", "full-sparkle")]


def parse_size(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def run_hftest(hftest, args, stdin=None):
    done = subprocess.run([hftest, *args], input=stdin, capture_output=True, check=False)
    if done.returncode != 0:
        sys.exit(f"hftest {' '.join(args)} failed: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


def listed_controls(hftest):
    """`hftest --list` as (name, kind, default), up to the About block."""
    rows = []
    for line in run_hftest(hftest, ["--list"]).decode().splitlines()[1:]:
        parts = line.split()
        if len(parts) < 4:
            continue
        name = " ".join(parts[1:-2])
        if name == "About":
            break
        rows.append((name, parts[-2], parts[-1]))
    return rows


class Handler(http.server.SimpleHTTPRequestHandler):
    """demo/ as it is, plus /xc/ for the job and the page's answers."""

    def __init__(self, *args, state, **kwargs):
        self.state = state
        super().__init__(*args, directory=DEMO, **kwargs)

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/xc/job.json":
            return self.reply(json.dumps(self.state["job"]).encode(), "application/json")
        if self.path == "/xc/cards.rgba":
            return self.reply(self.state["cards"], "application/octet-stream")
        return super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/xc/out/"):
            self.state["page"][self.path[len("/xc/out/"):]] = body
        elif self.path == "/xc/controls":
            self.state["controls"] = json.loads(body)
        elif self.path == "/xc/table":
            self.state["table"] = body.decode(errors="replace")
        elif self.path == "/xc/error":
            self.state["error"] = body.decode(errors="replace")
            self.state["finished"].set()
        elif self.path == "/xc/done":
            self.state["finished"].set()
        self.reply(b"ok", "text/plain")

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def compare(a, b, width, height):
    x = np.frombuffer(a, np.uint8).reshape(height, width, 4).astype(np.int16)
    y = np.frombuffer(b, np.uint8).reshape(height, width, 4).astype(np.int16)
    worst = np.abs(x - y).max(axis=2)
    return {
        "same": float((worst == 0).mean() * 100.0),
        "within1": float((worst <= 1).mean() * 100.0),
        "within4": float((worst <= 4).mean() * 100.0),
        "max": int(worst.max()),
        "mean": float(np.abs(x - y)[..., :3].mean()),
    }


def lit(out, card, width, height):
    x = np.frombuffer(out, np.uint8).reshape(height, width, 4).astype(np.int16)
    y = np.frombuffer(card, np.uint8).reshape(height, width, 4).astype(np.int16)
    return float((np.abs(x - y)[..., :3].max(axis=2) > 8).mean() * 100.0)


def check_controls(hftest, page):
    problems = 0
    listed = listed_controls(hftest)
    if page is None:
        print("controls: the page sent none")
        return 1
    if len(listed) != len(page):
        print(f"controls: hftest lists {len(listed)}, the page has {len(page)}")
        problems += 1
    for (name, kind, default), row in zip(listed, page):
        mine = (row["name"], row["kind"], f"{row['default']:.4f}")
        if mine != (name, kind, default):
            print(f"controls: hftest says {name} {kind} {default}, the page {mine[0]} {mine[1]} {mine[2]}")
            problems += 1
    if not problems:
        print(f"controls: all {len(listed)} names, kinds and defaults are hftest --list's, in its order")
    return problems


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--hftest", default=os.path.join(REPO, "build-verify", "hftest"))
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--keep", help="write each case's two pictures here as PNGs")
    parser.add_argument("--threshold", type=float, default=99.9,
                        help="share of pixels, %%, that must be within one level of 255 (default 99.9)")
    parser.add_argument("--worst", type=int, default=4,
                        help="levels of 255 no pixel may differ by (default 4)")
    parser.add_argument("--chrome", default=os.environ.get(
        "CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))
    parser.add_argument("--port", type=int, default=8794)
    args = parser.parse_args(argv)
    width, height = parse_size(args.size)
    if not os.access(args.hftest, os.X_OK):
        sys.exit(f"no hftest at {args.hftest}: build it (cmake -B build-verify && cmake --build build-verify --target hftest)")

    size = f"{width}x{height}"
    common = ["--size", size, "--fps", str(FPS), "--bpm", "0"]
    cards = run_hftest(args.hftest, ["--film", str(FRAMES), *common, "--set", "Mix=0"])
    frame_bytes = width * height * 4
    if len(cards) != FRAMES * frame_bytes:
        sys.exit(f"hftest --film gave {len(cards)} bytes, not {FRAMES} frames")
    last_card = cards[-frame_bytes:]

    plugin = {}
    for name, settings in CASES:
        sets = [arg for key, value in settings.items() for arg in ("--set", f"{key}={value}")]
        out = run_hftest(args.hftest, ["--pipe", *common, *sets], stdin=cards)
        if len(out) != FRAMES * frame_bytes:
            sys.exit(f"hftest --pipe for {name} gave {len(out)} bytes, not {FRAMES} frames")
        plugin[name] = out[-frame_bytes:]

    cases = [{"name": name, "settings": settings, "count": FRAMES} for name, settings in CASES]
    cases += [{"name": name, "settings": settings, "count": count} for name, _, settings, count in NEGATIVES]
    cases += [{"name": name, "settings": dict(CASES)[against], "count": FRAMES, "half": True} for name, against in HALF]
    state = {"job": {"width": width, "height": height, "fps": FPS, "cases": cases},
             "cards": cards, "page": {}, "controls": None, "table": None, "error": None,
             "finished": threading.Event()}

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), functools.partial(Handler, state=state))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="holofoil-chrome-")
    chrome = subprocess.Popen([
        args.chrome, "--headless=new", "--no-first-run", "--no-default-browser-check",
        f"--user-data-dir={profile}", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist",
        "--window-size=1600,1200", f"http://127.0.0.1:{args.port}/tools/crosscheck/",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not state["finished"].wait(300):
            sys.exit("the page never finished (300 s)")
    finally:
        chrome.terminate()
        try:
            chrome.wait(10)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    if state["error"]:
        sys.exit(f"the page failed:\n{state['error']}")

    if args.keep:
        from PIL import Image
        os.makedirs(args.keep, exist_ok=True)

    failures = check_controls(args.hftest, state["controls"])
    print(f"the page's colour table: {state['table']}\n")
    print(f"{size}, {FRAMES} frames at {FPS} fps, the last compared\n")
    print(f"{'case':<22} {'lit':>7} {'same':>9} {'<=1':>9} {'<=4':>9} {'max':>4} {'mean':>7}")
    for name, _ in CASES:
        got = state["page"].get(name)
        if got is None or len(got) != frame_bytes:
            print(f"{name:<22} no picture from the page")
            failures += 1
            continue
        s = compare(plugin[name], got, width, height)
        ok = s["within1"] >= args.threshold and s["max"] <= args.worst
        failures += 0 if ok else 1
        shown = lit(plugin[name], last_card, width, height)
        print(f"{name:<22} {shown:6.1f}% {s['same']:8.3f}% {s['within1']:8.3f}% {s['within4']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  {'ok' if ok else 'FAIL'}")
        if args.keep:
            for label, data in (("plugin", plugin[name]), ("page", got)):
                Image.frombytes("RGBA", (width, height), data).save(os.path.join(args.keep, f"{name}-{label}.png"))

    print()
    for name, against, _, count in NEGATIVES:
        got = state["page"].get(name)
        s = compare(plugin[against], got, width, height) if got else None
        caught = s is not None and (s["within1"] < args.threshold or s["max"] > args.worst)
        failures += 0 if caught else 1
        print(f"negative control {name!r} against {against}: "
              + (f"{s['within1']:.3f}% within one level, max {s['max']}, {'caught' if caught else 'NOT CAUGHT'}" if s else "no picture"))
    print()
    for name, against in HALF:
        got = state["page"].get(name)
        s = compare(plugin[against], got, width, height) if got else None
        print(f"RGBA16F table, {against}: "
              + (f"{s['same']:.3f}% the same, {s['within1']:.3f}% within one level, max {s['max']}" if s else "no picture"))
    print(f"\n{'all cases within threshold' if not failures else f'{failures} problem(s)'} "
          f"({args.threshold}% within one level, none more than {args.worst} off)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
