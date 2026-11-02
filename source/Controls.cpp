#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace holofoil
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
/// -most .. +most, linearly, the middle at 0.
double symmetric( float v, double most )
{
	return ( 2.0 * clamp01( v ) - 1.0 ) * most;
}
float inverseSymmetric( double value, double most )
{
	return static_cast< float >( std::clamp( 0.5 + 0.5 * value / most, 0.0, 1.0 ) );
}
float inverseLinear( double value, double most )
{
	return static_cast< float >( std::clamp( value / most, 0.0, 1.0 ) );
}
/// 1/distance = (1 - v)^2 / closest.
double inverseDistance( float v, double closest )
{
	const double r = 1.0 - clamp01( v );
	return r * r / closest;
}
float inverseOfInverseDistance( double distance, double closest )
{
	if( !( distance > 0.0 ) || !std::isfinite( distance ) )
		return 1.0f;
	return static_cast< float >( std::clamp( 1.0 - std::sqrt( std::clamp( closest / distance, 0.0, 1.0 ) ), 0.0, 1.0 ) );
}

constexpr double kTiltMost       = 40.0 * kDegree;
constexpr double kDepthMost      = 40.0 * kDegree;
constexpr double kRateLow        = 0.02, kRateHigh = 4.0;
constexpr double kViewerClosest  = 0.6;
constexpr double kLampClosest    = 0.4;
constexpr double kLampAngleMost  = 80.0 * kDegree;
constexpr double kSizeLow        = 0.2 * kDegree, kSizeHigh = 20.0 * kDegree;
constexpr double kLengthLow      = 1.0, kLengthHigh = 20.0;
constexpr double kBrightnessMost = 4.0;
constexpr double kPitchLow       = 500.0, kPitchHigh = 3000.0;
constexpr double kGrooveMost     = 400.0;
constexpr double kScaleLow       = 0.25, kScaleHigh = 4.0;
constexpr double kFacetMost      = 20.0 * kDegree;
constexpr double kRoughMost      = 10.0 * kDegree;
constexpr double kCornerMost     = 0.25;
constexpr double kFeatherMost    = 0.1;
constexpr double kInkBandMost    = 0.5;

const char* const kAreaNames[]    = { "Art Window", "Around Art", "Whole Card", "Bright Ink", "Dark Ink", "Clip Alpha" };
const char* const kPatternNames[] = { "Rainbow", "Crosshatch", "Cosmos", "Cracked Ice", "Sparkle" };
const char* const kMotionNames[]  = { "Hold", "Tip", "Swing", "Circle", "Lamp Orbit" };
const char* const kSyncNames[]    = { "Free", "Beat", "Bar" };

ControlInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, nullptr, 0 };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count };
}
ControlInfo of( unsigned int id, const char* name, const char* group, Kind kind, float value )
{
	return { id, name, group, kind, value, nullptr, 0 };
}

//---------------------------------------------------------------------------
// The defaults: a holo, a plain rainbow grating in the art window, on a card
// tipped gently toward and away at arm's length under a strip light above
// the viewer's head. Every number is in AGENTS.md with why.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_AREA, "Foil Area", "Finish", 0.0f, kAreaNames, static_cast< int >( Area::Count ) ),
	option( PT_PATTERN, "Pattern", "Finish", 0.0f, kPatternNames, static_cast< int >( Pattern::Count ) ),

	standard( PT_TILT_X, "Tilt X", "Card", 0.5f ),
	standard( PT_TILT_Y, "Tilt Y", "Card", 0.5f ),
	option( PT_MOTION, "Motion", "Card", 1.0f, kMotionNames, static_cast< int >( Motion::Count ) ),
	standard( PT_MOTION_DEPTH, "Motion Depth", "Card", ParamFromMotionDepth( 12.0 * kDegree ) ),
	standard( PT_MOTION_RATE, "Motion Rate", "Card", ParamFromMotionRate( 0.2 ) ),
	option( PT_MOTION_SYNC, "Motion Sync", "Card", 0.0f, kSyncNames, static_cast< int >( MotionSync::Count ) ),
	standard( PT_VIEWER, "Viewer Distance", "Card", ParamFromViewer( 2.5 ) ),

	standard( PT_LAMP_X, "Lamp X", "Lamp", 0.5f ),
	standard( PT_LAMP_Y, "Lamp Y", "Lamp", ParamFromLampAngle( 35.0 * kDegree ) ),
	standard( PT_LAMP_DISTANCE, "Lamp Distance", "Lamp", ParamFromLampDistance( 3.0 ) ),
	standard( PT_LAMP_SIZE, "Lamp Size", "Lamp", ParamFromLampSize( 2.0 * kDegree ) ),
	standard( PT_LAMP_LENGTH, "Lamp Length", "Lamp", ParamFromLampLength( 8.0 ) ),
	standard( PT_LAMP_BRIGHTNESS, "Lamp Brightness", "Lamp", ParamFromLampBrightness( 1.5 ) ),

	standard( PT_PITCH, "Pitch", "Foil", ParamFromPitch( 900.0 ) ),
	standard( PT_ANGLE, "Grating Angle", "Foil", ParamFromGratingAngle( 90.0 * kDegree ) ),
	standard( PT_GROOVE, "Groove Depth", "Foil", ParamFromGroove( 130.0 ) ),
	standard( PT_SCALE, "Pattern Scale", "Foil", ParamFromPatternScale( 1.0 ) ),
	standard( PT_FACET, "Facet Tilt", "Foil", ParamFromFacetTilt( 6.0 * kDegree ) ),
	standard( PT_ROUGHNESS, "Roughness", "Foil", ParamFromRoughness( 0.5 * kDegree ) ),

	standard( PT_INK, "Ink", "Print", 0.6f ),
	standard( PT_BASE, "Base", "Print", 0.7f ),

	of( PT_WINDOW_X, "Window X", "Area", Kind::XPos, 0.5f ),
	of( PT_WINDOW_Y, "Window Y", "Area", Kind::YPos, 0.64f ),
	standard( PT_WINDOW_WIDTH, "Window Width", "Area", 0.84f ),
	standard( PT_WINDOW_HEIGHT, "Window Height", "Area", 0.42f ),
	standard( PT_CORNER, "Corner", "Area", 0.04f ),
	standard( PT_SOFTNESS, "Softness", "Area", 0.0f ),
	standard( PT_THRESHOLD, "Threshold", "Area", 0.5f ),

	standard( PT_MIX, "Mix", "Output", 1.0f ),
};

static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in kTable" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::clamp( index, 0, count - 1 );
}

double TiltFromParam( float value )
{
	return symmetric( value, kTiltMost );
}
float ParamFromTilt( double radians )
{
	return inverseSymmetric( radians, kTiltMost );
}

double MotionDepthFromParam( float value )
{
	return clamp01( value ) * kDepthMost;
}
float ParamFromMotionDepth( double radians )
{
	return inverseLinear( radians, kDepthMost );
}

double MotionRateFromParam( float value )
{
	return geometric( value, kRateLow, kRateHigh );
}
float ParamFromMotionRate( double cycles )
{
	return inverseGeometric( cycles, kRateLow, kRateHigh );
}

double InverseViewerFromParam( float value )
{
	return inverseDistance( value, kViewerClosest );
}
float ParamFromViewer( double widths )
{
	return inverseOfInverseDistance( widths, kViewerClosest );
}
double InverseLampFromParam( float value )
{
	return inverseDistance( value, kLampClosest );
}
float ParamFromLampDistance( double widths )
{
	return inverseOfInverseDistance( widths, kLampClosest );
}

double LampAngleFromParam( float value )
{
	return symmetric( value, kLampAngleMost );
}
float ParamFromLampAngle( double radians )
{
	return inverseSymmetric( radians, kLampAngleMost );
}

double LampSizeFromParam( float value )
{
	return geometric( value, kSizeLow, kSizeHigh );
}
float ParamFromLampSize( double radians )
{
	return inverseGeometric( radians, kSizeLow, kSizeHigh );
}

double LampLengthFromParam( float value )
{
	return geometric( value, kLengthLow, kLengthHigh );
}
float ParamFromLampLength( double ratio )
{
	return inverseGeometric( ratio, kLengthLow, kLengthHigh );
}

double LampBrightnessFromParam( float value )
{
	return clamp01( value ) * kBrightnessMost;
}
float ParamFromLampBrightness( double gain )
{
	return inverseLinear( gain, kBrightnessMost );
}

double PitchFromParam( float value )
{
	return geometric( value, kPitchLow, kPitchHigh );
}
float ParamFromPitch( double nanometres )
{
	return inverseGeometric( nanometres, kPitchLow, kPitchHigh );
}

double GratingAngleFromParam( float value )
{
	return clamp01( value ) * kPi;
}
float ParamFromGratingAngle( double radians )
{
	return inverseLinear( radians, kPi );
}

double GrooveFromParam( float value )
{
	return clamp01( value ) * kGrooveMost;
}
float ParamFromGroove( double nanometres )
{
	return inverseLinear( nanometres, kGrooveMost );
}

double PatternScaleFromParam( float value )
{
	return geometric( value, kScaleLow, kScaleHigh );
}
float ParamFromPatternScale( double factor )
{
	return inverseGeometric( factor, kScaleLow, kScaleHigh );
}

double FacetTiltFromParam( float value )
{
	return clamp01( value ) * kFacetMost;
}
float ParamFromFacetTilt( double radians )
{
	return inverseLinear( radians, kFacetMost );
}

double RoughnessFromParam( float value )
{
	return clamp01( value ) * kRoughMost;
}
float ParamFromRoughness( double radians )
{
	return inverseLinear( radians, kRoughMost );
}

double CornerFromParam( float value )
{
	return clamp01( value ) * kCornerMost;
}

double WindowSoftnessFromParam( float value )
{
	return clamp01( value ) * kFeatherMost;
}
double InkSoftnessFromParam( float value )
{
	return clamp01( value ) * kInkBandMost;
}

} // namespace holofoil
