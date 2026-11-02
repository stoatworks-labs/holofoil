#pragma once

#include "Controls.h"

/**
    The card, the eye and the lamp, as numbers. No GL.

    ---------------------------------------------------------------- the frame

    Everything the shader does is in the CARD's frame: x across the picture,
    y up it, z out of it toward whoever is looking, lengths in picture widths
    from the picture's centre. The picture is never moved. Tilting the card is
    the same thing as moving the eye and the lamp round a card that stays put,
    so the shader draws the card square on (rectified) and only the optics
    know it is tilted -- lenticular's convention, and the reason this plugin
    and that one compose.

    The room has the eye on its z axis at distance D_v, looking at the card's
    centre, and the lamp at distance D_l in the direction (Lamp X, Lamp Y):
    azimuth across and elevation up, (cos ly sin lx, sin ly, cos ly cos lx).
    The card is turned by Tilt X about its own vertical axis and then by Tilt Y
    about its own horizontal one, so that in the card's frame the eye is in
    the direction

        e = ( cos Ty sin Tx, sin Ty, cos Ty cos Tx )

    -- at azimuth Tx and elevation Ty from the card's normal. A room vector r
    is r_card = Ry( Tx ) Rx( -Ty ) r. A distance of infinity is a direction,
    w = 0; anything nearer is a point, w = 1.

    ---------------------------------------------------------------- the motion

    Motion's phase is a cycle count kept in double and reduced to [0, 1), and
    it is advanced by Rate x the frame's time, never computed as Rate x the
    clock: turning Motion Rate mid-show changes how fast the card rocks, not
    where it is. Resolume's clock has been seen 499 million ms in, where a
    float cannot tell one frame from the next.
*/
namespace holofoil
{
/// A point (w = 1) or a direction (w = 0) in the card's frame, picture widths.
struct Place
{
	double x = 0.0, y = 0.0, z = 1.0, w = 0.0;
};

/// The angles that place the card and the lamp, after Motion.
struct Pose
{
	double tiltX = 0.0, tiltY = 0.0;///< radians
	double lampX = 0.0, lampY = 0.0;///< radians
};

/// Where the eye is, in the card's frame. `inverseDistance` 0 is infinity.
Place EyeInCard( double tiltX, double tiltY, double inverseDistance );

/// Where the lamp is, in the card's frame.
Place LampInCard( double tiltX, double tiltY, double lampX, double lampY, double inverseDistance );

/// The lamp's brightness as a Gaussian of direction: its covariance in the
/// card's direction cosines (x, y), from its angular radius `size` and its
/// length over width `length` (a strip, running level across the room).
/// Seen from the card's centre: a near lamp's size is not recomputed for
/// every point of the card.
struct Spread
{
	double xx = 0.0, xy = 0.0, yy = 0.0;
};
Spread LampSpread( double tiltX, double tiltY, double lampX, double lampY, double size, double length );

/// How far along its length a strip is allowed to reach, either side: 60
/// degrees, as a direction cosine. Past that a Gaussian is no model of it.
constexpr double kLongestStrip = 0.866;

/// Motion applied to the hand-set angles.
Pose PoseOf( Motion motion, double depth, double phase, const Pose& set );

/// Motion's phase: cycles, advanced in double and reduced to [0, 1).
class Wobble
{
public:
	/// `rate` cycles per unit, `units` of them elapsed this frame (seconds,
	/// beats or bars).
	void Advance( double rate, double units );
	double Phase() const
	{
		return phase;
	}

	/// The --motion negative control: the phase computed as rate x the time
	/// so far, which jumps when the rate changes.
	void SetAbsoluteForTest( bool absolute )
	{
		absoluteForTest = absolute;
	}

private:
	double phase           = 0.0;
	double elapsed         = 0.0;
	bool absoluteForTest   = false;
};

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------

/// Wyman, Sloan and Shirley's multi-lobe fit to the CIE 1931 2-degree colour
/// matching functions (JCGT 2(2), 2013). The GLSL in Shaders.cpp carries the
/// same fit; the harness carries its own copy.
void ColourMatching( double lambda, double& x, double& y, double& z );

/// The table pass's quadrature: midpoint taps over [kVisibleLow, kVisibleHigh].
/// The matrix's white balance is integrated the same way, so a spectrum that
/// reflects every wavelength equally comes out (1, 1, 1) to float rounding.
constexpr double kVisibleLow  = 360.0;
constexpr double kVisibleHigh = 830.0;
constexpr int kSpectrumTaps   = 96;

/// XYZ (integrated over nanometres, not normalised) to linear sRGB with the
/// lamp's equal-energy white as display white: row c of IEC 61966-2-1's
/// matrix divided by what that row makes of the fit's own white. Column-major,
/// as glUniformMatrix3fv takes it.
void XyzToDisplay( float columnMajor[ 9 ] );

} // namespace holofoil
