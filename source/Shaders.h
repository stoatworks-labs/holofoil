#pragma once

#include <string>

/**
	The passes.

	1. **table** -- only when Groove Depth changes: the colour every spectral
	   line the foil can send to the eye makes, into a 2048 x 129 RGBA32F
	   buffer. Rows 0..63 are the first order, 64..127 the second, each row a
	   line width; columns are where the line is centred. Row 128 is the
	   mirror's own colour (the zeroth order). Every texel integrates the
	   spectrum through the colour matching functions and the groove's
	   efficiency for that order, 256 taps.
	2. **foil** -- to the host: for each pixel, the card's grating (the
	   pattern's direction, pitch and tilt there), the eye and the lamp seen
	   from that point, the wavelength each order sends to the eye by the
	   grating equation, the lamp's spread round it, and the colour of that
	   line read from the table. Then the ink over it and the foil's area.

	The fragment shaders are assembled from pieces at InitGL by `Assemble()`,
	the one place the order is written. tools/glslc.sh's ASSEMBLED table
	mirrors it, so the text it compiles is the text the plugin runs.

	No sin, cos, tan or atan built-in anywhere (GLSL promises nothing about
	their accuracy). The grating's direction and the crosshatch's come from
	the CPU; a cell's random direction is `cosSinTurns`, a Taylor polynomial on
	an eighth of a turn, good to 2e-9.
*/
namespace holofoil::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kTableFragment;
extern const char* const kFoilFragment;

enum class Pass
{
	Table,
	Foil
};
std::string Assemble( Pass pass );

//---------------------------------------------------------------------------
// The colour table. kCommon repeats every one of these.
//---------------------------------------------------------------------------
/// Columns: a line centred at c (nm, any sign) is at u = 0.5 + 0.5 c / ( |c| + kLambdaScale ).
constexpr int kTableColumns   = 2048;
constexpr double kLambdaScale = 600.0;
/// Rows per order: line widths (one standard deviation, nm) from kSigmaLow
/// to kSigmaLow x kSigmaRatio, geometrically.
constexpr int kTableRows      = 64;
constexpr double kSigmaLow    = 0.5;
constexpr double kSigmaRatio  = 4000.0;
/// Two orders and the mirror's row.
constexpr int kTableHeight    = 2 * kTableRows + 1;

/// The pattern's cell, in picture widths at Pattern Scale 1.
constexpr double kHatchSpacing = 0.012;
constexpr double kCosmosCell   = 0.06;
constexpr double kIceCell      = 0.07;
constexpr double kSparkleCell  = 0.004;

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model.
enum Hook : int
{
	kHookMinusLamp      = 1 << 0, ///< the lamp's in-plane direction subtracted: a grating lit from behind
	kHookNoDirection    = 1 << 1, ///< the grating's direction ignored: lambda = d |s|
	kHookLampAtInfinity = 1 << 2, ///< the lamp's distance ignored
	kHookNoEfficiency   = 1 << 3, ///< the mirror keeps all its light whatever the groove
	kHookSquareCorners  = 1 << 4, ///< the art window's corners square
	kHookRoundTrip      = 1 << 5, ///< no exact passthrough: every pixel through linear light and back
	kHookOneAngle       = 1 << 6, ///< the optics use Grating Angle everywhere, whatever the pattern says
	kHookCodeLuma       = 1 << 7, ///< the ink's luminance from code values, not light
	kHookNoRoughness    = 1 << 8, ///< Roughness ignored
	kHookHardReverse    = 1 << 9, ///< Around Art from an unsmoothed window: not the complement at the edge
	kHookShowGrating    = 1 << 10,///< the picture is (g.x, g.y, slope.x, slope.y): the pattern read out
	kHookShowWeights    = 1 << 11,///< the picture is (w1, w2, pitch factor, 0)
	kHookPlainAdd       = 1 << 12 ///< the foil's light added over the print, the ink filtering nothing
};

} // namespace holofoil::shaders
