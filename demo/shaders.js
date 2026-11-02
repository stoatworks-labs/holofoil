// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \` for a backtick inside a comment.
export const VERSION = '#version 410 core\n';

// kQuadVertex, source/Shaders.cpp
export const QUAD_VERTEX = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kCommon, source/Shaders.cpp
export const COMMON = `
//Shaders.h's Hook and table constants, which this text must repeat.
const int kHookMinusLamp      = 1;
const int kHookNoDirection    = 2;
const int kHookLampAtInfinity = 4;
const int kHookNoEfficiency   = 8;
const int kHookSquareCorners  = 16;
const int kHookRoundTrip      = 32;
const int kHookOneAngle       = 64;
const int kHookCodeLuma       = 128;
const int kHookNoRoughness    = 256;
const int kHookHardReverse    = 512;
const int kHookShowGrating    = 1024;
const int kHookShowWeights    = 2048;
const int kHookPlainAdd       = 4096;

const int kTableColumns  = 2048;
const float kLambdaScale = 600.0;
const int kTableRows     = 64;
const float kSigmaLow    = 0.5;
const float kSigmaRatio  = 4000.0;
const int kTableHeight   = 129;

const float kVisibleLow  = 360.0;
const float kVisibleHigh = 830.0;
const int kSpectrumTaps  = 96;

const float kTwoPi     = 6.28318530717958648;
const float kRootTwoPi = 2.50662827463100050;

//sRGB, both ways. Light reflects and adds in linear light.
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}
`;

// kTableFragment, source/Shaders.cpp
export const TABLE_FRAGMENT = `
uniform float GrooveDepth;//nm, peak to peak
uniform mat3 XyzToDisplay;//linear sRGB, white balanced on the fit's equal-energy white
uniform int Hooks;

out vec4 fragColor;

//Wyman, Sloan and Shirley's multi-lobe fit to the CIE 1931 2-degree colour
//matching functions (JCGT 2(2), 2013, Listing 1): each lobe a Gaussian with
//its own reciprocal width either side of its peak.
float lobe( float x, float mu, float below, float above )
{
	float t = ( x - mu ) * ( x < mu ? below : above );
	return exp( -0.5 * t * t );
}
vec3 colourMatching( float lambda )
{
	return vec3( 0.362 * lobe( lambda, 442.0, 0.0624, 0.0374 ) + 1.056 * lobe( lambda, 599.8, 0.0264, 0.0323 )
	                 - 0.065 * lobe( lambda, 501.1, 0.0490, 0.0382 ),
	             0.821 * lobe( lambda, 568.8, 0.0213, 0.0247 ) + 0.286 * lobe( lambda, 530.9, 0.0613, 0.0322 ),
	             1.217 * lobe( lambda, 437.0, 0.0845, 0.0278 ) + 0.681 * lobe( lambda, 459.0, 0.0385, 0.0725 ) );
}

//J_m( x ) by its power series, the sum over k of ( -1 )^k ( x/2 )^( 2k + m ) /
//( k! ( k + m )! ). x is at most 2 pi 400 / 360 = 7 here, where 20 terms are
//below float.
float besselJ( int m, float x )
{
	float h    = 0.5 * x;
	float term = 1.0;
	for( int i = 1; i <= m; ++i )
		term *= h / float( i );
	float sum = term;
	float h2  = h * h;
	for( int k = 1; k < 20; ++k )
	{
		term *= -h2 / ( float( k ) * float( k + m ) );
		sum += term;
	}
	return sum;
}

//A thin sinusoidal relief h deep, peak to peak, reflects a wave with a phase
//swing of 2 pi h / lambda either side; the field is then a sum of plane waves
//weighted J_m of that swing (Raman-Nath), so order m carries J_m^2 of the
//light.
float efficiency( int m, float lambda )
{
	if( m == 0 && ( Hooks & kHookNoEfficiency ) != 0 )
		return 1.0;
	float j = besselJ( m, kTwoPi * GrooveDepth / lambda );
	return j * j;
}

void main()
{
	ivec2 at = ivec2( gl_FragCoord.xy );
	vec3 xyz = vec3( 0.0 );
	if( at.y >= 2 * kTableRows )
	{
		//The zeroth order sends every wavelength the one way, the mirror's:
		//its colour is the whole spectrum times J0^2.
		float step = ( kVisibleHigh - kVisibleLow ) / float( kSpectrumTaps );
		for( int i = 0; i < kSpectrumTaps; ++i )
		{
			float lambda = kVisibleLow + ( float( i ) + 0.5 ) * step;
			xyz += colourMatching( lambda ) * efficiency( 0, lambda ) * step;
		}
	}
	else
	{
		//A spectral line of one order: a Gaussian of unit area, centred where
		//this column says and as wide as this row says, through the colour
		//matching functions and the groove's efficiency for the order.
		int order    = at.y < kTableRows ? 1 : 2;
		int row      = at.y - ( order - 1 ) * kTableRows;
		float sigma  = kSigmaLow * pow( kSigmaRatio, float( row ) / float( kTableRows - 1 ) );
		float w      = ( float( at.x ) + 0.5 ) / float( kTableColumns ) * 2.0 - 1.0;
		float centre = kLambdaScale * w / ( 1.0 - abs( w ) );
		float lo     = max( kVisibleLow, centre - 6.0 * sigma );
		float hi     = min( kVisibleHigh, centre + 6.0 * sigma );
		if( hi > lo )
		{
			float step = ( hi - lo ) / float( kSpectrumTaps );
			float norm = step / ( kRootTwoPi * sigma );
			for( int i = 0; i < kSpectrumTaps; ++i )
			{
				float lambda = lo + ( float( i ) + 0.5 ) * step;
				float t      = ( lambda - centre ) / sigma;
				xyz += colourMatching( lambda ) * ( efficiency( order, lambda ) * exp( -0.5 * t * t ) * norm );
			}
		}
	}
	fragColor = vec4( XyzToDisplay * xyz, 1.0 );
}
`;

// kFoilFragment, source/Shaders.cpp
export const FOIL_FRAGMENT = `
uniform sampler2D InputTexture;
uniform sampler2D Table;
uniform ivec2 Size;      //the picture, pixels
uniform ivec2 ViewOrigin;//the host viewport's corner
uniform float PxToWidth; //picture widths a pixel

//The eye and the lamp in the card's frame (Card.h): w = 1 a point, 0 a
//direction (infinitely far).
uniform vec4 Eye;
uniform vec4 Lamp;
uniform vec3 LampSpread; //the lamp's covariance in direction cosines (Card.h): xx, xy, yy
uniform float RoughSigma;//the spread of the surface's normal below a pixel
uniform float Brightness;

//The grating.
uniform float Pitch;     //nm
uniform vec2 Direction;  //across the lines (Grating Angle)
uniform vec2 HatchA;     //the crosshatch's two families, across their lines
uniform vec2 HatchB;
uniform int PatternIndex;
uniform float Cell;      //the pattern's cell, picture widths
uniform float FacetSlope;//the most a cell's surface leans, as a slope

//The print over it.
uniform float Ink;
uniform float Base;

//Where the foil is.
uniform int AreaIndex;
uniform vec4 Window;     //centre and half-size, pixels
uniform float Corner;    //pixels
uniform float Ramp;      //the window's edge, pixels: one, plus Softness
uniform float Threshold; //the ink's luminance, as a code value
uniform float Band;      //half-width of the ramp about it
uniform float MixAmount;
uniform int Hooks;

out vec4 fragColor;

//---------------------------------------------------------------------------
// Randomness is integer (PCG): a GPU's sin is not the same on two GPUs.
//---------------------------------------------------------------------------
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
uint cellHash( ivec2 c, uint salt )
{
	return pcg( uint( c.x ) * 1597334677u ^ pcg( uint( c.y ) ^ ( salt * 2654435761u ) ) );
}
//24 bits, exactly representable: [0, 1).
float unitOf( uint h )
{
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}

//( cos, sin ) of t turns. Reduced to the nearest quarter turn, then Taylor
//on |x| <= pi/4: the sine's first missing term is under 1.8e-9, the cosine's
//1.1e-10.
vec2 cosSinTurns( float t )
{
	float u  = t - floor( t );
	float q  = floor( 4.0 * u + 0.5 );
	float x  = ( u - 0.25 * q ) * kTwoPi;
	float x2 = x * x;
	float s  = x * ( 1.0 + x2 * ( -1.0 / 6.0 + x2 * ( 1.0 / 120.0 + x2 * ( -1.0 / 5040.0 + x2 * ( 1.0 / 362880.0 ) ) ) ) );
	float c  = 1.0 + x2 * ( -0.5 + x2 * ( 1.0 / 24.0 + x2 * ( -1.0 / 720.0 + x2 * ( 1.0 / 40320.0 + x2 * ( -1.0 / 3628800.0 ) ) ) ) );
	int k    = int( q ) & 3;
	if( k == 0 )
		return vec2( c, s );
	if( k == 1 )
		return vec2( -s, c );
	if( k == 2 )
		return vec2( -c, -s );
	return vec2( s, -c );
}

//---------------------------------------------------------------------------
// The pattern: what grating the foil has at a point.
//
// g1 and g2 are grating directions (across the lines) with weights w1 and w2
// (one grating: w1 1; where two cross, half each; bare mirror: both 0);
// slope is the surface's lean, (dz/dx, dz/dy) of its normal; pitch scales
// Pitch.
//---------------------------------------------------------------------------
struct Surface
{
	vec2 g1;
	float w1;
	vec2 g2;
	float w2;
	vec2 slope;
	float pitch;
};

Surface oneGrating( vec2 g )
{
	return Surface( g, 1.0, vec2( 1.0, 0.0 ), 0.0, vec2( 0.0 ), 1.0 );
}

//A cell's own grating: its direction uniform over half a turn (a grating's
//direction has no sign), its pitch within 0.8 to 1.25 of the foil's,
//geometrically, and its surface leaning in a uniformly random direction by up
//to Facet Tilt, uniform over the disc of slopes.
Surface cellGrating( ivec2 c )
{
	vec2 g      = cosSinTurns( 0.5 * unitOf( cellHash( c, 1u ) ) );
	float lean  = FacetSlope * sqrt( unitOf( cellHash( c, 2u ) ) );
	vec2 slope  = lean * cosSinTurns( unitOf( cellHash( c, 3u ) ) );
	float pitch = exp2( ( unitOf( cellHash( c, 4u ) ) - 0.5 ) * 0.643856 );
	return Surface( g, 1.0, vec2( 1.0, 0.0 ), 0.0, slope, pitch );
}

//Two families of lines at Grating Angle +- 45 degrees, half of each family's
//period a line; a line's grooves run along it. Off both, bare mirror.
Surface crosshatch( vec2 p )
{
	float a  = dot( p, HatchA ) / Cell;
	float b  = dot( p, HatchB ) / Cell;
	bool onA = abs( a - floor( a + 0.5 ) ) < 0.25;
	bool onB = abs( b - floor( b + 0.5 ) ) < 0.25;
	float wa = onA ? ( onB ? 0.5 : 1.0 ) : 0.0;
	float wb = onB ? ( onA ? 0.5 : 1.0 ) : 0.0;
	return Surface( HatchA, wa, HatchB, wb, vec2( 0.0 ), 1.0 );
}

//Discs of every size, mostly small, one a cell at a jittered place, each its
//own grating; where two overlap the one stamped later is on top. Between
//them, the plain grating.
Surface cosmos( vec2 p )
{
	vec2 q     = p / Cell;
	ivec2 base = ivec2( floor( q ) );
	float top  = -1.0;
	ivec2 hit  = base;
	for( int j = -1; j <= 1; ++j )
		for( int i = -1; i <= 1; ++i )
		{
			ivec2 c       = base + ivec2( i, j );
			vec2 centre   = vec2( c ) + 0.5 + 0.6 * ( vec2( unitOf( cellHash( c, 5u ) ), unitOf( cellHash( c, 6u ) ) ) - 0.5 );
			float u       = unitOf( cellHash( c, 7u ) );
			float radius  = 0.12 + 0.43 * u * u * u;
			vec2 d        = q - centre;
			float stamped = unitOf( cellHash( c, 8u ) );
			if( dot( d, d ) < radius * radius && stamped > top )
			{
				top = stamped;
				hit = c;
			}
		}
	if( top < 0.0 )
		return oneGrating( Direction );
	return cellGrating( hit );
}

//Shards: the cells of the nearest of one jittered seed a cell (searched over
//the 3 x 3 cells around, which misplaces rare slivers), each its own grating.
Surface crackedIce( vec2 p )
{
	vec2 q        = p / Cell;
	ivec2 base    = ivec2( floor( q ) );
	float nearest = 1e9;
	ivec2 hit     = base;
	for( int j = -1; j <= 1; ++j )
		for( int i = -1; i <= 1; ++i )
		{
			ivec2 c   = base + ivec2( i, j );
			vec2 seed = vec2( c ) + vec2( unitOf( cellHash( c, 9u ) ), unitOf( cellHash( c, 10u ) ) );
			vec2 d    = q - seed;
			float d2  = dot( d, d );
			if( d2 < nearest )
			{
				nearest = d2;
				hit     = c;
			}
		}
	return cellGrating( hit );
}

Surface surfaceAt( vec2 p )
{
	if( PatternIndex == 1 )
		return crosshatch( p );
	if( PatternIndex == 2 )
		return cosmos( p );
	if( PatternIndex == 3 )
		return crackedIce( p );
	if( PatternIndex == 4 )
		return cellGrating( ivec2( floor( p / Cell ) ) );
	return oneGrating( Direction );
}

//---------------------------------------------------------------------------
// The light.
//---------------------------------------------------------------------------

//The colour of a spectral line of unit area, centred at \`centre\` nm and
//\`sigma\` nm wide, in order 1 or 2: the table, between its columns and rows.
vec3 lineColour( float centre, float sigma, int order )
{
	float u = 0.5 + 0.5 * centre / ( abs( centre ) + kLambdaScale );
	float k = clamp( log( sigma / kSigmaLow ) / log( kSigmaRatio ) * float( kTableRows - 1 ), 0.0, float( kTableRows - 1 ) );
	float v = ( float( ( order - 1 ) * kTableRows ) + k + 0.5 ) / float( kTableHeight );
	return texture( Table, vec2( u, v ) ).rgb;
}

//What the foil at p sends to the eye, in linear light (the lamp at
//Brightness 1 seen in a perfect mirror is 1).
//
//With v the way to the eye and l the way to the lamp, both unit, the light
//that reaches the eye off a grating of direction g and period d in order m
//is the wavelength for which ( v + l )'s part in the surface is m lambda / d
//along g and nothing across it: the grating equation in direction cosines.
//The lamp is not a point: its brightness is a Gaussian of direction, of
//covariance S. The light of wavelength lambda that reaches the eye left the
//lamp in the direction whose part in the surface is m lambda / d g - v, so
//the eye sees the lamp's Gaussian along the line t g, t = m lambda / d, at
//s = h.xy: a Gaussian in lambda, centred where the line comes closest to s
//in S's metric, and dimmed by how close that is. The zeroth order (m = 0) is
//the mirror: the lamp's own Gaussian at s.
vec3 sheenAt( vec2 p, Surface sf )
{
	vec3 at = vec3( p, 0.0 );
	vec3 v  = normalize( Eye.xyz - Eye.w * at );
	vec3 l  = normalize( Lamp.xyz - ( ( Hooks & kHookLampAtInfinity ) != 0 ? 0.0 : Lamp.w ) * at );
	vec3 h  = v + l;
	if( ( Hooks & kHookMinusLamp ) != 0 )
		h.xy = v.xy - l.xy;

	//In a leaning cell's own plane, to first order in its slope: the mirror
	//condition there is h.xy = h.z slope.
	vec2 s = h.xy - h.z * sf.slope;

	//Roughness spreads the normal by RoughSigma, which spreads the reflection
	//by h.z times that: the lamp seen in it is wider by that much in
	//quadrature every way, and dimmer to keep its light.
	float rough = ( Hooks & kHookNoRoughness ) != 0 ? 0.0 : h.z * RoughSigma;
	vec3 cov    = LampSpread + vec3( rough * rough, 0.0, rough * rough );
	float det   = cov.x * cov.z - cov.y * cov.y;
	mat2 inv    = mat2( cov.z, -cov.y, -cov.y, cov.x ) / det;
	float gain  = sqrt( ( LampSpread.x * LampSpread.z - LampSpread.y * LampSpread.y ) / det );

	//The mirror is white where the foil is bare and the zeroth order's
	//colour where there is a grating.
	float covered = min( sf.w1 + sf.w2, 1.0 );
	vec3 mirror   = mix( vec3( 1.0 ), texelFetch( Table, ivec2( 0, 2 * kTableRows ), 0 ).rgb, covered );
	vec3 light    = mirror * exp( -0.5 * dot( s, inv * s ) );

	float pitch = Pitch * sf.pitch;
	for( int n = 0; n < 2; ++n )
	{
		float weight = n == 0 ? sf.w1 : sf.w2;
		if( weight <= 0.0 )
			continue;
		vec2 g = n == 0 ? sf.g1 : sf.g2;
		if( ( Hooks & kHookOneAngle ) != 0 )
			g = Direction;
		if( ( Hooks & kHookNoDirection ) != 0 && dot( s, s ) > 0.0 )
			g = normalize( s );
		//Along the line, the lamp's Gaussian is centred at t = g.S^-1 s /
		//g.S^-1 g and 1 / sqrt( g.S^-1 g ) wide; it comes closest to s at
		//( n.s )^2 / n.S n, n across g (the distance to a line in S's metric).
		vec2 ig      = inv * g;
		float a      = dot( g, ig );
		float along  = dot( s, ig ) / a;
		float spread = inversesqrt( a );
		vec2 across  = vec2( -g.y, g.x );
		float miss   = dot( across, s );
		float reach  = cov.x * across.x * across.x + 2.0 * cov.y * across.x * across.y + cov.z * across.y * across.y;
		float band   = weight * exp( -0.5 * miss * miss / reach );
		for( int order = 1; order <= 2; ++order )
		{
			//Orders +m and -m: lines centred either side of zero, of which at
			//most one is in the visible unless the lamp is very wide.
			float width  = pitch * spread / float( order );
			float centre = pitch * along / float( order );
			light += band * kRootTwoPi * width * ( lineColour( centre, width, order ) + lineColour( -centre, width, order ) );
		}
	}
	light *= Brightness * gain;

	//A spectral colour is outside sRGB. Add white until it is not: that
	//moves it straight toward white, so its dominant wavelength stays.
	float lowest = min( light.r, min( light.g, light.b ) );
	return light - vec3( min( lowest, 0.0 ) );
}

//---------------------------------------------------------------------------
// Where the foil is.
//---------------------------------------------------------------------------

//The rounded rectangle's signed distance, as a pixel's coverage: a ramp one
//pixel (plus Softness) wide across the edge, which is the exact coverage for
//a straight edge.
float windowCoverage( vec2 px, bool smoothed )
{
	float r  = ( Hooks & kHookSquareCorners ) != 0 ? 0.0 : Corner;
	vec2 q   = abs( px - Window.xy ) - Window.zw + vec2( r );
	float sd = length( max( q, vec2( 0.0 ) ) ) + min( max( q.x, q.y ), 0.0 ) - r;
	if( !smoothed )
		return sd < 0.0 ? 1.0 : 0.0;
	return clamp( 0.5 - sd / Ramp, 0.0, 1.0 );
}

//The ink's luminance (Rec. 709 weights on linear light), encoded back to a
//code value so Threshold reads like the picture does.
float inkLevel( vec3 light, vec3 code )
{
	if( ( Hooks & kHookCodeLuma ) != 0 )
		return dot( code, vec3( 0.2126, 0.7152, 0.0722 ) );
	return toCode( vec3( dot( light, vec3( 0.2126, 0.7152, 0.0722 ) ) ) ).r;
}

float aboveThreshold( float level )
{
	if( Band <= 0.0 )
		return level > Threshold ? 1.0 : ( level < Threshold ? 0.0 : 0.5 );
	return clamp( 0.5 + 0.5 * ( level - Threshold ) / Band, 0.0, 1.0 );
}

float areaOf( vec2 px, vec4 src, vec3 light )
{
	if( AreaIndex == 0 )
		return windowCoverage( px, true );
	if( AreaIndex == 1 )
		return 1.0 - windowCoverage( px, ( Hooks & kHookHardReverse ) == 0 );
	if( AreaIndex == 2 )
		return 1.0;
	if( AreaIndex == 3 )
		return aboveThreshold( inkLevel( light, src.rgb ) );
	if( AreaIndex == 4 )
		return 1.0 - aboveThreshold( inkLevel( light, src.rgb ) );
	return src.a;
}

void main()
{
	ivec2 pix = ivec2( gl_FragCoord.xy ) - ViewOrigin;
	//texelFetch, not texture(): where there is no foil the clip comes through
	//untouched, bit for bit, and a filtered read at a texel's centre is not
	//promised to be the texel on every rasteriser.
	vec4 src = texelFetch( InputTexture, clamp( pix, ivec2( 0 ), Size - ivec2( 1 ) ), 0 );
	vec2 px  = vec2( pix ) + 0.5;
	vec2 p   = ( px - 0.5 * vec2( Size ) ) * PxToWidth;

	if( ( Hooks & ( kHookShowGrating | kHookShowWeights ) ) != 0 )
	{
		Surface shown = surfaceAt( p );
		fragColor     = ( Hooks & kHookShowGrating ) != 0 ? vec4( shown.g1, shown.slope ) : vec4( shown.w1, shown.w2, shown.pitch, 0.0 );
		return;
	}

	vec3 light = toLinear( src.rgb );
	float m    = areaOf( px, src, light ) * MixAmount;
	if( m <= 0.0 && ( Hooks & kHookRoundTrip ) == 0 )
	{
		fragColor = src;
		return;
	}

	//The ink is a filter the light crosses twice, so what shows through it
	//of the foil is the foil times the ink's colour on white paper. Ink 1 is
	//foil under the print; Ink 0 a clear foil film over it.
	vec3 sheen = sheenAt( p, surfaceAt( p ) );
	vec3 foil  = light * mix( 1.0, Base, Ink ) + sheen * mix( vec3( 1.0 ), light, Ink );
	if( ( Hooks & kHookPlainAdd ) != 0 )
		foil = light + sheen;
	vec3 lit   = m >= 1.0 ? foil : mix( light, foil, m );
	fragColor  = vec4( toCode( lit ), src.a );
}
`;
