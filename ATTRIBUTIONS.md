# Attributions

Holofoil is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the GL state guard, the pass buffer, the log and the tooling — Stoatworks slitscan

<https://github.com/stoatworks-labs/slitscan>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape and its table of controls, source/GLState.h (radar's, which is vectrix's, from resolume-scopes), source/PassBuffer.{h,cpp} (the fleet's, without a depth buffer) and source/Diag.{h,cpp}; the host clock's unit vote and SetBeatInfo; the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues and --offline; and tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py (schlieren's and fiche's before it). All adapted.

### The card's frame — Stoatworks lenticular

<https://github.com/stoatworks-labs/lenticular>  
Licence: MIT  
Copyright: Stoatworks Labs

A convention rather than code: the card stays put while the eye and the lamp move round it, and distances are measured in picture widths. source/Card.cpp is written fresh to it.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Simple Analytic Approximations to the CIE XYZ Color Matching Functions — C. Wyman, P.-P. Sloan and P. Shirley, JCGT 2(2), 2013

<https://jcgt.org/published/0002/02/01/>

The multi-lobe fit's coefficients (Listing 1), in the shader's colour table and, written out again independently, in the harness's per-wavelength reference. hftest --grating checks the plugin's light against that sum.

### Diffraction grating behaviour in direction cosine space — J. E. Harvey and C. L. Vernold, Applied Optics 37(34), 1998; J. E. Harvey and R. N. Pfisterer, Optical Engineering 58(8), 2019

The grating equation as (v + l) projected on the surface = mλ/d along the grating's direction, which is how sheenAt() in source/Shaders.cpp finds each pixel's wavelength, order by order.

### Thin-grating efficiency (Raman–Nath) — MIT OpenCourseWare, MAS.450 Holographic Imaging

How much of each wavelength a groove of a given depth sends into each order, J_m² of the phase swing. The colour table carries it, and hftest --groove checks the mirror's share against J₀².

### Acquiring Spatially Varying Appearance of Printed Holographic Surfaces — A. Toisoul, D. S. Dhillon and A. Ghosh, ACM TOG 37(6), 2018

The idea that a holographic print is a map of small gratings, each with its own direction and pitch. The patterns (Crosshatch, Cosmos, Cracked Ice, Sparkle) set the grating cell by cell this way.

### Diffraction Shaders — J. Stam, SIGGRAPH 1999

Diffraction as a reflectance a renderer can evaluate per pixel, and the lamp's size as what turns a grating's single wavelength into a band.

### Hash Functions for GPU Rendering — M. Jarzynski and M. Olano, JCGT 9(3), 2020

<https://jcgt.org/published/0009/03/02/>

The PCG hash (after Melissa E. O'Neill's PCG) that gives each cell its direction, pitch and lean, so the same cells come out on every GPU.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Holographic trading-card finishes

The holo window, the reverse holo, full-card foil, cold foil, etched and textured finishes, and the cosmos, cracked-ice, crosshatch and glitter patterns printed on trading cards. Built from how a foil is made (an embossed aluminium grating under the ink) and how it catches a lamp; no card art, scan, brand or name is used.

## Standards and published specifications

What the implementation is measured against.

- **CIE 1931 standard colorimetric observer** — the colour matching functions every wavelength of the foil's light goes through on its way to XYZ, and from there to sRGB.
- **IEC 61966-2-1 (sRGB)** — the XYZ to linear sRGB matrix and the transfer function in source/Shaders.cpp: the clip is linearised so the foil's light adds to it as light does, then encoded again.
- **ITU-R BT.709 luminance** — the weights Bright Ink and Dark Ink key on, applied to linear light. hftest --luma checks it.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
