#include "Card.h"

#include <algorithm>
#include <cmath>

namespace holofoil
{
namespace
{
/// r_card = Ry( Tx ) Rx( -Ty ) r_room.
void toCard( double tiltX, double tiltY, double& x, double& y, double& z )
{
	const double cy = std::cos( tiltY ), sy = std::sin( tiltY );
	const double y1 = y * cy + z * sy;
	const double z1 = -y * sy + z * cy;
	const double cx = std::cos( tiltX ), sx = std::sin( tiltX );
	const double x2 = x * cx + z1 * sx;
	const double z2 = -x * sx + z1 * cx;
	x = x2;
	y = y1;
	z = z2;
}

Place placed( double x, double y, double z, double inverseDistance )
{
	Place p;
	if( inverseDistance > 0.0 )
	{
		p.x = x / inverseDistance;
		p.y = y / inverseDistance;
		p.z = z / inverseDistance;
		p.w = 1.0;
	}
	else
	{
		p.x = x;
		p.y = y;
		p.z = z;
		p.w = 0.0;
	}
	return p;
}

/// One lobe of the fit, as the paper's Listing 1 writes it: exp( -t^2 / 2 ),
/// t = ( x - mu ) times the reciprocal width on that side.
double lobe( double x, double mu, double below, double above )
{
	const double t = ( x - mu ) * ( x < mu ? below : above );
	return std::exp( -0.5 * t * t );
}
} // namespace

Place EyeInCard( double tiltX, double tiltY, double inverseDistance )
{
	double x = 0.0, y = 0.0, z = 1.0;
	toCard( tiltX, tiltY, x, y, z );
	return placed( x, y, z, inverseDistance );
}

Place LampInCard( double tiltX, double tiltY, double lampX, double lampY, double inverseDistance )
{
	double x = std::cos( lampY ) * std::sin( lampX );
	double y = std::sin( lampY );
	double z = std::cos( lampY ) * std::cos( lampX );
	toCard( tiltX, tiltY, x, y, z );
	return placed( x, y, z, inverseDistance );
}

Spread LampSpread( double tiltX, double tiltY, double lampX, double lampY, double size, double length )
{
	//The two ways the lamp extends, as room directions at right angles to it:
	//level (along the strip) and up.
	double ax = std::cos( lampX ), ay = 0.0, az = -std::sin( lampX );
	double bx = -std::sin( lampY ) * std::sin( lampX ), by = std::cos( lampY ), bz = -std::sin( lampY ) * std::cos( lampX );
	toCard( tiltX, tiltY, ax, ay, az );
	toCard( tiltX, tiltY, bx, by, bz );
	const double along  = std::min( size * length, kLongestStrip );
	const double across = size;
	Spread s;
	s.xx = along * along * ax * ax + across * across * bx * bx;
	s.xy = along * along * ax * ay + across * across * bx * by;
	s.yy = along * along * ay * ay + across * across * by * by;
	return s;
}

Pose PoseOf( Motion motion, double depth, double phase, const Pose& set )
{
	Pose pose      = set;
	const double a = 2.0 * kPi * phase;
	switch( motion )
	{
	case Motion::Hold:
	case Motion::Count: break;
	case Motion::Tip: pose.tiltY += depth * std::sin( a ); break;
	case Motion::Swing: pose.tiltX += depth * std::sin( a ); break;
	case Motion::Circle:
		pose.tiltX += depth * std::sin( a );
		pose.tiltY += depth * std::cos( a );
		break;
	case Motion::LampOrbit:
		pose.lampX += depth * std::sin( a );
		pose.lampY += depth * std::cos( a );
		break;
	}
	return pose;
}

void Wobble::Advance( double rate, double units )
{
	elapsed += units;
	if( absoluteForTest )
		phase = rate * elapsed;
	else
		phase += rate * units;
	phase -= std::floor( phase );
}

void ColourMatching( double lambda, double& x, double& y, double& z )
{
	x = 0.362 * lobe( lambda, 442.0, 0.0624, 0.0374 ) + 1.056 * lobe( lambda, 599.8, 0.0264, 0.0323 )
	    - 0.065 * lobe( lambda, 501.1, 0.0490, 0.0382 );
	y = 0.821 * lobe( lambda, 568.8, 0.0213, 0.0247 ) + 0.286 * lobe( lambda, 530.9, 0.0613, 0.0322 );
	z = 1.217 * lobe( lambda, 437.0, 0.0845, 0.0278 ) + 0.681 * lobe( lambda, 459.0, 0.0385, 0.0725 );
}

void XyzToDisplay( float columnMajor[ 9 ] )
{
	//IEC 61966-2-1, XYZ (D65) to linear sRGB.
	const double m[ 3 ][ 3 ] = {
		{ 3.2406, -1.5372, -0.4986 },
		{ -0.9689, 1.8758, 0.0415 },
		{ 0.0557, -0.2040, 1.0570 },
	};
	//The fit's own equal-energy white, by the table pass's quadrature.
	double white[ 3 ] = { 0.0, 0.0, 0.0 };
	const double step = ( kVisibleHigh - kVisibleLow ) / kSpectrumTaps;
	for( int i = 0; i < kSpectrumTaps; ++i )
	{
		double x, y, z;
		ColourMatching( kVisibleLow + ( i + 0.5 ) * step, x, y, z );
		white[ 0 ] += x * step;
		white[ 1 ] += y * step;
		white[ 2 ] += z * step;
	}
	for( int row = 0; row < 3; ++row )
	{
		const double made = m[ row ][ 0 ] * white[ 0 ] + m[ row ][ 1 ] * white[ 1 ] + m[ row ][ 2 ] * white[ 2 ];
		for( int col = 0; col < 3; ++col )
			columnMajor[ col * 3 + row ] = static_cast< float >( m[ row ][ col ] / made );
	}
}

} // namespace holofoil
