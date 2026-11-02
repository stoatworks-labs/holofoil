"""Render the README's figures with hftest: every finish on the harness's
card at three tilts, and a short film of the card tipping.

Each still is the card held (Motion Hold, the art frozen) at Tilt Y 0, +5 and
+10 degrees under the default strip lamp, 35 degrees up, three picture widths
off; everything else is the defaults unless the finish's row says. The two
crosshatches are at twice their natural scale, so their lines are 4 pixels
wide at this raster and not aliased; cold foil and etched, which are mostly
mirror, are under a lamp level with the eye and 5 degrees wide, where their
mirror sheen crosses the card at these tilts. The film
is four finishes side by side, tipping as the defaults have it (Tip, 12
degrees, a cycle every 5 s), one cycle at 60 fps.

Needs Pillow and ffmpeg. Usage::

    python3 tools/figures.py [--build BUILD_DIR] [--out docs]
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

REPO = pathlib.Path(__file__).resolve().parent.parent

W, H = 360, 504
TILTS = [("0", "0.5"), ("+5", "0.5625"), ("+10", "0.625")]

# label -> settings
CARD = [
    ("holo", []),
    ("reverse holo", ["Foil Area=Around Art"]),
    ("full foil", ["Foil Area=Whole Card"]),
    ("shiny (sparkle)", ["Foil Area=Whole Card", "Pattern=Sparkle", "Facet Tilt=0.4", "Roughness=0.2"]),
]
TEXTURES = [
    ("cosmos", ["Pattern=Cosmos"]),
    ("cracked ice", ["Pattern=Cracked Ice", "Facet Tilt=0.4"]),
    ("crosshatch", ["Pattern=Crosshatch", "Pattern Scale=0.75"]),
    ("square crosshatch", ["Foil Area=Whole Card", "Pattern=Crosshatch", "Pattern Scale=0.75", "Grating Angle=0.25"]),
    ("cold foil", ["Foil Area=Bright Ink", "Groove Depth=0.15", "Lamp Y=0.5", "Lamp Size=0.7"]),
    ("etched", ["Foil Area=Bright Ink", "Pattern=Sparkle", "Roughness=0.4", "Groove Depth=0.25", "Lamp Y=0.5", "Lamp Size=0.7"]),
]
FILM = [
    ("holo", []),
    ("reverse holo", ["Foil Area=Around Art"]),
    ("cracked ice", ["Pattern=Cracked Ice", "Facet Tilt=0.4"]),
    ("shiny (sparkle)", ["Foil Area=Whole Card", "Pattern=Sparkle", "Facet Tilt=0.4", "Roughness=0.2"]),
]


def still(hftest, out, settings, tilt):
    args = [str(hftest), "--out", str(out), "--frozen", "--size", f"{W}x{H}", "--set", "Motion=Hold", "--set", f"Tilt Y={tilt}"]
    for s in settings:
        args += ["--set", s]
    subprocess.run(args, check=True, capture_output=True)
    return Image.open(out).convert("RGB")


def font(size):
    for name in ("/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def sheet(hftest, scratch, rows, path):
    label_w, head_h, pad = 220, 34, 8
    big, small = font(20), font(16)
    img = Image.new("RGB", (label_w + 3 * (W + pad), head_h + len(rows) * (H + pad)), (24, 26, 32))
    draw = ImageDraw.Draw(img)
    for c, (deg, _) in enumerate(TILTS):
        draw.text((label_w + c * (W + pad) + W // 2 - 50, 8), f"Tilt Y {deg}°", fill=(220, 220, 220), font=big)
    for r, (label, settings) in enumerate(rows):
        y = head_h + r * (H + pad)
        draw.text((10, y + H // 2 - 30), label, fill=(235, 235, 235), font=big)
        for i, s in enumerate(settings):
            draw.text((10, y + H // 2 + i * 18), s, fill=(150, 150, 160), font=small)
        for c, (_, tilt) in enumerate(TILTS):
            img.paste(still(hftest, scratch / f"s{r}-{c}.png", settings, tilt), (label_w + c * (W + pad), y))
    img.save(path)
    print(f"wrote {path}")


def film(hftest, path, frames=300):
    raws = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for i, (_, settings) in enumerate(FILM):
            raw = tmp / f"f{i}.rgba"
            args = [str(hftest), "--film", str(frames), "--size", f"{W}x{H}"]
            for s in settings:
                args += ["--set", s]
            with open(raw, "wb") as f:
                subprocess.run(args, check=True, stdout=f)
            raws.append(raw)
        inputs = []
        for raw in raws:
            inputs += ["-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{H}", "-r", "60", "-i", str(raw)]
        stack = "".join(f"[{i}:v]" for i in range(len(raws))) + f"hstack=inputs={len(raws)},format=yuv420p[out]"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *inputs, "-filter_complex", stack, "-map", "[out]",
                        "-c:v", "libx264", "-crf", "18", "-movflags", "+faststart", str(path)], check=True)
    print(f"wrote {path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--out", type=pathlib.Path, default=REPO / "docs")
    args = parser.parse_args()
    hftest = (args.build if args.build.is_absolute() else REPO / args.build) / "hftest"
    if not hftest.exists():
        print(f"{hftest} not found", file=sys.stderr)
        return 1
    out = args.out if args.out.is_absolute() else REPO / args.out
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        sheet(hftest, scratch, CARD, out / "finishes.png")
        sheet(hftest, scratch, TEXTURES, out / "textures.png")
    film(hftest, out / "tip.mp4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
