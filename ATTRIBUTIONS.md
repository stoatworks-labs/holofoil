# Attributions

Holofoil is built on other people's work. This file lists what that work is, who did it, and what it is doing here.

It is written by hand for now, because the project is unpublished. When it is published, the master lists in `stoatworks-backend` should take it over through `scripts/sync-attributions.py`, as the rest of the fleet's do.

## Code we derived from other people's work

### The effect's shape, the GL state guard, the pass buffer, the log and the tooling — Stoatworks slitscan

<https://github.com/stoatworks-labs/slitscan>  
Licence: MIT  
Copyright: Stoatworks Labs

Adapted from slitscan:

- the plugin class's shape and its table of controls
- `source/GLState.h`
- `source/PassBuffer.{h,cpp}` (FFGLFBO with the SDK's colour-texture leak fixed)
- `source/Diag.{h,cpp}`
- the host clock's unit vote and `SetBeatInfo`
- the About block's hand-written FFGL half (`StoatworksAboutLinks.h`, `StoatworksAboutParams.h`)
- the harness's rig, PNG writer, cue sheets, `--pipe` and `--film`, `--negative` with its Perturb, `--state`, `--names`, `--cues` and `--offline`
- `tools/verify.sh`, `tools/mutate.sh`, `tools/glslc.sh` and `tools/sweep.py`

All of it is adapted, and it reached slitscan from the rest of the fleet.

### The card's frame — Stoatworks lenticular

<https://github.com/stoatworks-labs/lenticular>  
Licence: MIT  
Copyright: Stoatworks Labs

This is a convention, not code: the card stays put while the eye and the lamp move round it, and distances are measured in picture widths. `source/Card.cpp` is written fresh to that convention.

## Published methods and numbers

These are used as published equations and constants. No code from them is used.

- **The CIE 1931 colour matching functions:** C. Wyman, P.-P. Sloan and P. Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions", *JCGT* 2(2), 2013, <https://jcgt.org/published/0002/02/01/>. Holofoil uses the multi-lobe fit's coefficients from Listing 1, both in the shader and, independently, in the harness.
- **sRGB:** IEC 61966-2-1:1999 with Amendment 1 (2003). Holofoil uses the XYZ→linear sRGB matrix and the transfer function.
- **The grating equation in direction cosines:** J. E. Harvey and C. L. Vernold, "Description of diffraction grating behavior in direction cosine space", *Applied Optics* 37(34), 1998; and J. E. Harvey and R. N. Pfisterer, *Optical Engineering* 58(8), 087105, 2019.
- **Thin-grating efficiency** (Raman–Nath, *J_m*²): the MIT OpenCourseWare notes for MAS.450 *Holographic Imaging*, appendix on grating efficiency.
- **Gratings set per region by maps:** A. Toisoul, D. S. Dhillon and A. Ghosh, "Acquiring Spatially Varying Appearance of Printed Holographic Surfaces", *ACM TOG* 37(6), 2018. This is the idea that a pattern sets the grating's direction and pitch region by region.
- **Diffraction as a BRDF:** J. Stam, "Diffraction Shaders", SIGGRAPH 1999.
- **The hash:** M. Jarzynski and M. Olano, "Hash Functions for GPU Rendering", *JCGT* 9(3), 2020. Holofoil uses its PCG hash, after M. E. O'Neill's PCG.
- **Luminance:** ITU-R BT.709's weights, applied to linear light.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

It is vendored as a git submodule at `external/ffgl`, pinned at b1afaf9. It is the plugin ABI itself.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

It is needed on Windows only, through vcpkg. The Windows build has not yet been made.

### zlib

<https://zlib.net>  
Licence: zlib  
Copyright: Jean-loup Gailly and Mark Adler

Only the harness (`hftest`) uses it, to write PNGs. It is linked from the system and not shipped in the plugin.

## Looked at, nothing taken

- **simeydotme/pokemon-cards-css** (GPL-3.0). It does screen-space CSS blending, with no wavelengths and no gratings. No code, asset, number or name came from it.
- **GPU Gems, chapter 8** (all rights reserved), **the OpenGL Shading Language §14.2** (licence not stated), and **Alan Zucconi's spectral fits** (credit required). Holofoil reads colour from the CIE fit above instead.
