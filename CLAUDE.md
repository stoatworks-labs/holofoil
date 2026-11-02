# holofoil

Trading-card holographic foil for Resolume Arena/Avenue, as an FFGL effect.

- **What it does:** each pixel of the clip is a point on a printed card with a foil under it. The foil is a diffraction grating under a lamp, and the colour comes from the grating equation, through the CIE 1931 matching functions.
- **Finishes:** holo (Art Window), reverse holo (Around Art), full foil (Whole Card), and foil keyed by the print (Bright Ink, Dark Ink, Clip Alpha).
- **Patterns:** Rainbow, Crosshatch, Cosmos, Cracked Ice and Sparkle.
- **Build:** C++/GLSL, CMake MODULE → universal `.bundle` (macOS).
- **Identity:** MIT. ID `HF01`, display name `SW Holofoil`.
- **Status:** public at github.com/stoatworks-labs/holofoil since 2026-11-02; v0.1.0 is being released.

Read `AGENTS.md` before changing the optics, the colour table, the lamp's covariance, a default or any tolerance in the harness. The spec is `~/Projects/resolume/specs/SPEC-holofoil.md`.

## Commands (CMake)

- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects)
- Render offline: `./build/hftest --out /tmp/frame.png`. That is the card, 90 frames in. Add `--frozen` to hold the art and `--black` for a black clip.
- List parameters: `./build/hftest --list`
- Set anything by name: `./build/hftest --set "Foil Area=Around Art" --set "Pattern=Cracked Ice"`
- Film: `./build/hftest --film 300 --size 720x1008 | ffmpeg -f rawvideo -pix_fmt rgba -s 720x1008 -r 60 -i - out.mp4`
- A clip through it: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/hftest --pipe --size WxH | ffmpeg …`
- The README's figures: `python3 tools/figures.py` (Pillow and ffmpeg)

## Verify

- **Everything:** `tools/verify.sh`, about 8 minutes on an M4 Max. It runs:
  - a fresh universal build
  - every check at 630×882 and 320×180, and again on Apple's software renderer
  - the negative controls and the 29 mutants
  - the sweep
  - lipo, plist, ad-hoc signature
  - `oxbow probe` and `selftest`
  - the bench

  `OXBOW=` points at an oxbow binary if `~/Projects/resolume/oxbow/build/oxbow` isn't built.
- **The optics:**
  - `--grating`: per-wavelength light, and the dominant wavelength against *d*|s·g|/*m*
  - `--mirror`: the highlight at the mirror point
  - `--groove`: J₀² in the mirror
  - `--lamp`: purity against lamp size; roughness as a wider, dimmer lamp
  - `--patterns`: each pattern's statistics, and the light from what it embossed
- **The print and the area:**
  - `--complement`: holo + reverse = clip + whole foil
  - `--window`: exact coverage
  - `--luma`: linear-light luminance keying
  - `--ink`: the filter model and Mix
  - `--passthrough`: bit for bit
- **The rest:** `--smooth`, `--clock` and `--state`. No GL needed: `--motion`, `--controls`, `--cues` and `--names`.
- **The checks can fail:** `--negative` (17 wrong models) and `tools/mutate.sh` (29 mutants; `tools/mutate.sh TEXT` runs only the ones whose description contains TEXT).
- **No dead controls:** `python3 tools/sweep.py` sweeps 31 controls, all live. `--bare` lists the three that only act in context.
- **Render cost:** `./build/hftest --bench`.
- **CI** (`.github/workflows/ci.yml`): macOS arm64 build, `hftest --list` and `--offline`, glslc; a Windows x64 build through vcpkg's GLEW. No pixel checks: the runner has no GL.

## Notes

- **The optics live in the shader.** `sheenAt()` in `Shaders.cpp` has the derivation in its comments. `Card.cpp` has the frame, the lamp's covariance and the motion.
- **The colour table** (2048 × 129 RGBA32F) is rebuilt only when Groove Depth changes. The harness counts the rebuilds (`--state`).
- **The harness's reference never shares the plugin's shortcuts.** It sums the light per wavelength in double, without the table, the lines or the along/across algebra. Keep it that way: a reference built from the plugin's own functions passes every mutant.
- **Ratios hide common factors.** Anchor at least one side of every comparison to an absolute reference (AGENTS.md, "The recorded GLSL mutation").
- **Apple's software renderer does not repeat a draw bit for bit.** Expect differences up to ~6e-6. A cross-draw claim goes through `drawsAgreeWithin()`, and pixels are classified by the passed-through clip, never by a tolerance.
- **Test hooks:** `uniform int Hooks` ships, so the harness measures the real shader. Its bits are in `Shaders.h` and must be repeated in `kCommon`.
- **No trig in the GLSL.** Directions come from the CPU; a cell's random direction comes from `cosSinTurns()`. `verify.sh` greps for trig. Randomness is PCG, never `sin`.
- **Passthrough uses `texelFetch`** and an early return, so Mix 0 and pixels outside the area are bit-exact.
- **Host parameters:** all are 0..1, mapped in `Controls.cpp`, and options hold the element index. `--controls` checks every mapping against the numbers in `Controls.h`, written into the harness as literals.
- **GLSL reserved words** (`patch sample input output filter common active half layout flat packed`, `noise1`–`noise4`) must not be identifiers. `verify.sh` greps for them.
- **Override `SetTextParameter`** to return `FF_SUCCESS` for the About block, or no host can instantiate the plugin.
- **`holofoil_core` is an OBJECT library:** the plugin registers itself from a file-scope constructor that nothing references.
- **The macOS build must be universal.** Check with `lipo`, never the build log.
- **No trademarks** in the plugin's name, labels, options or any future preset names.
- **Generated files:** `source/StoatworksAbout.h` (backend `scripts/sync-about.py`), `ATTRIBUTIONS.md` (`scripts/sync-attributions.py`), `.github/ISSUE_TEMPLATE/` (`scripts/sync-issue-templates.py`) and `scripts/release-lib.sh` (the backend's `release/`). Edit them there, never here.

## Not done yet

- Never loaded into Resolume. The Windows build compiles in CI and nowhere else. No presets or demo.

## Diagnostics

`source/Diag.{h,cpp}` writes a log file only, with no crash handler (this runs inside Resolume).

    ~/Library/Logs/holofoil/holofoil.YYYY-MM-DD.log
    %LOCALAPPDATA%\holofoil\logs\holofoil.YYYY-MM-DD.log

`HOLOFOIL_LOG_DIR` overrides the folder.
