#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Option parameters hold the element index.

    Units: angles in degrees at the host and radians inside; lengths on the
    card in picture WIDTHS (lenticular's convention), the grating's pitch and
    groove depth in nanometres. Tilt, Lamp X and Lamp Y are linear in the
    slider and the two distances are linear in 1/distance's square root, so
    every one of them is smooth to automate and Viewer Distance and Lamp
    Distance reach infinity at the top of the slider without a jump.
*/
namespace holofoil
{
/// Every control, in the order Resolume shows them. The About block is LAST,
/// so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Finish: where the foil is, and what is embossed in it ---------------
	PT_AREA = 0,
	PT_PATTERN,

	// -- Card: how it is held -------------------------------------------------
	PT_TILT_X,
	PT_TILT_Y,
	PT_MOTION,
	PT_MOTION_DEPTH,
	PT_MOTION_RATE,
	PT_MOTION_SYNC,
	PT_VIEWER,

	// -- Lamp -----------------------------------------------------------------
	PT_LAMP_X,
	PT_LAMP_Y,
	PT_LAMP_DISTANCE,
	PT_LAMP_SIZE,
	PT_LAMP_LENGTH,
	PT_LAMP_BRIGHTNESS,

	// -- Foil: the grating ----------------------------------------------------
	PT_PITCH,
	PT_ANGLE,
	PT_GROOVE,
	PT_SCALE,
	PT_FACET,
	PT_ROUGHNESS,

	// -- Print: the ink over the foil -----------------------------------------
	PT_INK,
	PT_BASE,

	// -- Area: the art window and the ink thresholds --------------------------
	PT_WINDOW_X,
	PT_WINDOW_Y,
	PT_WINDOW_WIDTH,
	PT_WINDOW_HEIGHT,
	PT_CORNER,
	PT_SOFTNESS,
	PT_THRESHOLD,

	// -- Output -----------------------------------------------------------------
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Option,  ///< an element index
	XPos,    ///< 0..1 across the frame
	YPos     ///< 0..1 up the frame
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name;  ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, or the index
	const char* const* options;
	int optionCount;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options. The shaders switch on these indices, so their order is fixed.
//---------------------------------------------------------------------------

/// Where the foil is. The first three are the card finishes: a holo has foil
/// in the art window, a reverse holo everywhere but, a full foil everywhere.
enum class Area
{
	ArtWindow = 0,///< holo
	AroundArt,    ///< reverse holo: exactly the window's complement
	WholeCard,    ///< full foil
	BrightInk,    ///< where the clip is lighter than Threshold
	DarkInk,      ///< where it is darker: exactly Bright Ink's complement
	ClipAlpha,    ///< the clip's own alpha
	Count
};

/// What is embossed in the foil: the grating's direction (and the facet's
/// tilt) region by region.
enum class Pattern
{
	Rainbow = 0,///< one grating across the card
	Crosshatch, ///< two families of diagonal lines, each a grating across its own lines
	Cosmos,     ///< discs of every size, each its own grating, on a plain one
	CrackedIce, ///< shards, each its own grating and slightly tilted
	Sparkle,    ///< tiny tilted facets, each a grating: glitter
	Count
};

/// How the card moves on its own.
enum class Motion
{
	Hold = 0, ///< as set
	Tip,      ///< toward and away, about Tilt Y: what sweeps a grating's colour under a lamp overhead
	Swing,    ///< side to side, about Tilt X
	Circle,   ///< the tilt goes round a circle about (Tilt X, Tilt Y)
	LampOrbit,///< the card holds still and the lamp goes round its place
	Count
};

/// What Motion Rate is counted in.
enum class MotionSync
{
	Free = 0,///< cycles a second
	Beat,    ///< cycles a beat of Resolume's
	Bar,     ///< cycles a bar
	Count
};

/// An option's index from the value the host stored (it reads back 0..1
/// whatever the count -- the fleet's trap -- so the table's own count is
/// what bounds it).
int OptionIndex( float value, int count );

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

constexpr double kPi     = 3.14159265358979323846;
constexpr double kDegree = kPi / 180.0;

/// Tilt X and Tilt Y: -40 to +40 degrees, linearly; 0.5 is square on.
double TiltFromParam( float value );
float ParamFromTilt( double radians );

/// Motion Depth: 0 to 40 degrees, linearly.
double MotionDepthFromParam( float value );
float ParamFromMotionDepth( double radians );

/// Motion Rate: 0.02 to 4 cycles a second (or a beat, or a bar), geometrically.
double MotionRateFromParam( float value );
float ParamFromMotionRate( double cycles );

/// Viewer Distance and Lamp Distance, in picture widths from the card's
/// centre: 1/distance = (1 - v)^2 / closest, so the top of the slider is
/// infinity (0 returned) and the slider is smooth all the way there.
double InverseViewerFromParam( float value );
float ParamFromViewer( double widths );
double InverseLampFromParam( float value );
float ParamFromLampDistance( double widths );

/// Lamp X (across) and Lamp Y (up): where the lamp is seen from the card's
/// centre, -80 to +80 degrees, linearly.
double LampAngleFromParam( float value );
float ParamFromLampAngle( double radians );

/// Lamp Size: the lamp's angular radius (one standard deviation of its
/// brightness), 0.2 to 20 degrees, geometrically.
double LampSizeFromParam( float value );
float ParamFromLampSize( double radians );

/// Lamp Length: a strip light, its length over its width, 1 (round) to 20,
/// geometrically. It runs across the room, level, so a grating whose lines
/// run across too catches it all along them. The long way is capped at 60
/// degrees either side.
double LampLengthFromParam( float value );
float ParamFromLampLength( double ratio );

/// Lamp Brightness: 0 to 4, linearly. 1 is the paper's white.
double LampBrightnessFromParam( float value );
float ParamFromLampBrightness( double gain );

/// Pitch: the grating's period, 500 to 3000 nm, geometrically.
double PitchFromParam( float value );
float ParamFromPitch( double nanometres );

/// Grating Angle: the grating's direction (across its lines), 0 to 180
/// degrees, linearly.
double GratingAngleFromParam( float value );
float ParamFromGratingAngle( double radians );

/// Groove Depth: the relief's peak-to-peak depth, 0 to 400 nm, linearly.
double GrooveFromParam( float value );
float ParamFromGroove( double nanometres );

/// Pattern Scale: the pattern's cells, 0.25 to 4 times their natural size,
/// geometrically.
double PatternScaleFromParam( float value );
float ParamFromPatternScale( double factor );

/// Facet Tilt: the most a cell's surface leans, 0 to 20 degrees, linearly.
double FacetTiltFromParam( float value );
float ParamFromFacetTilt( double radians );

/// Roughness: the spread of the surface's normal below a pixel, 0 to 10
/// degrees, linearly.
double RoughnessFromParam( float value );
float ParamFromRoughness( double radians );

/// Corner: the art window's corner radius, 0 to a quarter of the picture's
/// height.
double CornerFromParam( float value );

/// Softness: the foil area's edge. For the window, an extra ramp up to a
/// tenth of the picture's height; for the ink areas, the half-width of the
/// luminance ramp about Threshold, up to 0.5.
double WindowSoftnessFromParam( float value );
double InkSoftnessFromParam( float value );

} // namespace holofoil
