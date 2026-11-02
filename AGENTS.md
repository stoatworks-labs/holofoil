# AGENTS.md: Holofoil

Read this before you change the optics, the colour table, the lamp, a tolerance in the harness or a default.

## The idea

The clip is a printed card with holographic foil under the print. The foil is a reflection grating. Light of wavelength λ reaches the eye off it in order *m* when the in-plane part of (v + l) is *m*λ/*d* along the grating's direction *g*, and zero across it. That is the grating equation in direction cosines (Harvey and Vernold).

Everything the plugin draws is that equation, per pixel, in linear light, for an extended lamp:

1. **The lamp is a Gaussian in direction,** with covariance **S** (2×2, direction cosines in the card's plane). It is round (Lamp Size) or a strip (× Lamp Length), with the long axis level in the room. `LampSpread()` in `Card.cpp` builds S. It projects the lamp's two extents, carried into the card's frame, onto the surface.
2. **Each order is a Gaussian spectral line.** Light of wavelength λ left the lamp along *t g*, *t* = *m*λ/*d*, through s = (v + l)∥. The eye therefore sees the lamp's Gaussian along that line:
   - centre λ = *d* (gᵀS⁻¹s)/(gᵀS⁻¹g)/*m*
   - width *d* / (√(gᵀS⁻¹g) *m*)
   - overall weight exp(−½ (n·s)²/(nᵀSn)), with n across g.

   The derivation is in `sheenAt()` in `Shaders.cpp`.
3. **A table holds every such line's colour.** It is 2048 × 129 RGBA32F, the table pass in `Shaders.cpp`:
   - **Columns:** the centre, warped as u = ½ + ½c/(|c| + 600), so that negative orders and the far reaches both fit.
   - **Rows 0–127:** the width, geometric from 0.5 nm over a ratio of 4000, 64 rows for each of orders 1 and 2.
   - **Row 128:** the mirror's colour, the whole spectrum × J₀².

   Each entry is the line through the Wyman–Sloan–Shirley fit and the groove's Raman–Nath efficiency J_m²(2πh/λ), integrated with 96 taps over ±6σ. It is built on the GPU only when Groove Depth (or the NoEfficiency test hook) changes.
4. **Order 0 is the mirror:** the lamp's own Gaussian at s. It is white on bare foil and J₀²-tinted under a grating.
5. **Patterns set g, d and the surface's lean per region** (Toisoul et al.'s idea):
   - Rainbow is one grating.
   - Crosshatch is two line families at ±45° to Grating Angle, with bare mirror between.
   - Cosmos is discs, each with its own grating, on the plain grating.
   - Cracked Ice is Voronoi shards.
   - Sparkle is tiny square cells.

   A cell's grating comes from a PCG hash: direction uniform over half a turn, pitch 0.8–1.25×, lean uniform over the disc of slopes up to Facet Tilt.
6. **Roughness widens the lamp.** A normal spread σ widens the reflected cone by h_z σ, so S′ = S + (h_z σ)²I, and the light is dimmed by √(det S/det S′).
7. **Ink and area come last:**
   - foil = C (1 − Ink + Ink·Base) + sheen (1 − Ink + Ink·C)
   - lit = mix(C, foil, area × Mix), in linear light
   - The areas are the window's coverage, its complement, the ink's luminance (Rec. 709 on linear light, encoded back) against Threshold, or alpha.

## Decisions taken without asking

- **Name and id.** `SW Holofoil`, `HF01`, bundle `com.stoatworks.ffgl.holofoil`, harness `hftest`.
  - Not `foil`: `~/Projects/presentation/foil` exists.
  - HF01 was checked free against all 136 ids in `~/Projects/*/*/source` and `~/dev`.
  - Built in `~/dev/holofoil`, then moved to `~/Projects/resolume/holofoil` (slitscan's route) and made public at github.com/stoatworks-labs/holofoil the same day, on Allan's go-ahead.
- **Effect, not mixer; not lenticular.** Lenticular (LN01) is a mixer that picks one of two layers through a lens sheet. Holofoil is an effect on one clip and throws colour from a grating.
  - Shared with lenticular: the card stays put while the eye and lamp move round it, distances are in picture widths, and the eye is at (cos Ty sin Tx, sin Ty, cos Ty cos Tx).
  - The code is new.
- **No trademarks.** Plugin name, labels and option names are generic: Rainbow, Crosshatch, Cosmos, Cracked Ice, Sparkle, Art Window, Around Art.
  - The README's finish table uses collectors' generic terms (holo, reverse holo, cold foil, etched), never a game's name or a rarity's brand name.
  - The figure row for the horizontal-and-vertical crosshatch is called "square crosshatch".
- **Nothing from pokemon-cards-css** (GPL-3.0). Its approach is screen-space CSS blending, not optics, and nothing was needed from it.
- **The lamp is a strip by default.** A 1-D grating under a point or round lamp lights only the pixels where s runs along g: a streak (Toisoul §4.3).
  - The first version, under a round lamp overhead with the card rocking sideways, showed almost nothing.
  - A strip lamp running across the room is how foil is usually seen (a ceiling tube, a window).
  - The defaults are Lamp Length 8, so 2° × 16°. The long axis is capped at 0.866 (60° either side) so S stays a sensible Gaussian.
- **The default motion is Tip** (toward and away, about Tilt Y, 12°, 0.2 Hz). It sweeps the grating's colour along its direction (up the card, lines across) under that lamp. Swing (side to side) is there, but under the default lamp it mostly moves the streak.
- **Colour.**
  - **Matching functions:** the WSS multi-lobe fit (no tables to ship).
  - **sRGB conversion:** the IEC matrix, **white-balanced on the fit's own equal-energy white**, so a flat spectrum is white rather than the pink (1.20, 0.95, 0.91) the bare matrix gives.
  - **Out-of-gamut colour:** handled by **adding white** until the lowest channel is zero. That moves a colour straight toward white, so its dominant wavelength against the fit's white is kept, which is what the hue check measures.
  - **No clamping:** light above 1 is passed to the host as it is.
- **Orders 0, ±1, ±2 only.** Order 3 begins at *d*·s ≥ 1.2 µm: near grazing at the default 900 nm, in view at coarse pitches. The spec says so.
- **Efficiency: Raman–Nath (thin sinusoidal grating), not RCWA.** It gives the right shape (the shallow grooves are bluish in the first order, warm in the mirror, magenta near 210 nm). Its absolute level is the ideal one; real embossed foil does worse, and Lamp Brightness stands in for that.
- **Ink is a filter crossed twice:** foil × C. This follows the print literature ("the lighter areas are more reflective"; a process ink on metal tints it).
  - Base is how much of the paper's own white the foil replaces under the ink.
  - The defaults are Ink 0.6, Base 0.7, Brightness 1.5, chosen by eye on the harness card: the print stays readable and the foil shows.
- **The window and complement.**
  - The window is a rounded box whose coverage is a one-pixel ramp of the SDF, exact for straight edges.
  - Around Art is 1 − that coverage, so it is the complement to the float.
  - Default window: 0.84 × 0.42 at (0.5, 0.64), matching the harness card's art box.
- **Passthrough is bit-exact.** Where the area is 0 or Mix is 0, the shader returns `texelFetch` of the clip and skips linear light, the table and the encode.
- **No trig in the GLSL.** The grating's and the crosshatch's directions and tan(Facet Tilt) come from the CPU. A cell's random direction uses `cosSinTurns()`, a quarter-turn reduction plus a Taylor series good to 2e-9. `verify.sh` greps for trig.
- **PCG hash, not `fract(sin(...))`:** the same cells on every GPU.
- **The test hooks ship.** `uniform int Hooks` and its branches are in the shipped shader, so the harness measures the real one. Every branch is uniform, and the cost is in the bench (0.53 ms at 4K with them).
- **CI has no GL.** `.github/workflows/ci.yml` builds for macOS (arm64) and Windows (x64, vcpkg's GLEW). The macOS runner cannot make an accelerated GL context, so it runs `hftest --list`, `hftest --offline` and `tools/glslc.sh`; the pixel checks run in `tools/verify.sh`, on a Mac.
- **The About block** has no guide, page or repo link while unpublished (`StoatworksAbout.h` says so). It shows the text line and Support the work, 33 parameters in all. Regenerate it with the backend's `sync-about.py` when the repo goes public.

## The traps actually hit

- **The streak.** Described above: sideways rocking under a round lamp overhead showed nothing. The physics was right; the defaults were wrong.
- **The dominant wavelength is a delicate measurement.** The hue check first disagreed by 10–110 nm, from four separate causes:
  1. The fit's white is not (⅓, ⅓). Dominant wavelength is now taken against the fit's own equal-energy white.
  2. The ends of the locus are ill-conditioned: a tiny chromaticity error is tens of nm. The check covers 470–600 nm only.
  3. The fit's locus curls back past ~650 nm. The search runs 400–650.
  4. A wide line's centroid is pulled toward where x̄+ȳ+z̄ is larger. The bound is 0.25 + 0.044 σ_λ² nm, where 0.044 is the steepest d ln(x̄+ȳ+z̄)/dλ over 470–600, computed separately.
- **The window's total area could not be bounded by π/12 of a pixel per corner.** Lattice sampling of a curved edge errs by up to 0.63 px². The check now integrates each pixel's exact coverage over 2048 strips and bounds per pixel:
  - straight edges 1e-4
  - corners 0.044 + √2³/(12 r): the ramp's worst on a 45° edge, plus curvature
- **Purity below a degree saturates.** At 0.2° and 0.5° the line is narrower than sRGB can show, and both clip to the same gamut edge. The lamp check uses 1–16°.
- **A mirror pose off the frame.** One `--mirror` pose put the highlight outside 320×180 and fitted noise. All poses are inside both rasters now.
- **The software renderer builds the table in seconds,** about 6 s at 96 taps (10 s at 256).
  - Each check used to create a plugin instance per case: 60 for passthrough, a table build each.
  - They now keep one instance per raster (`Rigs`) and set every control they use before each render.
  - The whole software pass takes about 5 minutes.
- **A ratio cannot see a common factor.** The recorded mutation is below.
- **Patterns are point-sampled.** At its natural scale (0.012 widths, 8.6 px at 720 wide) the crosshatch aliases on small rasters. The figures use Pattern Scale ×2. No anti-aliasing was added in 0.1.0.
- **A mostly-mirror foil shows nothing away from the lamp.**
  - Cold foil (Groove ~60 nm) under the default lamp, 35° up, is blank at Tilt Y 0–10°: the mirror angle is near −17°.
  - Real cold foil looks "icy" because it reflects the room. Holofoil has one lamp and no room.
  - The figures light cold foil and etched with a lamp level with the eye, 5° wide.
- **Apple's software renderer does not repeat a draw bit for bit.**
  - The first `verify.sh` failed `--complement` there: the bitwise "where one is the clip, the other is the whole card's" did not hold. The count of "part-covered" pixels changed from run to run (4570, 4800, 5051, 5218, 5281), although it is fixed on the GPU.
  - **Measured:** with nothing changed, the first draws after the program is linked differ from later ones by up to 5.7e-6 (84 float steps), in blocks of rows, by a different amount each run. That is consistent with the renderer swapping in a differently compiled shader part way through; that cause is inferred, not confirmed.
  - **The fix is in the harness, not the plugin.** Which side is the clip is read off the passed-through pixels, which are bit-exact on every renderer. A claim that two *draws* agree is bit for bit on the GPU and within 2e-5 on the software renderer (`drawsAgreeWithin()`). Each check prints which it held to, and the worst gap.
  - A first attempt that also classified "the whole card's" by tolerance misfiled pixels of coverage 0.99999 as whole. Do not classify by a tolerance.

## Verified, and assumed

**Verified** on this Mac (the numbers are in the README's Status and the spec):

- each pixel's light matches the grating equation wavelength by wavelength, and its hue is the grating's
- the mirror point
- J₀² in the mirror
- roughness ≡ a wider, dimmer lamp
- each pattern's statistics, and its light
- the complements, to the float (bit for bit on the GPU where one side is the clip; within 2e-5 on the software renderer, for the reason above)
- the window's coverage
- the luminance keying and the ink model
- bit-exact passthrough
- smooth tilt
- the clock's unit and origin
- GL state
- every slider's range
- a stand-in host (oxbow) loads it, reads 33 parameters and renders 120 frames

**Assumed, not measured:**

- That this is what a real card looks like. No real foil card has been photographed and compared. The optics are textbook; the finish recipes (pitch, depth, pattern, cell size) come from generic hologram literature (dot-matrix pixels 65 µm, 1000–1300 lines/mm, 120–210 nm deep), not from trading cards.
- That Raman–Nath's shape is close enough, and that one lamp is enough.
- That Resolume calls it as oxbow does. It has never been loaded in Resolume.
- That the Windows build compiles. It has not been tried.

## Would this hold on another rasteriser, at another raster?

- **Another rasteriser.** Every picture check also passes on Apple's software renderer at 320×180, a second GL implementation. Nothing checked depends on a filtered read except the table, which is read with `texture()` between texels on purpose and is checked against a reference that has no table at all.
- **Another raster.** Every check runs on a portrait card (630×882) and a landscape frame (320×180), so neither the window's fractions nor the optics' picture widths depend on the aspect.

## The negative controls and mutants

**Negative controls.** `hftest --negative` runs 17 wrong models. Thirteen are built into the real shader through `SetHooksForTest` (eleven distinct hooks, two used twice); two are switches in the plugin's C++ (absolute phase, snapped tilt); two are in the harness (the clock's unit, ramped cues). Every one must fail its check:

- the lamp subtracted (×2)
- the grating's direction ignored
- the lamp at infinity (×2)
- one grating angle everywhere
- the mirror keeping all its light
- roughness ignored
- an unsmoothed Around Art
- square corners
- luminance from code values
- the sheen added unfiltered
- a linear-light round trip
- an absolute motion phase
- a snapped tilt
- a millisecond clock taken as seconds
- ramped cues

**Mutants.** `tools/mutate.sh` changes one character of the shipped code and runs the one check that should see it. It makes 29 mutants: 21 in the GLSL, 8 in the C++. `tools/mutate.sh TEXT` runs only those whose description contains TEXT.

### The recorded GLSL mutation

The first full run, 2026-11-02, caught 28 of 29. The survivor:

    source/Shaders.cpp
    float gain  = sqrt( ( LampSpread.x * LampSpread.z - LampSpread.y * LampSpread.y ) / det );
                                                                                       ^ / -> *
    == mutant: GLSL: the lamp's area in the wrong place, every pixel scaled by its determinant
       FAIL  --lamp still PASSES -- the check does not cover this code

**Why it survived.** `--lamp` compared a rough foil under a small lamp with a smooth foil under the equivalent wider lamp, scaled by the ratio of their areas. The mutant multiplies every render by a determinant (about 1e-7 for a degree-wide lamp). Both renders therefore went black together, and their ratio was still exactly right.

**The fix.** `--lamp` now also checks the rough render's absolute light against the spectrum's, wavelength by wavelength. On the rerun:

    == mutant: GLSL: the lamp's area in the wrong place, every pixel scaled by its determinant, / -> *
       ok    --lamp fails against the mutant (exit 1):
              FAIL  Roughness 1 degrees under a 1 degree lamp ... worst 1.7e-07 of 0.000 (bound 1e-5); against the spectrum's ...

`--grating` would have caught it too, since it checks absolute light. But the mutant names `--lamp`, and a check should catch what it claims to cover. The lesson stands for any ratio-only comparison: anchor one side to an absolute reference.

## The sweep's honest list

`tools/sweep.py` sweeps all 31 controls, each where it acts. Pattern Scale and Facet Tilt are swept on Cracked Ice, Threshold on Bright Ink; the rest use the defaults, which already tip the card and use Ink 0.6. All 31 are live.

`--bare` (no context) reports these as dead, and that run exits 1 on purpose:

- **Pattern Scale** and **Facet Tilt:** Rainbow has no cells.
- **Threshold:** the art window ignores the ink.

The weakest live controls are Motion Rate (0.15 mean 8-bit at 320×180), Corner (0.18, swept 0 to 1) and Softness (0.24). Every other control moves the picture by at least 0.59.

## Not done

- **Never loaded into Resolume**, on either platform.
- **Not released yet:** no website entry, user guide, presets or browser demo. The Windows build compiles in CI and nowhere else.
- **Not modelled:**
  - thin-film iridescence
  - polarisation
  - RCWA efficiency
  - aluminium's reflectance curve
  - orders past 2
  - a room (environment light)
  - emboss or raised foil from the clip's own luminance
  - anti-aliased pattern edges
  - glare and bloom
  - dot-matrix holograms with per-dot artwork
- **Open questions for Allan:**
  - Should the finishes become presets? They would need names without trademarks; the README's table is a start.
  - Should a "room" term (a dim, large second lamp) be added, so mirror foils read as metal away from the highlight?
  - Should Pattern Scale default bigger, for 720p and below?
