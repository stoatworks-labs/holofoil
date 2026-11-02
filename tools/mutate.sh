#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders, or built its references from the plugin's own functions, would
# pass every mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# 320x180, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught). Most of them are in the GLSL, because that is
# where the optics are: the harness computes the light per wavelength in
# double, with no table, no spectral lines and no along/across, so a mutant in
# any of those has nothing on the harness's side to hide behind.
#
#     tools/mutate.sh            every mutant
#     tools/mutate.sh TEXT       only the mutants whose description contains TEXT
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|float t = ( x - mu ) * ( x < mu ? below : above );|float t = ( x - mu ) * ( x > mu ? below : above );|--grating|GLSL: each matching-function lobe's two widths swapped side for side, < -> >"
	"source/Shaders.cpp|0.821 * lobe( lambda, 568.8, 0.0213, 0.0247 )|0.821 * lobe( lambda, 558.8, 0.0213, 0.0247 )|--grating|GLSL: y-bar's main lobe ten nanometres bluer, 6 -> 5"
	"source/Shaders.cpp|float centre = kLambdaScale * w / ( 1.0 - abs( w ) );|float centre = kLambdaScale * w / ( 1.0 + abs( w ) );|--grating|GLSL: the table's columns at the wrong wavelengths, - -> +"
	"source/Shaders.cpp|float norm = step / ( kRootTwoPi * sigma );|float norm = step * ( kRootTwoPi * sigma );|--grating|GLSL: a spectral line not of unit area, / -> *"
	"source/Shaders.cpp|float width  = pitch * spread / float( order );|float width  = pitch * spread * float( order );|--grating|GLSL: the second order's line twice as wide, not half, / -> *"
	"source/Shaders.cpp|float k = clamp( log( sigma / kSigmaLow ) / log( kSigmaRatio ) * float( kTableRows - 1 )|float k = clamp( log( sigma / kSigmaLow ) / log( kSigmaRatio ) * float( kTableRows - 2 )|--grating|GLSL: the line's width read a row off, 1 -> 2"
	"source/Shaders.cpp|float h    = 0.5 * x;|float h    = 0.6 * x;|--groove|GLSL: the Bessel series' argument a fifth long, 5 -> 6"
	"source/Shaders.cpp|float j = besselJ( m, kTwoPi * GrooveDepth / lambda );|float j = besselJ( m, kTwoPi * GrooveDepth * lambda );|--groove|GLSL: the phase swing growing with wavelength, / -> *"
	"source/Shaders.cpp|vec3 v  = normalize( Eye.xyz - Eye.w * at );|vec3 v  = normalize( Eye.xyz + Eye.w * at );|--mirror|GLSL: the way to a near eye from the wrong side, - -> +"
	"source/Shaders.cpp|vec2 s = h.xy - h.z * sf.slope;|vec2 s = h.xy + h.z * sf.slope;|--patterns|GLSL: a facet leaning the other way, - -> +"
	"source/Shaders.cpp|vec3 cov    = LampSpread + vec3( rough * rough, 0.0, rough * rough );|vec3 cov    = LampSpread + vec3( rough * rough, 0.0, rough + rough );|--lamp|GLSL: roughness spreading the lamp unevenly, * -> +"
	"source/Shaders.cpp|LampSpread.y * LampSpread.y ) / det );|LampSpread.y * LampSpread.y ) * det );|--lamp|GLSL: the lamp's area in the wrong place, every pixel scaled by its determinant, / -> *"
	"source/Shaders.cpp|float reach  = cov.x * across.x * across.x + 2.0 * cov.y|float reach  = cov.x * across.x * across.x + 3.0 * cov.y|--grating|GLSL: the lamp's reach across the grating with its cross term off, 2 -> 3"
	"source/Shaders.cpp|ivec2( 0, 2 * kTableRows )|ivec2( 0, 1 * kTableRows )|--groove|GLSL: the mirror's colour read from the second order's first row, 2 -> 1"
	"source/Shaders.cpp|return clamp( 0.5 - sd / Ramp, 0.0, 1.0 );|return clamp( 0.6 - sd / Ramp, 0.0, 1.0 );|--window|GLSL: the window a tenth of a pixel too big, 5 -> 6"
	"source/Shaders.cpp|vec2 q   = abs( px - Window.xy ) - Window.zw + vec2( r );|vec2 q   = abs( px - Window.xy ) - Window.zw - vec2( r );|--window|GLSL: the corners rounded outward, + -> -"
	"source/Shaders.cpp|return clamp( 0.5 + 0.5 * ( level - Threshold ) / Band, 0.0, 1.0 );|return clamp( 0.6 + 0.5 * ( level - Threshold ) / Band, 0.0, 1.0 );|--luma|GLSL: the ink's ramp off its threshold, 5 -> 6"
	"source/Shaders.cpp|vec3 foil  = light * mix( 1.0, Base, Ink ) + sheen|vec3 foil  = light * mix( 1.0, Base, Ink ) - sheen|--ink|GLSL: the foil's light taken from the print, + -> -"
	"source/Shaders.cpp|1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055|1.055 * pow( l, vec3( 1.0 / 2.5 ) ) - 0.055|--ink|GLSL: sRGB encoded at 1/2.5, 4 -> 5"
	"source/Shaders.cpp|float pitch = exp2( ( unitOf( cellHash( c, 4u ) ) - 0.5 ) * 0.643856 );|float pitch = exp2( ( unitOf( cellHash( c, 4u ) ) - 0.5 ) * 0.743856 );|--patterns|GLSL: a cell's pitch spread wider than 0.8 to 1.25, 6 -> 7"
	"source/Shaders.cpp|bool onA = abs( a - floor( a + 0.5 ) ) < 0.25;|bool onA = abs( a - floor( a + 0.5 ) ) < 0.35;|--patterns|GLSL: the crosshatch's first lines wider than half their period, 2 -> 3"
	"source/Card.cpp|const double z1 = -y * sy + z * cy;|const double z1 = +y * sy + z * cy;|--mirror|C++: Tilt Y rotating the wrong way into the card's frame, - -> +"
	"source/Card.cpp|double x = std::cos( lampY ) * std::sin( lampX );|double x = std::cos( lampX ) * std::sin( lampX );|--mirror|C++: the lamp's across from Lamp X twice, Y -> X"
	"source/Card.cpp|s.xy = along * along * ax * ay + across * across * bx * by;|s.xy = along * along * ax * ay - across * across * bx * by;|--grating|C++: the strip lamp's cross term the wrong sign, + -> -"
	"source/Card.cpp|case Motion::Tip: pose.tiltY += depth * std::sin( a ); break;|case Motion::Tip: pose.tiltY -= depth * std::sin( a ); break;|--motion|C++: Tip starting away, not toward, + -> -"
	"source/Holofoil.cpp|const double hatchA = angle + 45.0 * kDegree;|const double hatchA = angle + 46.0 * kDegree;|--patterns|C++: the crosshatch's first family a degree off, 5 -> 6"
	"source/Holofoil.cpp|const double ramp   = 1.0 + WindowSoftnessFromParam|const double ramp   = 2.0 + WindowSoftnessFromParam|--window|C++: the window's edge two pixels wide, 1 -> 2"
	"source/Controls.cpp|return low * std::pow( high / low, clamp01( v ) );|return low * std::pow( high * low, clamp01( v ) );|--controls|C++: every geometric slider off its top, / -> *"
	"source/Controls.cpp|constexpr double kViewerClosest  = 0.6;|constexpr double kViewerClosest  = 0.5;|--controls|C++: the closest viewer half a width, 6 -> 5"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

only="${1:-}"
caught=0
ran=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	[[ -z "$only" || "$what" == *"$only"* ]] || continue
	ran=$(( ran + 1 ))
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target hftest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	limit 300 "$tree/build/hftest" "$check" --size 320x180 >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "$ran" "$caught"
[[ "$ran" -gt 0 && "$caught" -eq "$ran" ]]
