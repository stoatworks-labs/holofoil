/**
    hftest -- render Holofoil offline, and measure the light the foil sends
    to the eye against the optics it claims.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context, and reads the
    host's output back as floats. What the picture is measured against is the
    optics, worked out here again and differently: the eye and the lamp put
    in the card's frame by rotation matrices (the plugin composes two
    rotations by hand), the spectrum a grating sends to the eye integrated
    wavelength by wavelength through this file's own copy of the colour
    matching functions (the plugin reads a table of pre-integrated lines),
    the groove's efficiency from Bessel's integral (the plugin sums the power
    series), and a colour's dominant wavelength found on the spectral locus.

    The pictures are built so that the output reads back as a measurement:

      - on a BLACK clip with Ink 0 the output is the foil's own light, so a
        pixel's colour is what the grating sent there;
      - on a black clip with a uniform sheen (eye and lamp at infinity) the
        output over the sheen is the foil's coverage, pixel by pixel.

        hftest --out /tmp/holofoil.png    the defaults on the harness's card
        hftest --list                     every parameter and its default
        hftest --pipe                     raw RGBA frames in on stdin, out on stdout
        hftest --film N                   N frames of the card, raw RGBA on stdout
        hftest --offline                  the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option or an event STEPS.

    HFTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Card.h"
#include "Controls.h"
#include "Holofoil.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace holofoil;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

/// How far apart two pixels are, the worst channel, relative to the larger;
/// alpha must match exactly.
double relativeApart( const float* a, const float* b )
{
	double worst = 0.0;
	for( int c = 0; c < 4; ++c )
	{
		if( a[ c ] == b[ c ] )
			continue;
		if( c == 3 )
			return std::numeric_limits< double >::infinity();
		const double big = std::max( std::fabs( a[ c ] ), std::fabs( b[ c ] ) );
		worst            = std::max( worst, std::fabs( static_cast< double >( a[ c ] ) - b[ c ] ) / big );
	}
	return worst;
}

/// What two DRAWS of the same pixel through the same arithmetic may differ
/// by. On a GPU, nothing: bit for bit. Apple's software renderer does not
/// promise that: redrawing one unchanged frame there, the first few draws
/// after a program is linked differ from the later ones by up to 5.7e-6
/// (84 float steps), in blocks of rows, by a different amount each run --
/// consistent with the renderer swapping in a differently compiled shader
/// part way through a draw. So on it a claim that two draws agree is a claim
/// to 2e-5. The clip's own pixels (texelFetch, passed through) are bit for
/// bit everywhere, and every check of them says so.
double drawsAgreeWithin()
{
	return g_software ? 2e-5 : 0.0;
}
std::string agreement( double apart )
{
	if( !g_software )
		return "bit for bit";
	char text[ 96 ];
	std::snprintf( text, sizeof text, "software renderer: within %.0e, worst %.1e", drawsAgreeWithin(), apart );
	return text;
}

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "HFTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "hftest: HFTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_XPOS: return "x";
	case FF_TYPE_YPOS: return "y";
	case FF_TYPE_BUFFER: return "buffer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch or a press: cues STEP between
/// keys for these, and everything else ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}


//---------------------------------------------------------------------------
// sRGB in double, for the references.
//---------------------------------------------------------------------------
double linearOf( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
double codeOf( double light )
{
	return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow( light, 1.0 / 2.4 ) - 0.055;
}


//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// Eight bits, as a host's clip is.
float eightBit( double v )
{
	return static_cast< float >( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) / 255.0 );
}

/// The look card at t seconds: a sunset sky, a sun crossing it, a skyline and
/// a stripe of saturated bars scrolling under it, and a white title block --

/// The harness's card: a trading card's layout, portrait or not -- a border,
/// a name bar, an art window (a sky, a sun, mountains and a lake) exactly
/// where the default Window puts it, a type line, a text box of grey lines
/// and a strip of small print. Edges, flats, light and dark, and colour.
/// `t` slides the sun, so a film shows the art is live.
Floats buildCard( int width, int height, double t = 0.0 )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double aspect = static_cast< double >( width ) / height;
	auto inBox          = []( double u, double v, double u0, double v0, double u1, double v1 ) {
        return u >= u0 && u < u1 && v >= v0 && v < v1;
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			double r = 0.16, g = 0.18, b = 0.24;//the border
			if( inBox( u, v, 0.035, 0.025, 0.965, 0.975 ) )
			{
				//The frame: a cool metal-looking gradient.
				const double k = 0.5 + 0.5 * ( v - u ) ;
				r = 0.50 + 0.12 * k;
				g = 0.58 + 0.10 * k;
				b = 0.62 + 0.08 * k;
			}
			if( inBox( u, v, 0.07, 0.885, 0.93, 0.945 ) )
			{
				r = 0.93, g = 0.90, b = 0.82;
				const int letter = static_cast< int >( std::floor( ( u - 0.1 ) * 60.0 ) );
				if( u > 0.1 && u < 0.52 && v > 0.9 && v < 0.93 && letter % 5 != 4 )
					r = g = b = 0.12;
				if( u > 0.82 && u < 0.9 && v > 0.9 && v < 0.93 )
					r = 0.75, g = 0.15, b = 0.12;
			}
			if( inBox( u, v, 0.08, 0.43, 0.92, 0.85 ) )
			{
				//The art: the window's own coordinates, 0..1 up and across.
				const double au = ( u - 0.08 ) / 0.84, av = ( v - 0.43 ) / 0.42;
				const double k  = std::clamp( ( av - 0.35 ) / 0.65, 0.0, 1.0 );
				r = 0.98 - 0.80 * k, g = 0.62 - 0.30 * k, b = 0.38 + 0.35 * k;
				const double sx = 0.66 - 0.05 * std::sin( 0.6 * t ), sy = 0.56;
				const double d  = std::hypot( ( au - sx ) * 0.84 * aspect, ( av - sy ) * 0.42 );
				if( d < 0.06 )
					r = 1.0, g = 0.92, b = 0.6;
				if( av > 0.72 && pcg( static_cast< uint32_t >( x ) * 7919u + static_cast< uint32_t >( y ) * 104729u ) % 900u == 0u )
					r = g = b = 1.0;
				const double ridge = 0.38 + 0.12 * std::fabs( std::sin( au * 9.0 ) ) * ( 0.6 + 0.4 * std::sin( au * 23.0 + 1.0 ) );
				if( av < ridge )
					r = 0.22, g = 0.14, b = 0.30;
				if( av < 0.22 )
				{
					//The lake: the sky again, darker, with ripples.
					const double ripple = 0.5 + 0.5 * std::sin( av * 180.0 + au * 6.0 );
					r = 0.30 + 0.25 * ripple, g = 0.30 + 0.12 * ripple, b = 0.48 + 0.10 * ripple;
				}
			}
			if( inBox( u, v, 0.07, 0.375, 0.93, 0.415 ) )
			{
				r = 0.84, g = 0.85, b = 0.84;
				if( u > 0.1 && u < 0.45 && v > 0.385 && v < 0.405 )
					r = g = b = 0.2;
			}
			if( inBox( u, v, 0.08, 0.1, 0.92, 0.36 ) )
			{
				r = 0.95, g = 0.93, b = 0.86;
				const int line = static_cast< int >( std::floor( ( 0.345 - v ) / 0.034 ) );
				const double inLine = ( 0.345 - v ) / 0.034 - line;
				const double length = 0.55 + 0.3 * hash01( static_cast< uint32_t >( line ), 3 );
				if( line >= 0 && line < 6 && inLine > 0.25 && inLine < 0.7 && u > 0.11 && u < 0.11 + length * 0.78 )
					r = g = b = 0.32;
			}
			if( inBox( u, v, 0.1, 0.045, 0.6, 0.07 ) && static_cast< int >( std::floor( u * 140.0 ) ) % 4 != 3 )
				r = g = b = 0.75;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = eightBit( r );
			o[ 1 ]   = eightBit( g );
			o[ 2 ]   = eightBit( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

void fillFlat( Floats& card, double r, double g, double b, double a = 1.0 )
{
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = static_cast< float >( r );
		card[ i + 1 ] = static_cast< float >( g );
		card[ i + 2 ] = static_cast< float >( b );
		card[ i + 3 ] = static_cast< float >( a );
	}
}

Floats flatCard( int width, int height, double r, double g, double b, double a = 1.0 )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	fillFlat( card, r, g, b, a );
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock and,
// when asked for, a synthetic beat.
//---------------------------------------------------------------------------
struct Rig
{
	HolofoilPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame  = 0;
	double fps = 60.0;
	double bpm = 0.0;///< 0: no beat sent
	/// The host's clock: SetTime( origin + seconds x unit ), declared to the
	/// plugin as `clockScale` seconds a unit.
	double timeOrigin = 0.0, timeUnit = 1.0, clockScale = 1.0;
	std::function< void( int ) > beforeFrame;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/holofoil for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( clockScale );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}
	template< typename E >
	void Option( unsigned int id, E value )
	{
		plugin.SetFloatParameter( id, static_cast< float >( static_cast< int >( value ) ) );
	}
	float Get( unsigned int id )
	{
		return plugin.GetFloatParameter( id );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			plugin.SetTime( timeOrigin + Seconds( frame ) * timeUnit );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( Seconds( frame ) * bpm / 240.0, 1.0 ) ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}
};

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

const float* pixelAt( const Floats& image, int width, int x, int y )
{
	return &image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( HolofoilPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements; anything else is refused, never read as 0 (polyhedral's
/// trap: strtof( "Scan" ) is 0, silently).
bool applySetting( HolofoilPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_TEXT || parameter.type == FF_TYPE_BUFFER )
			{
				error = name + " cannot be set from here";
				return false;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( HolofoilPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock, with a
// beat at --bpm (120 unless told otherwise). --film renders the moving card.
//===========================================================================
int runPipe( int width, int height, double fps, double bpm, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings )
{
	Rig rig;
	rig.fps = fps;
	rig.bpm = bpm;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else
			rig.Upload( buildCard( width, height, rig.Seconds( index ) ) );

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks            = 0;    ///< shaders::Hook bits
	bool snapTilt        = false;///< --smooth: the tilt in whole degrees
	bool absoluteMotion  = false;///< --motion: the phase as rate x time
	bool clockAsSeconds  = false;///< --clock: a millisecond host taken for a seconds one
	bool cuesRamp        = false;///< --cues: every control ramps
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
/// A card, portrait, and a small landscape frame: the window's fractions and
/// the optics' picture widths both have to hold at either shape.
std::vector< Raster > kRasters = { { 630, 882 }, { 320, 180 } };

void applyPerturb( Rig& rig, const Perturb& perturb )
{
	rig.plugin.SetHooksForTest( perturb.hooks );
	rig.plugin.SetSnapTiltForTest( perturb.snapTilt );
	rig.plugin.SetAbsoluteMotionForTest( perturb.absoluteMotion );
}


//===========================================================================
// The optics, again -- this file's own, and built differently.
//===========================================================================
struct V3
{
	double x = 0.0, y = 0.0, z = 0.0;
};
V3 operator+( V3 a, V3 b )
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}
V3 operator-( V3 a, V3 b )
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V3 operator*( double k, V3 a )
{
	return { k * a.x, k * a.y, k * a.z };
}
double dot( V3 a, V3 b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 cross( V3 a, V3 b )
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
V3 unit( V3 a )
{
	return ( 1.0 / std::sqrt( dot( a, a ) ) ) * a;
}

/// A rotation as a matrix, from an axis and an angle by Rodrigues' formula.
struct M3
{
	double m[ 3 ][ 3 ] = {};
	V3 operator()( V3 v ) const
	{
		return { m[ 0 ][ 0 ] * v.x + m[ 0 ][ 1 ] * v.y + m[ 0 ][ 2 ] * v.z, m[ 1 ][ 0 ] * v.x + m[ 1 ][ 1 ] * v.y + m[ 1 ][ 2 ] * v.z,
			     m[ 2 ][ 0 ] * v.x + m[ 2 ][ 1 ] * v.y + m[ 2 ][ 2 ] * v.z };
	}
};
M3 rotation( V3 axis, double angle )
{
	const V3 k     = unit( axis );
	const double c = std::cos( angle ), s = std::sin( angle ), t = 1.0 - c;
	const double a[ 3 ] = { k.x, k.y, k.z };
	M3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			r.m[ i ][ j ] = t * a[ i ] * a[ j ] + ( i == j ? c : 0.0 );
	r.m[ 0 ][ 1 ] -= s * k.z;
	r.m[ 0 ][ 2 ] += s * k.y;
	r.m[ 1 ][ 0 ] += s * k.z;
	r.m[ 1 ][ 2 ] -= s * k.x;
	r.m[ 2 ][ 0 ] -= s * k.y;
	r.m[ 2 ][ 1 ] += s * k.x;
	return r;
}
M3 operator*( const M3& a, const M3& b )
{
	M3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			for( int k = 0; k < 3; ++k )
				r.m[ i ][ j ] += a.m[ i ][ k ] * b.m[ k ][ j ];
	return r;
}

/// The room seen from the card (Card.h's contract): the card turned about its
/// vertical axis by Tilt X and then its horizontal one by Tilt Y, so the eye
/// -- the room's +z -- is at azimuth Tx and elevation Ty from its normal.
M3 roomToCard( double tiltX, double tiltY )
{
	return rotation( { 0.0, 1.0, 0.0 }, tiltX ) * rotation( { 1.0, 0.0, 0.0 }, -tiltY );
}

/// The direction (azimuth across, elevation up) as a room vector.
V3 directionOf( double azimuth, double elevation )
{
	return { std::cos( elevation ) * std::sin( azimuth ), std::sin( elevation ), std::cos( elevation ) * std::cos( azimuth ) };
}

/// The colour matching functions: Wyman, Sloan and Shirley's multi-lobe fit
/// (JCGT 2(2), 2013, Listing 1), written out again.
void matching( double l, double& x, double& y, double& z )
{
	auto g = []( double v, double mu, double s1, double s2 ) {
		const double t = ( v - mu ) * ( v < mu ? s1 : s2 );
		return std::exp( -t * t / 2.0 );
	};
	x = 1.056 * g( l, 599.8, 0.0264, 0.0323 ) + 0.362 * g( l, 442.0, 0.0624, 0.0374 ) - 0.065 * g( l, 501.1, 0.0490, 0.0382 );
	y = 0.821 * g( l, 568.8, 0.0213, 0.0247 ) + 0.286 * g( l, 530.9, 0.0613, 0.0322 );
	z = 1.217 * g( l, 437.0, 0.0845, 0.0278 ) + 0.681 * g( l, 459.0, 0.0385, 0.0725 );
}

/// J_m( x ) by Bessel's integral, ( 1 / pi ) times the integral over [0, pi]
/// of cos( m t - x sin t ): the trapezoid rule on a periodic integrand,
/// exact to double long before 400 points. The plugin sums the power series.
double besselIntegral( int m, double x )
{
	constexpr int kSteps = 400;
	double sum           = 0.0;
	for( int i = 0; i <= kSteps; ++i )
	{
		const double t = kPi * i / kSteps;
		const double w = ( i == 0 || i == kSteps ) ? 0.5 : 1.0;
		sum += w * std::cos( m * t - x * std::sin( t ) );
	}
	return sum / kSteps;
}

/// The spectrum, wavelength by wavelength: 0.25 nm steps across the visible.
constexpr double kStep  = 0.25;
constexpr int kLambdas = static_cast< int >( ( 830.0 - 360.0 ) / kStep );
double lambdaAt( int i )
{
	return 360.0 + ( i + 0.5 ) * kStep;
}

struct Colour
{
	double r = 0.0, g = 0.0, b = 0.0;
};

/// XYZ to linear sRGB (IEC 61966-2-1), white balanced so that a flat
/// spectrum of 1 is ( 1, 1, 1 ), and back.
struct Display
{
	double toRgb[ 3 ][ 3 ];
	double toXyz[ 3 ][ 3 ];
	double whiteX = 1.0 / 3.0, whiteY = 1.0 / 3.0;//the fit's own equal-energy white
	std::vector< double > cx, cy, cz;
	Display()
	{
		const double m[ 3 ][ 3 ] = { { 3.2406, -1.5372, -0.4986 }, { -0.9689, 1.8758, 0.0415 }, { 0.0557, -0.2040, 1.0570 } };
		cx.resize( kLambdas );
		cy.resize( kLambdas );
		cz.resize( kLambdas );
		double w[ 3 ] = { 0.0, 0.0, 0.0 };
		for( int i = 0; i < kLambdas; ++i )
		{
			matching( lambdaAt( i ), cx[ i ], cy[ i ], cz[ i ] );
			w[ 0 ] += cx[ i ] * kStep;
			w[ 1 ] += cy[ i ] * kStep;
			w[ 2 ] += cz[ i ] * kStep;
		}
		//The fit's integrals are not quite equal, as the CIE's are by
		//construction, so its white is not quite ( 1/3, 1/3 ): near the red
		//end, where the locus is all but straight, that is worth 10 nm.
		whiteX = w[ 0 ] / ( w[ 0 ] + w[ 1 ] + w[ 2 ] );
		whiteY = w[ 1 ] / ( w[ 0 ] + w[ 1 ] + w[ 2 ] );
		for( int r = 0; r < 3; ++r )
		{
			const double made = m[ r ][ 0 ] * w[ 0 ] + m[ r ][ 1 ] * w[ 1 ] + m[ r ][ 2 ] * w[ 2 ];
			for( int c = 0; c < 3; ++c )
				toRgb[ r ][ c ] = m[ r ][ c ] / made;
		}
		//The inverse, by cofactors.
		const double( &a )[ 3 ][ 3 ] = toRgb;
		const double det = a[ 0 ][ 0 ] * ( a[ 1 ][ 1 ] * a[ 2 ][ 2 ] - a[ 1 ][ 2 ] * a[ 2 ][ 1 ] )
		                   - a[ 0 ][ 1 ] * ( a[ 1 ][ 0 ] * a[ 2 ][ 2 ] - a[ 1 ][ 2 ] * a[ 2 ][ 0 ] )
		                   + a[ 0 ][ 2 ] * ( a[ 1 ][ 0 ] * a[ 2 ][ 1 ] - a[ 1 ][ 1 ] * a[ 2 ][ 0 ] );
		for( int r = 0; r < 3; ++r )
			for( int c = 0; c < 3; ++c )
			{
				const int r1 = ( c + 1 ) % 3, r2 = ( c + 2 ) % 3, c1 = ( r + 1 ) % 3, c2 = ( r + 2 ) % 3;
				toXyz[ r ][ c ] = ( a[ r1 ][ c1 ] * a[ r2 ][ c2 ] - a[ r1 ][ c2 ] * a[ r2 ][ c1 ] ) / det;
			}
	}
	Colour rgbOf( double x, double y, double z ) const
	{
		return { toRgb[ 0 ][ 0 ] * x + toRgb[ 0 ][ 1 ] * y + toRgb[ 0 ][ 2 ] * z, toRgb[ 1 ][ 0 ] * x + toRgb[ 1 ][ 1 ] * y + toRgb[ 1 ][ 2 ] * z,
			     toRgb[ 2 ][ 0 ] * x + toRgb[ 2 ][ 1 ] * y + toRgb[ 2 ][ 2 ] * z };
	}
	/// Chromaticity ( x, y ) of a linear sRGB colour.
	void chromaticity( const Colour& c, double& x, double& y ) const
	{
		const double X = toXyz[ 0 ][ 0 ] * c.r + toXyz[ 0 ][ 1 ] * c.g + toXyz[ 0 ][ 2 ] * c.b;
		const double Y = toXyz[ 1 ][ 0 ] * c.r + toXyz[ 1 ][ 1 ] * c.g + toXyz[ 1 ][ 2 ] * c.b;
		const double Z = toXyz[ 2 ][ 0 ] * c.r + toXyz[ 2 ][ 1 ] * c.g + toXyz[ 2 ][ 2 ] * c.b;
		const double sum = X + Y + Z;
		x = X / sum;
		y = Y / sum;
	}
	/// The dominant wavelength against equal-energy white -- the white a
	/// spectrum of 1 makes, which is what the foil adds to a colour to bring
	/// it into gamut, so adding it leaves this alone: where on the
	/// spectral locus the line from white through the colour lands, between
	/// 400 and 650 nm. Past 650 the fit's locus curls back on itself as its
	/// lobes die away at different rates, and a red can land there twice.
	/// Also the excitation purity: how far along that line the colour is.
	double dominant( const Colour& c, double* purity = nullptr ) const
	{
		double x, y;
		chromaticity( c, x, y );
		const double angle = std::atan2( y - whiteY, x - whiteX );
		double best = 0.0, bestMiss = 1e9, prevMiss = 0.0, prevLambda = 0.0;
		for( double l = 400.0; l <= 650.0; l += 0.01 )
		{
			double X, Y, Z;
			matching( l, X, Y, Z );
			const double lx = X / ( X + Y + Z ), ly = Y / ( X + Y + Z );
			double miss = std::atan2( ly - whiteY, lx - whiteX ) - angle;
			miss        = std::remainder( miss, 2.0 * kPi );
			//The locus crosses the colour's direction between two steps: interpolate.
			if( l > 400.0 && ( miss > 0.0 ) != ( prevMiss > 0.0 ) && std::fabs( miss - prevMiss ) < 0.5 )
			{
				const double at = prevLambda + ( l - prevLambda ) * prevMiss / ( prevMiss - miss );
				if( std::fabs( miss ) < bestMiss )
				{
					bestMiss = std::fabs( miss );
					best     = at;
					if( purity )
						*purity = std::hypot( x - whiteX, y - whiteY ) / std::hypot( lx - whiteX, ly - whiteY );
				}
			}
			prevMiss   = miss;
			prevLambda = l;
		}
		return best;
	}
};
const Display& display()
{
	static const Display d;
	return d;
}

/// The groove's efficiency for orders 0, 1 and 2, wavelength by wavelength.
struct Efficiency
{
	double groove = -1.0;
	std::vector< double > eta[ 3 ];
	void For( double depth )
	{
		if( depth == groove )
			return;
		groove = depth;
		for( int m = 0; m < 3; ++m )
		{
			eta[ m ].resize( kLambdas );
			for( int i = 0; i < kLambdas; ++i )
			{
				const double j = besselIntegral( m, 2.0 * kPi * depth / lambdaAt( i ) );
				eta[ m ][ i ]  = j * j;
			}
		}
	}
};

/// Everything about the card, the eye and the lamp that the light depends on,
/// read off the plugin's parameters through the controls' own mappings and
/// then worked out here.
struct Scene
{
	V3 eye;
	double eyeW = 0.0;
	V3 lamp;
	double lampW = 0.0;
	double sxx = 0.0, sxy = 0.0, syy = 0.0;//the lamp's covariance, card frame
	double rough = 0.0, brightness = 1.0, pitch = 1000.0, groove = 0.0;
	double angle = 0.0;
	double width = 1.0;//pixels a picture width
	int w = 0, h = 0;
};

Scene sceneOf( HolofoilPlugin& plugin, int w, int h )
{
	Scene sc;
	sc.w     = w;
	sc.h     = h;
	sc.width = w;
	auto p   = [ & ]( unsigned int id ) { return plugin.GetFloatParameter( id ); };
	//The test scenes hold still: the pose is the controls' own.
	const double tx = TiltFromParam( p( PT_TILT_X ) ), ty = TiltFromParam( p( PT_TILT_Y ) );
	const double lx = LampAngleFromParam( p( PT_LAMP_X ) ), ly = LampAngleFromParam( p( PT_LAMP_Y ) );
	const M3 r      = roomToCard( tx, ty );
	const double iv = InverseViewerFromParam( p( PT_VIEWER ) ), il = InverseLampFromParam( p( PT_LAMP_DISTANCE ) );
	sc.eye          = r( iv > 0.0 ? ( 1.0 / iv ) * V3 { 0.0, 0.0, 1.0 } : V3 { 0.0, 0.0, 1.0 } );
	sc.eyeW         = iv > 0.0 ? 1.0 : 0.0;
	const V3 d      = directionOf( lx, ly );
	sc.lamp         = r( il > 0.0 ? ( 1.0 / il ) * d : d );
	sc.lampW        = il > 0.0 ? 1.0 : 0.0;
	//The lamp's two extents: level (along a strip) and up, at right angles to it.
	const V3 level    = unit( cross( { 0.0, 1.0, 0.0 }, d ) );
	const V3 up       = unit( cross( d, level ) );
	const V3 a        = r( level ), b = r( up );
	const double size = LampSizeFromParam( p( PT_LAMP_SIZE ) );
	const double sa   = std::min( size * LampLengthFromParam( p( PT_LAMP_LENGTH ) ), kLongestStrip ), sb = size;
	sc.sxx            = sa * sa * a.x * a.x + sb * sb * b.x * b.x;
	sc.sxy            = sa * sa * a.x * a.y + sb * sb * b.x * b.y;
	sc.syy            = sa * sa * a.y * a.y + sb * sb * b.y * b.y;
	sc.rough          = RoughnessFromParam( p( PT_ROUGHNESS ) );
	sc.brightness     = LampBrightnessFromParam( p( PT_LAMP_BRIGHTNESS ) );
	sc.pitch          = PitchFromParam( p( PT_PITCH ) );
	sc.groove         = GrooveFromParam( p( PT_GROOVE ) );
	sc.angle          = GratingAngleFromParam( p( PT_ANGLE ) );
	return sc;
}

/// A pixel centre in the card's frame: picture widths from the centre.
void cardPoint( const Scene& sc, int x, int y, double& px, double& py )
{
	px = ( x + 0.5 - 0.5 * sc.w ) / sc.width;
	py = ( y + 0.5 - 0.5 * sc.h ) / sc.width;
}

/// s = ( v + l ).xy, in a surface leaning by `slope`; and h.z.
void halfway( const Scene& sc, double px, double py, double slopeX, double slopeY, double& sx, double& sy, double& hz )
{
	const V3 at = { px, py, 0.0 };
	const V3 v  = unit( sc.eye - sc.eyeW * at );
	const V3 l  = unit( sc.lamp - sc.lampW * at );
	const V3 hv = v + l;
	sx          = hv.x - hv.z * slopeX;
	sy          = hv.y - hv.z * slopeY;
	hz          = hv.z;
}

/// The grating at a point: up to two directions with weights, the surface's
/// lean and the pitch's factor.
struct Grating
{
	double g1x = 1.0, g1y = 0.0, w1 = 1.0;
	double g2x = 1.0, g2y = 0.0, w2 = 0.0;
	double slopeX = 0.0, slopeY = 0.0, pitch = 1.0;
};
Grating plainGrating( const Scene& sc )
{
	Grating g;
	g.g1x = std::cos( sc.angle );
	g.g1y = std::sin( sc.angle );
	return g;
}

/// What the foil at a pixel sends to the eye, in linear light: the lamp's
/// Gaussian of direction, evaluated at the direction each wavelength of each
/// order had to leave the lamp in, through the matching functions. No
/// table, no lines, no along and across.
Colour expectedSheen( const Scene& sc, Efficiency& eff, int x, int y, const Grating& gr )
{
	eff.For( sc.groove );
	double px, py, sx, sy, hz;
	cardPoint( sc, x, y, px, py );
	halfway( sc, px, py, gr.slopeX, gr.slopeY, sx, sy, hz );
	const double r2  = hz * sc.rough * hz * sc.rough;
	const double cxx = sc.sxx + r2, cxy = sc.sxy, cyy = sc.syy + r2;
	const double det = cxx * cyy - cxy * cxy;
	const double ixx = cyy / det, ixy = -cxy / det, iyy = cxx / det;
	auto lampAt      = [ & ]( double dx, double dy ) { return std::exp( -0.5 * ( ixx * dx * dx + 2.0 * ixy * dx * dy + iyy * dy * dy ) ); };
	const double gain = std::sqrt( ( sc.sxx * sc.syy - sc.sxy * sc.sxy ) / det );

	const double covered = std::min( gr.w1 + gr.w2, 1.0 );
	const double mirror  = lampAt( sx, sy );
	const double d       = sc.pitch * gr.pitch;
	double X = 0.0, Y = 0.0, Z = 0.0;
	const Display& dp = display();
	for( int i = 0; i < kLambdas; ++i )
	{
		const double l = lambdaAt( i );
		double radiance = mirror * ( 1.0 - covered + covered * eff.eta[ 0 ][ i ] );
		for( int n = 0; n < 2; ++n )
		{
			const double w = n == 0 ? gr.w1 : gr.w2;
			if( w <= 0.0 )
				continue;
			const double gx = n == 0 ? gr.g1x : gr.g2x, gy = n == 0 ? gr.g1y : gr.g2y;
			for( int m : { -2, -1, 1, 2 } )
			{
				//The grating equation: this wavelength reaches the eye in order
				//m from the lamp direction whose part in the surface is
				//m lambda / d along g, less v.
				const double t = m * l / d;
				radiance += w * eff.eta[ std::abs( m ) ][ i ] * lampAt( t * gx - sx, t * gy - sy );
			}
		}
		X += dp.cx[ i ] * radiance * kStep;
		Y += dp.cy[ i ] * radiance * kStep;
		Z += dp.cz[ i ] * radiance * kStep;
	}
	Colour c = dp.rgbOf( X, Y, Z );
	const double k = sc.brightness * gain;
	c.r *= k;
	c.g *= k;
	c.b *= k;
	const double lowest = std::min( { c.r, c.g, c.b, 0.0 } );
	c.r -= lowest;
	c.g -= lowest;
	c.b -= lowest;
	return c;
}

Colour linearAt( const Floats& out, int w, int x, int y )
{
	const float* o = pixelAt( out, w, x, y );
	return { linearOf( o[ 0 ] ), linearOf( o[ 1 ] ), linearOf( o[ 2 ] ) };
}

/// The measuring set-up: a black clip, a clear foil (Ink 0) over the whole
/// card, held still, so the output is the foil's own light.
void measureSetup( Rig& rig )
{
	rig.Option( PT_AREA, Area::WholeCard );
	rig.Option( PT_PATTERN, Pattern::Rainbow );
	rig.Option( PT_MOTION, Motion::Hold );
	rig.Set( PT_INK, 0.0f );
	rig.Set( PT_LAMP_BRIGHTNESS, ParamFromLampBrightness( 1.0 ) );
	rig.Set( PT_ROUGHNESS, 0.0f );
	rig.Set( PT_LAMP_LENGTH, ParamFromLampLength( 1.0 ) );
	rig.Set( PT_MIX, 1.0f );
}

/// A pose and a lamp, by the controls' own units (degrees, picture widths;
/// 0 distance is infinity).
struct Pose3
{
	double tiltX, tiltY, lampX, lampY, lampDistance, eyeDistance;
};
void setPose( Rig& rig, const Pose3& p )
{
	rig.Set( PT_TILT_X, ParamFromTilt( p.tiltX * kDegree ) );
	rig.Set( PT_TILT_Y, ParamFromTilt( p.tiltY * kDegree ) );
	rig.Set( PT_LAMP_X, ParamFromLampAngle( p.lampX * kDegree ) );
	rig.Set( PT_LAMP_Y, ParamFromLampAngle( p.lampY * kDegree ) );
	rig.Set( PT_LAMP_DISTANCE, p.lampDistance > 0.0 ? ParamFromLampDistance( p.lampDistance ) : 1.0f );
	rig.Set( PT_VIEWER, p.eyeDistance > 0.0 ? ParamFromViewer( p.eyeDistance ) : 1.0f );
}



/// One instance per raster for the whole of a check: the colour table is
/// built on the first frame and again only when the groove changes, and on
/// Apple's software renderer each build is seconds. Every check sets every
/// control it relies on before each render, so nothing carries over.
struct Rigs
{
	std::map< std::pair< int, int >, std::unique_ptr< Rig > > byRaster;
	Rig* At( int w, int h, const Floats* picture = nullptr )
	{
		std::unique_ptr< Rig >& r = byRaster[ { w, h } ];
		if( !r )
		{
			r = std::make_unique< Rig >();
			if( !r->Init( w, h, picture ) )
			{
				r.reset();
				return nullptr;
			}
		}
		else
			r->Upload( picture ? *picture : buildCard( w, h ) );
		return r.get();
	}
};

//===========================================================================
// --grating: the colour a pixel shows is the wavelength the grating equation
// sends to the eye, and all of its light is the spectrum's.
//===========================================================================
struct GratingCase
{
	const char* what;
	Pose3 pose;
	double angle, pitch, size, length, rough, groove;
};
const GratingCase kGratingCases[] = {
	{ "lamp overhead at infinity, lines across, 900 nm", { 0, 0, 0, 35, 0, 2.5 }, 90, 900, 0.2, 1, 0, 130 },
	{ "card turned (10, -6), lamp (-20, 30), lines at 60 degrees, 1500 nm", { 10, -6, -20, 30, 0, 2.0 }, 60, 1500, 0.2, 1, 0, 130 },
	{ "a near lamp (3 widths), card (-8, 5), lines at 120 degrees, 700 nm", { -8, 5, 15, 40, 3.0, 2.5 }, 120, 700, 0.2, 1, 0, 160 },
	{ "the second order alone: 2400 nm", { 0, 0, 0, 35, 0, 2.5 }, 90, 2400, 0.2, 1, 0, 200 },
	{ "the defaults' strip lamp (2 degrees by 8), rough 0.5 degrees", { 0, 4, 0, 35, 3.0, 2.5 }, 90, 900, 2.0, 8, 0.5, 130 },
};

void gratingSetup( Rig& rig, const GratingCase& c )
{
	measureSetup( rig );
	setPose( rig, c.pose );
	rig.Set( PT_ANGLE, ParamFromGratingAngle( c.angle * kDegree ) );
	rig.Set( PT_PITCH, ParamFromPitch( c.pitch ) );
	rig.Set( PT_LAMP_SIZE, ParamFromLampSize( c.size * kDegree ) );
	rig.Set( PT_LAMP_LENGTH, ParamFromLampLength( c.length ) );
	rig.Set( PT_ROUGHNESS, ParamFromRoughness( c.rough * kDegree ) );
	rig.Set( PT_GROOVE, ParamFromGroove( c.groove ) );
}

/// How far the rendered light may be from the spectrum's, as a fraction of
/// the picture's brightest channel: the table is read between columns ~2 nm
/// apart and rows whose widths step by 14%, bilinearly, and the line's
/// colour is not linear across either. Measured at 3e-3 here; the bound is
/// set at 1%, about a third of a code value at the brightest.
constexpr double kTableTolerance = 0.01;
/// Where a wavelength can be read off a colour: between 470 and 600 nm the
/// locus turns round white by at least 0.3 degree a nanometre. Toward either
/// end it is all but a straight line seen end on from white -- 6 degrees from
/// 400 to 460, 18 from 600 to 700 -- and the table's 1e-3 moves the reading
/// by tens of nanometres there; the light check above covers those pixels.
constexpr double kHueLow  = 470.0;
constexpr double kHueHigh = 600.0;
/// How far a line's dominant wavelength may be from its centre. A Gaussian
/// line sigma wide is pulled toward where the matching functions are larger,
/// by sigma^2 d ln( x + y + z ) / d lambda to first order, and that slope is
/// at most 0.044 a nanometre between 470 and 600; a quarter of a nanometre
/// more for the table's 2 nm columns and the reading's interpolation. The
/// white added to bring a line into gamut adds nothing to its hue.
constexpr double kHueSlope = 0.044;
constexpr double kHueFloor = 0.25;

int runGrating( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== grating: a pixel's hue is the wavelength d (v + l).g / m, and its light is the lamp's Gaussian, wavelength by "
	             "wavelength, through the groove's efficiency and the colour matching functions\n" );
	for( const Raster& raster : kRasters )
		for( const GratingCase& c : kGratingCases )
		{
			const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
			Rig* const made    = rigs.At( raster.w, raster.h, &black );
			if( !made )
				return 1;
			Rig& rig = *made;
			gratingSetup( rig, c );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const Scene sc   = sceneOf( rig.plugin, raster.w, raster.h );
			Efficiency eff;
			const Grating gr = plainGrating( sc );

			//Every pixel's light, on a grid.
			const int step = std::max( 1, std::min( raster.w, raster.h ) / 60 );
			double worst = 0.0, peak = 1e-6;
			int sampled  = 0;
			std::vector< std::pair< int, int > > lines;
			for( int y = step / 2; y < raster.h; y += step )
				for( int x = step / 2; x < raster.w; x += step )
				{
					const Colour want = expectedSheen( sc, eff, x, y, gr );
					const Colour got  = linearAt( out, raster.w, x, y );
					worst = std::max( { worst, std::fabs( got.r - want.r ), std::fabs( got.g - want.g ), std::fabs( got.b - want.b ) } );
					peak  = std::max( { peak, want.r, want.g, want.b } );
					++sampled;
				}
			Check( worst <= kTableTolerance * peak,
			       fmt( "%dx%d %s: %d pixels' light against the spectrum's, worst %.2e of the brightest %.3f (bound %.0e)", raster.w,
			            raster.h, c.what, sampled, worst / peak, peak, kTableTolerance ) );

			//The hue, wherever one line alone is lit and clear of the mirror.
			if( c.size > 0.5 )
				continue;
			const double sigma = c.size * kDegree;//round lamp, Length 1: the up extent is foreshortened, so this is the widest
			int hues = 0, wrong = 0;
			double worstHue = 0.0, lo = 1e9, hi = -1e9;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double px, py, sx, sy, hz;
					cardPoint( sc, x, y, px, py );
					halfway( sc, px, py, 0.0, 0.0, sx, sy, hz );
					const double along = sx * gr.g1x + sy * gr.g1y, across = -sx * gr.g1y + sy * gr.g1x;
					if( std::fabs( across ) > 0.5 * sigma || along * along + across * across < 100.0 * sigma * sigma )
						continue;
					//One order in the visible, well inside it.
					int order = 0, visible = 0;
					for( int m = 1; m <= 2; ++m )
					{
						const double centre = sc.pitch * std::fabs( along ) / m;
						if( centre > 340.0 && centre < 860.0 )
						{
							++visible;
							order = m;
						}
					}
					const double expect = sc.pitch * std::fabs( along ) / std::max( order, 1 );
					if( visible != 1 || expect < kHueLow || expect > kHueHigh )
						continue;
					const double lineWidth = sc.pitch * sigma / order;
					const double bound     = kHueFloor + kHueSlope * lineWidth * lineWidth;
					const Colour got       = linearAt( out, raster.w, x, y );
					if( got.r + got.g + got.b < 1e-3 )
					{
						++wrong;
						++hues;
						continue;
					}
					const double hue = display().dominant( got );
					worstHue         = std::max( worstHue, std::fabs( hue - expect ) / bound );
					wrong += std::fabs( hue - expect ) > bound;
					lo = std::min( lo, expect );
					hi = std::max( hi, expect );
					++hues;
				}
			if( lo > hi )
				lo = hi = 0.0;
			Check( hues > 10 && wrong == 0,
			       fmt( "%dx%d %s: %d pixels on the line from %.0f to %.0f nm, dominant wavelength against d |s.g| / m: worst %.2f of "
			            "the bound ( %.2f + %.3f x the line's width^2, %.2f nm at %.1f nm wide ), %d wrong",
			            raster.w, raster.h, c.what, hues, lo, hi, worstHue, kHueFloor, kHueSlope,
			            kHueFloor + kHueSlope * std::pow( sc.pitch * sigma, 2.0 ), sc.pitch * sigma, wrong ) );
		}
	return Verdict();
}

//===========================================================================
// --patterns: what is embossed where, and the optics follow it.
//===========================================================================
struct Readout
{
	Floats grating, weights;
};
Readout readPattern( Rig& rig, int hooks )
{
	Readout r;
	rig.plugin.SetHooksForTest( hooks | shaders::kHookShowGrating );
	rig.Render( 1 );
	r.grating = rig.Output();
	rig.plugin.SetHooksForTest( hooks | shaders::kHookShowWeights );
	rig.Render( 1 );
	r.weights = rig.Output();
	rig.plugin.SetHooksForTest( hooks );
	return r;
}

int runPatterns( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== patterns: each pattern embosses what it says -- one grating, two families of lines, discs, shards, glitter -- "
	             "and every pixel's light is the grating equation's for the grating embossed there\n" );
	const Pattern patterns[] = { Pattern::Rainbow, Pattern::Crosshatch, Pattern::Cosmos, Pattern::CrackedIce, Pattern::Sparkle };
	for( const Raster& raster : kRasters )
		for( Pattern pattern : patterns )
		{
			const char* name = InfoOf( PT_PATTERN ).options[ static_cast< int >( pattern ) ];
			const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
			Rig* const made    = rigs.At( raster.w, raster.h, &black );
			if( !made )
				return 1;
			Rig& rig = *made;
			measureSetup( rig );
			rig.Option( PT_PATTERN, pattern );
			setPose( rig, { 4, 3, -10, 30, 3.0, 2.5 } );
			rig.Set( PT_LAMP_SIZE, ParamFromLampSize( 3.0 * kDegree ) );
			rig.Set( PT_LAMP_LENGTH, ParamFromLampLength( 6.0 ) );
			rig.Set( PT_ANGLE, ParamFromGratingAngle( 75.0 * kDegree ) );
			rig.Set( PT_FACET, ParamFromFacetTilt( 8.0 * kDegree ) );
			applyPerturb( rig, perturb );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats out = rig.Output();
			const Readout ro = readPattern( rig, perturb.hooks );
			const Scene sc   = sceneOf( rig.plugin, raster.w, raster.h );
			const double angle = sc.angle, facet = std::tan( FacetTiltFromParam( rig.Get( PT_FACET ) ) );
			const double ax = std::cos( angle + 45.0 * kDegree ), ay = std::sin( angle + 45.0 * kDegree );
			const double bx = std::cos( angle - 45.0 * kDegree ), by = std::sin( angle - 45.0 * kDegree );

			//What is embossed.
			int n = 0, plain = 0, onA = 0, onB = 0, both = 0, bad = 0;
			double c2 = 0.0, s2 = 0.0;
			int cells = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const float* g = pixelAt( ro.grating, raster.w, x, y );
					const float* w = pixelAt( ro.weights, raster.w, x, y );
					++n;
					const double len   = std::hypot( g[ 0 ], g[ 1 ] );
					const double slope = std::hypot( g[ 2 ], g[ 3 ] );
					const bool isPlain = std::fabs( g[ 0 ] - std::cos( angle ) ) < 1e-5 && std::fabs( g[ 1 ] - std::sin( angle ) ) < 1e-5
					                     && slope == 0.0 && w[ 2 ] == 1.0f;
					plain += isPlain;
					switch( pattern )
					{
					case Pattern::Rainbow: bad += !( isPlain && w[ 0 ] == 1.0f && w[ 1 ] == 0.0f ); break;
					case Pattern::Crosshatch:
						bad += !( std::fabs( g[ 0 ] - ax ) < 1e-5 && std::fabs( g[ 1 ] - ay ) < 1e-5 && slope == 0.0 && w[ 2 ] == 1.0f );
						onA += w[ 0 ] > 0.0f;
						onB += w[ 1 ] > 0.0f;
						both += w[ 0 ] > 0.0f && w[ 1 ] > 0.0f;
						bad += !( ( w[ 0 ] == 0.0f || w[ 0 ] == 1.0f || w[ 0 ] == 0.5f ) && ( w[ 0 ] + w[ 1 ] <= 1.0f ) );
						break;
					case Pattern::Cosmos:
					case Pattern::CrackedIce:
					case Pattern::Sparkle:
						bad += !( std::fabs( len - 1.0 ) < 1e-5 && slope <= facet * ( 1.0 + 1e-5 ) && w[ 0 ] == 1.0f && w[ 1 ] == 0.0f
						          && w[ 2 ] >= 0.8f - 1e-5f && w[ 2 ] <= 1.25f + 1e-5f );
						if( !isPlain )
						{
							//A grating's direction has no sign: double its angle.
							const double t = 2.0 * std::atan2( g[ 1 ], g[ 0 ] );
							c2 += std::cos( t );
							s2 += std::sin( t );
							++cells;
						}
						break;
					case Pattern::Count: break;
					}
				}
			std::string stats;
			bool ok = bad == 0;
			if( pattern == Pattern::Crosshatch )
			{
				//Half of each family's period is line: a quarter of the card is on
				//both, a quarter on neither.
				const double fa = double( onA ) / n, fb = double( onB ) / n, fab = double( both ) / n;
				ok              = ok && std::fabs( fa - 0.5 ) < 0.03 && std::fabs( fb - 0.5 ) < 0.03 && std::fabs( fab - 0.25 ) < 0.03;
				stats           = fmt( "on the first family %.3f, the second %.3f, both %.3f (0.5, 0.5, 0.25 +- 0.03)", fa, fb, fab );
			}
			else if( pattern == Pattern::Cosmos )
			{
				const double fd = 1.0 - double( plain ) / n;
				ok              = ok && fd > 0.05 && fd < 0.6;
				stats           = fmt( "discs cover %.3f of the card", fd );
			}
			else if( pattern != Pattern::Rainbow )
			{
				//Directions uniform over half a turn: the mean of the doubled
				//angle's unit vector is near 0 (pixel-weighted, so loose).
				const double bias = std::hypot( c2, s2 ) / std::max( cells, 1 );
				ok                = ok && bias < ( pattern == Pattern::Sparkle ? 0.05 : 0.25 ) && plain < n / 100;
				stats             = fmt( "directions' mean resultant %.3f", bias );
			}
			Check( ok, fmt( "%dx%d %-11s embossed as it says: %d pixels out of shape; %s", raster.w, raster.h, name, bad, stats.c_str() ) );

			//And the light follows it: the spectrum's light for the grating
			//read out at the pixel.
			Efficiency eff;
			double worst = 0.0, peak = 1e-6;
			int sampled  = 0;
			const int step = std::max( 1, std::min( raster.w, raster.h ) / 90 );
			for( int y = step / 3; y < raster.h; y += step )
				for( int x = step / 3; x < raster.w; x += step )
				{
					const float* g = pixelAt( ro.grating, raster.w, x, y );
					const float* w = pixelAt( ro.weights, raster.w, x, y );
					Grating gr;
					gr.g1x    = g[ 0 ];
					gr.g1y    = g[ 1 ];
					gr.w1     = w[ 0 ];
					gr.g2x    = pattern == Pattern::Crosshatch ? bx : 1.0;
					gr.g2y    = pattern == Pattern::Crosshatch ? by : 0.0;
					gr.w2     = w[ 1 ];
					gr.slopeX = g[ 2 ];
					gr.slopeY = g[ 3 ];
					gr.pitch  = w[ 2 ];
					const Colour want = expectedSheen( sc, eff, x, y, gr );
					const Colour got  = linearAt( out, raster.w, x, y );
					worst = std::max( { worst, std::fabs( got.r - want.r ), std::fabs( got.g - want.g ), std::fabs( got.b - want.b ) } );
					peak  = std::max( { peak, want.r, want.g, want.b } );
					++sampled;
				}
			Check( worst <= kTableTolerance * peak,
			       fmt( "%dx%d %-11s %d pixels' light against the spectrum's for the grating embossed there: worst %.2e of the "
			            "brightest %.3f (bound %.0e)",
			            raster.w, raster.h, name, sampled, worst / peak, peak, kTableTolerance ) );
		}
	return Verdict();
}

//===========================================================================
// --mirror: with no groove the foil is a mirror, and the lamp's highlight
// sits where the mirror puts it.
//===========================================================================
/// The mirror point, in the card's frame: where the eye sees the lamp. With
/// E and L homogeneous, P = ( Lz E.xy + Ez L.xy ) / ( Ez Lw + Lz Ew ).
void mirrorPoint( const Scene& sc, double& x, double& y )
{
	const double den = sc.eye.z * sc.lampW + sc.lamp.z * sc.eyeW;
	x                = ( sc.lamp.z * sc.eye.x + sc.eye.z * sc.lamp.x ) / den;
	y                = ( sc.lamp.z * sc.eye.y + sc.eye.z * sc.lamp.y ) / den;
}

/// The peak of an image's luminance to a fraction of a pixel: a quadratic
/// in two variables fitted to log I over the 5 x 5 pixels round the
/// brightest, by least squares.
bool peakOf( const Floats& out, int w, int h, double& px, double& py )
{
	int bx = -1, by = -1;
	double best = 0.0;
	auto lum    = [ & ]( int x, int y ) {
        const Colour c = linearAt( out, w, x, y );
        return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
	};
	for( int y = 2; y < h - 2; ++y )
		for( int x = 2; x < w - 2; ++x )
			if( lum( x, y ) > best )
			{
				best = lum( x, y );
				bx   = x;
				by   = y;
			}
	if( bx < 0 || best <= 0.0 )
		return false;
	//log I = a + b u + c v + d u^2 + e u v + f v^2.
	double A[ 6 ][ 7 ] = {};
	for( int j = -2; j <= 2; ++j )
		for( int i = -2; i <= 2; ++i )
		{
			const double v = lum( bx + i, by + j );
			if( v <= 0.0 )
				return false;
			const double f[ 6 ] = { 1.0, double( i ), double( j ), double( i * i ), double( i * j ), double( j * j ) };
			for( int r = 0; r < 6; ++r )
			{
				for( int c = 0; c < 6; ++c )
					A[ r ][ c ] += f[ r ] * f[ c ];
				A[ r ][ 6 ] += f[ r ] * std::log( v );
			}
		}
	for( int c = 0; c < 6; ++c )
	{
		int pivot = c;
		for( int r = c + 1; r < 6; ++r )
			if( std::fabs( A[ r ][ c ] ) > std::fabs( A[ pivot ][ c ] ) )
				pivot = r;
		for( int k = 0; k < 7; ++k )
			std::swap( A[ c ][ k ], A[ pivot ][ k ] );
		for( int r = 0; r < 6; ++r )
			if( r != c )
			{
				const double f = A[ r ][ c ] / A[ c ][ c ];
				for( int k = 0; k < 7; ++k )
					A[ r ][ k ] -= f * A[ c ][ k ];
			}
	}
	double q[ 6 ];
	for( int r = 0; r < 6; ++r )
		q[ r ] = A[ r ][ 6 ] / A[ r ][ r ];
	//The gradient's zero: [ 2d e; e 2f ] [ u v ] = -[ b c ].
	const double det = 4.0 * q[ 3 ] * q[ 5 ] - q[ 4 ] * q[ 4 ];
	const double u   = ( -q[ 1 ] * 2.0 * q[ 5 ] + q[ 2 ] * q[ 4 ] ) / det;
	const double v   = ( -q[ 2 ] * 2.0 * q[ 3 ] + q[ 1 ] * q[ 4 ] ) / det;
	px               = bx + u;
	py               = by + v;
	return std::fabs( u ) < 1.0 && std::fabs( v ) < 1.0;
}

/// How far the fitted peak may be from the mirror point: log I is quadratic
/// near the peak only to first order in the pixel's offset (v and l turn as
/// the point moves), which pulls a 5 x 5 fit by under 0.002 pixel here.
constexpr double kPeakTolerance = 0.01;

int runMirror( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== mirror: with no groove the foil is a mirror, and the lamp shows where the eye sees it reflected -- "
	             "( Lz E + Ez L ) / ( Ez + Lz ) -- for near and far eyes and lamps, the card turned\n" );
	//The frame first: the eye is where Card.h says.
	{
		double worst = 0.0;
		for( int i = 0; i < 50; ++i )
		{
			const double tx = ( hash01( i, 1 ) - 0.5 ) * 80.0 * kDegree, ty = ( hash01( i, 2 ) - 0.5 ) * 80.0 * kDegree;
			const double lx = ( hash01( i, 3 ) - 0.5 ) * 160.0 * kDegree, ly = ( hash01( i, 4 ) - 0.5 ) * 160.0 * kDegree;
			const V3 e      = roomToCard( tx, ty )( { 0.0, 0.0, 1.0 } );
			const V3 said   = { std::cos( ty ) * std::sin( tx ), std::sin( ty ), std::cos( ty ) * std::cos( tx ) };
			const Place pe  = EyeInCard( tx, ty, 0.0 );
			const V3 l      = roomToCard( tx, ty )( directionOf( lx, ly ) );
			const Place pl  = LampInCard( tx, ty, lx, ly, 0.0 );
			worst = std::max( { worst, std::fabs( e.x - said.x ), std::fabs( e.y - said.y ), std::fabs( e.z - said.z ), std::fabs( pe.x - e.x ),
			                    std::fabs( pe.y - e.y ), std::fabs( pe.z - e.z ), std::fabs( pl.x - l.x ), std::fabs( pl.y - l.y ),
			                    std::fabs( pl.z - l.z ) } );
		}
		Check( worst < 1e-12, fmt( "the frame: 50 poses, the eye at ( cos Ty sin Tx, sin Ty, cos Ty cos Tx ) and the eye and lamp as "
		                           "rotation matrices put them, worst %.1e",
		                           worst ) );
	}
	const Pose3 poses[] = {
		{ 0, 0, 0, 5, 4.0, 2.5 },
		{ 6, -3, -4, 4, 0.0, 3.0 },
		{ -5, 4, 8, -2, 2.0, 0.0 },
		{ 12, -8, 10, 2, 1.5, 1.8 },
	};
	for( const Raster& raster : kRasters )
		for( const Pose3& pose : poses )
		{
			const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
			Rig* const made    = rigs.At( raster.w, raster.h, &black );
			if( !made )
				return 1;
			Rig& rig = *made;
			measureSetup( rig );
			setPose( rig, pose );
			rig.Set( PT_GROOVE, 0.0f );
			rig.Set( PT_LAMP_SIZE, ParamFromLampSize( 1.0 * kDegree ) );
			applyPerturb( rig, perturb );
			if( !rig.Render( 1 ) )
				return 1;
			const Floats out = rig.Output();
			const Scene sc   = sceneOf( rig.plugin, raster.w, raster.h );
			double mx, my;
			mirrorPoint( sc, mx, my );
			const double wantX = mx * sc.width + 0.5 * raster.w - 0.5, wantY = my * sc.width + 0.5 * raster.h - 0.5;
			double gotX = -1.0, gotY = -1.0;
			const bool found = peakOf( out, raster.w, raster.h, gotX, gotY );
			const double miss = std::hypot( gotX - wantX, gotY - wantY );
			Check( found && miss <= kPeakTolerance,
			       fmt( "%dx%d card (%g, %g), lamp (%g, %g) at %s, eye at %s: the highlight at (%.3f, %.3f), the mirror point at "
			            "(%.3f, %.3f) -- %.3f px apart (bound %.2f)",
			            raster.w, raster.h, pose.tiltX, pose.tiltY, pose.lampX, pose.lampY,
			            pose.lampDistance > 0 ? fmt( "%g widths", pose.lampDistance ).c_str() : "infinity",
			            pose.eyeDistance > 0 ? fmt( "%g widths", pose.eyeDistance ).c_str() : "infinity", gotX, gotY, wantX, wantY, miss,
			            kPeakTolerance ) );
		}
	return Verdict();
}

//===========================================================================
// --groove: the mirror keeps J0^2 of the light, wavelength by wavelength.
//===========================================================================
int runGroove( const Perturb& perturb )
{
	std::printf( "\n=== groove: a groove h deep leaves the mirror J0( 2 pi h / lambda )^2 of each wavelength -- white at 0, "
	             "magenta near 210 nm, where green's is gone\n" );
	const Raster raster = kRasters.back();
	const double grooves[] = { 0.0, 60.0, 130.0, 210.0, 300.0, 400.0 };
	Efficiency eff;
	for( double groove : grooves )
	{
		Rig rig;
		const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
		if( !rig.Init( raster.w, raster.h, &black ) )
			return 1;
		measureSetup( rig );
		//The eye and the lamp both along the card's normal, far off: every
		//pixel is the mirror, and the first order is centred at 0 nm.
		setPose( rig, { 0, 0, 0, 0, 0.0, 0.0 } );
		rig.Set( PT_LAMP_SIZE, ParamFromLampSize( 1.0 * kDegree ) );
		rig.Set( PT_GROOVE, ParamFromGroove( groove ) );
		applyPerturb( rig, perturb );
		if( !rig.Render( 1 ) )
			return 1;
		const Floats out  = rig.Output();
		const Scene sc    = sceneOf( rig.plugin, raster.w, raster.h );
		const Colour want = expectedSheen( sc, eff, raster.w / 2, raster.h / 2, plainGrating( sc ) );
		double worst      = 0.0;
		for( int y = 0; y < raster.h; y += 7 )
			for( int x = 0; x < raster.w; x += 7 )
			{
				const Colour got = linearAt( out, raster.w, x, y );
				worst = std::max( { worst, std::fabs( got.r - want.r ), std::fabs( got.g - want.g ), std::fabs( got.b - want.b ) } );
			}
		const Colour mid = linearAt( out, raster.w, raster.w / 2, raster.h / 2 );
		bool shape       = true;
		if( groove == 0.0 )
			shape = std::fabs( mid.r - 1.0 ) < 1e-3 && std::fabs( mid.g - 1.0 ) < 1e-3 && std::fabs( mid.b - 1.0 ) < 1e-3;
		if( groove == 210.0 )
			shape = mid.g < 0.5 * mid.r && mid.g < 0.5 * mid.b;
		//The table's mirror row integrates the same smooth functions with 256
		//midpoint taps; this file uses 1880. They agree to 1e-5.
		Check( worst < 2e-4 && shape,
		       fmt( "Groove Depth %3.0f nm: the mirror ( %.4f %.4f %.4f ), J0^2 through the matching functions ( %.4f %.4f %.4f ), "
		            "worst %.1e over the card (bound 2e-4)%s",
		            groove, mid.r, mid.g, mid.b, want.r, want.g, want.b, worst,
		            groove == 0.0 ? ", white" : groove == 210.0 ? ", green below half red and blue" : "" ) );
	}
	return Verdict();
}

//===========================================================================
// --lamp: a bigger lamp washes the colour out; roughness is a bigger lamp.
//===========================================================================
int runLamp( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== lamp: the colour's purity falls as the lamp grows, and a rough foil under a small lamp is a smooth one under a "
	             "lamp sqrt( size^2 + ( h.z roughness )^2 ) wide, dimmer by the ratio of their areas\n" );
	const Raster raster = kRasters.back();
	//The lamp along the card's normal and the eye 35 degrees below it, both
	//far off: every pixel the same, the lamp round as the card sees it, and
	//the first order at 517 nm.
	const Pose3 pose = { 0, -35, 0, 35, 0.0, 0.0 };
	auto render      = [ & ]( double size, double rough, Floats& out, Scene& sc ) {
        const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
        Rig* const made    = rigs.At( raster.w, raster.h, &black );
        if( !made )
            return false;
        Rig& rig = *made;
        measureSetup( rig );
        setPose( rig, pose );
        rig.Set( PT_LAMP_SIZE, ParamFromLampSize( size ) );
        rig.Set( PT_ROUGHNESS, ParamFromRoughness( rough ) );
        applyPerturb( rig, perturb );
        if( !rig.Render( 1 ) )
            return false;
        out = rig.Output();
        sc  = sceneOf( rig.plugin, raster.w, raster.h );
        return true;
	};
	{
		std::string list;
		double last = 2.0;
		bool falls  = true;
		//From a degree up: below it the line is narrower than sRGB can show
		//and the white added to bring it into gamut sets the purity.
		for( double size : { 1.0, 2.0, 4.0, 8.0, 16.0 } )
		{
			Floats out;
			Scene sc;
			if( !render( size * kDegree, 0.0, out, sc ) )
				return 1;
			double purity    = 0.0;
			const double hue = display().dominant( linearAt( out, raster.w, raster.w / 2, raster.h / 2 ), &purity );
			falls            = falls && purity < last;
			last             = purity;
			list += fmt( " %g:%.4f(%.0fnm)", size, purity, hue );
		}
		Check( falls, fmt( "purity at each Lamp Size (degrees: purity):%s -- falling", list.c_str() ) );
	}
	for( double rough : { 1.0, 3.0, 6.0 } )
	{
		Floats roughOut, wideOut;
		Scene sc, wideScene;
		const double small = 1.0 * kDegree;
		if( !render( small, rough * kDegree, roughOut, sc ) )
			return 1;
		double px, py, sx, sy, hz;
		cardPoint( sc, 0, 0, px, py );
		halfway( sc, px, py, 0.0, 0.0, sx, sy, hz );
		const double size  = LampSizeFromParam( ParamFromLampSize( small ) );
		const double spread = hz * sc.rough;
		const double wide   = std::sqrt( size * size + spread * spread );
		if( !render( wide, 0.0, wideOut, wideScene ) )
			return 1;
		const double wideAsSet = LampSizeFromParam( ParamFromLampSize( wide ) );
		const double ratio     = size * size / ( wideAsSet * wideAsSet );
		double worst           = 0.0, peak = 1e-9;
		for( int y = 0; y < raster.h; y += 5 )
			for( int x = 0; x < raster.w; x += 5 )
			{
				const Colour a = linearAt( roughOut, raster.w, x, y ), b = linearAt( wideOut, raster.w, x, y );
				worst = std::max( { worst, std::fabs( a.r - ratio * b.r ), std::fabs( a.g - ratio * b.g ), std::fabs( a.b - ratio * b.b ) } );
				peak  = std::max( { peak, a.r, a.g, a.b } );
			}
		//And the rough foil's own light against the spectrum's, wavelength by
		//wavelength: the comparison above is a ratio, and a factor common to
		//both renders -- a lamp's area wrong whatever the roughness -- would
		//pass it.
		Efficiency eff;
		const Grating plain = plainGrating( sc );
		double absolute = 0.0, brightest = 1e-9;
		for( int y = 2; y < raster.h; y += 15 )
			for( int x = 2; x < raster.w; x += 15 )
			{
				const Colour got = linearAt( roughOut, raster.w, x, y ), want = expectedSheen( sc, eff, x, y, plain );
				absolute  = std::max( { absolute, std::fabs( got.r - want.r ), std::fabs( got.g - want.g ), std::fabs( got.b - want.b ) } );
				brightest = std::max( { brightest, want.r, want.g, want.b } );
			}
		//The two differ by the width the table reads at -- the same line, a
		//float's rounding apart -- and by the float the Lamp Size slider
		//holds: measured under 1e-6 of the light.
		Check( worst <= 1e-5 * peak && absolute <= 1e-2 * brightest,
		       fmt( "Roughness %g degrees under a 1 degree lamp (h.z %.3f) against a %.3f degree lamp, x %.4f: "
		            "worst %.1e of %.3f (bound 1e-5); against the spectrum's light %.1e of %.3f (bound 1e-2)",
		            rough, hz, wideAsSet / kDegree, ratio, worst / peak, peak, absolute / brightest, brightest ) );
	}
	return Verdict();
}

//===========================================================================
// --complement: a reverse holo is exactly a holo's complement.
//===========================================================================
int runComplement( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== complement: Around Art is Art Window's complement and Dark Ink is Bright Ink's -- in linear light the two add to "
	             "the clip plus the whole card's foil, every pixel; where one is the clip bit for bit the other is the whole card's\n" );
	for( const Raster& raster : kRasters )
		for( int pair = 0; pair < 2; ++pair )
			for( double softness : { 0.0, 0.4 } )
			{
				const Area a = pair == 0 ? Area::ArtWindow : Area::BrightInk;
				const Area b = pair == 0 ? Area::AroundArt : Area::DarkInk;
				Floats outs[ 4 ];
				const Area areas[] = { a, b, Area::WholeCard, Area::WholeCard };
				for( int k = 0; k < 4; ++k )
				{
					Rig* const made = rigs.At( raster.w, raster.h );
					if( !made )
						return 1;
					Rig& rig = *made;
					rig.Option( PT_MOTION, Motion::Hold );
					setPose( rig, { 5, 3, -10, 35, 3.0, 2.5 } );
					rig.Option( PT_AREA, areas[ k ] );
					rig.Option( PT_PATTERN, Pattern::CrackedIce );
					rig.Set( PT_SOFTNESS, static_cast< float >( softness ) );
					rig.Set( PT_CORNER, 0.6f );
					//Window edges between pixels.
					rig.Set( PT_WINDOW_X, 0.4987f );
					rig.Set( PT_WINDOW_WIDTH, 0.7213f );
					rig.Set( PT_MIX, k == 3 ? 0.0f : 1.0f );
					applyPerturb( rig, perturb );
					if( !rig.Render( 1 ) )
						return 1;
					outs[ k ] = rig.Output();
				}
				const Floats &holo = outs[ 0 ], &rev = outs[ 1 ], &whole = outs[ 2 ], &src = outs[ 3 ];
				double worst = 0.0;
				int fractional = 0, bitsWrong = 0;
				double apart   = 0.0;
				for( int y = 0; y < raster.h; ++y )
					for( int x = 0; x < raster.w; ++x )
					{
						const float *ph = pixelAt( holo, raster.w, x, y ), *pr = pixelAt( rev, raster.w, x, y );
						const float *pw = pixelAt( whole, raster.w, x, y ), *ps = pixelAt( src, raster.w, x, y );
						for( int c = 0; c < 3; ++c )
						{
							const double sum  = linearOf( ph[ c ] ) + linearOf( pr[ c ] );
							const double want = linearOf( ps[ c ] ) + linearOf( pw[ c ] );
							worst = std::max( worst, std::fabs( sum - want ) / std::max( 1.0, want ) );
						}
						//Which side is the clip is read off the deterministic
						//path -- the clip passed through, bit for bit on every
						//renderer -- and the other side must then be the whole
						//card's, as two draws agree (drawsAgreeWithin).
						const bool holoIsSrc = std::memcmp( ph, ps, 16 ) == 0, revIsSrc = std::memcmp( pr, ps, 16 ) == 0;
						if( holoIsSrc != revIsSrc )
						{
							const double other = relativeApart( holoIsSrc ? pr : ph, pw );
							bitsWrong += other > drawsAgreeWithin();
							if( other <= drawsAgreeWithin() )
								apart = std::max( apart, other );
						}
						else if( !holoIsSrc )
							++fractional;
						else
							bitsWrong += std::memcmp( ps, pw, 16 ) != 0;//both the clip, where the foil is not
						bitsWrong += ph[ 3 ] != ps[ 3 ] || pr[ 3 ] != ps[ 3 ];
					}
				//Both sides are mix( clip, foil, m ) in float, through the
				//sRGB curve and back: a few float roundings of a value <= 1.
				Check( worst < 2e-5 && bitsWrong == 0 && ( softness == 0.0 || fractional > 100 ),
				       fmt( "%dx%d %s + %s, Softness %.1f: worst %.1e (bound 2e-5); %d pixels part covered; %d where one is the clip "
				            "and the other is not the whole card's (%s), or both are the clip, or alpha moved",
				            raster.w, raster.h, InfoOf( PT_AREA ).options[ static_cast< int >( a ) ],
				            InfoOf( PT_AREA ).options[ static_cast< int >( b ) ], softness, worst, fractional, bitsWrong,
				            agreement( apart ).c_str() ) );
			}
	return Verdict();
}

//===========================================================================
// --window: the art window covers each pixel by the share of it inside.
//===========================================================================
/// The share of the pixel [x, x + 1] x [y, y + 1] inside the rounded
/// rectangle, column by column: 2048 strips, the rectangle's height at each
/// from its corner circle.
double exactCoverage( int x, int y, double cx, double cy, double hw, double hh, double r )
{
	constexpr int kStrips = 2048;
	double sum            = 0.0;
	for( int i = 0; i < kStrips; ++i )
	{
		const double u  = x + ( i + 0.5 ) / kStrips;
		const double du = std::fabs( u - cx );
		if( du > hw )
			continue;
		double half = hh;
		if( du > hw - r )
		{
			const double k = du - ( hw - r );
			half           = hh - r + std::sqrt( std::max( 0.0, r * r - k * k ) );
		}
		sum += std::max( 0.0, std::min( y + 1.0, cy + half ) - std::max( double( y ), cy - half ) );
	}
	return sum / kStrips;
}

/// A one-pixel ramp across the edge is the exact share for an edge along the
/// pixels; for a straight one at 45 degrees it is off by a corner triangle,
/// 0.043, at most. Round a corner the edge takes every angle between, and it
/// bends away from its tangent across the pixel by up to w^2 / 8r over a
/// chord w <= sqrt 2, which is 2/3 w s = sqrt( 2 )^3 / 12 r of area.
double curvedTolerance( double r )
{
	return 0.044 + std::pow( std::sqrt( 2.0 ), 3.0 ) / ( 12.0 * r );
}
/// Along a straight edge, what the window's place costs as floats: a
/// thousandth of a pixel at 4K is still 1e-4 of the share.
constexpr double kStraightTolerance = 1e-4;

int runWindow( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== window: the art window covers each pixel by the share of it inside the rounded rectangle -- exactly along its "
	             "straight edges, and to 0.044 + 0.24 / radius round its corners -- and whole pixels in and out are the whole card's "
	             "and the clip's bit for bit\n" );
	struct Box
	{
		float cx, cy, w, h, corner;
	};
	const Box boxes[] = { { 0.5f, 0.64f, 0.84f, 0.42f, 0.04f }, { 0.4371f, 0.5523f, 0.6131f, 0.4447f, 0.8f }, { 0.61f, 0.47f, 0.3f, 0.7f, 1.0f } };
	for( const Raster& raster : kRasters )
		for( const Box& box : boxes )
		{
			Floats outs[ 2 ];
			for( int k = 0; k < 2; ++k )
			{
				const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
				Rig* const made    = rigs.At( raster.w, raster.h, &black );
				if( !made )
					return 1;
				Rig& rig = *made;
				measureSetup( rig );
				//Eye and lamp far off and a big lamp: the same sheen everywhere.
				setPose( rig, { 0, 0, 0, 20, 0.0, 0.0 } );
				rig.Set( PT_LAMP_SIZE, ParamFromLampSize( 20.0 * kDegree ) );
				rig.Option( PT_AREA, k == 0 ? Area::ArtWindow : Area::WholeCard );
				rig.Set( PT_WINDOW_X, box.cx );
				rig.Set( PT_WINDOW_Y, box.cy );
				rig.Set( PT_WINDOW_WIDTH, box.w );
				rig.Set( PT_WINDOW_HEIGHT, box.h );
				rig.Set( PT_CORNER, box.corner );
				applyPerturb( rig, perturb );
				if( !rig.Render( 1 ) )
					return 1;
				outs[ k ] = rig.Output();
			}
			const double cx = double( box.cx ) * raster.w, cy = double( box.cy ) * raster.h;
			const double hw = 0.5 * box.w * raster.w, hh = 0.5 * box.h * raster.h;
			const double r  = std::min( { CornerFromParam( box.corner ) * raster.h, hw, hh } );
			double total = 0.0, straightWorst = 0.0, curvedWorst = 0.0;
			int inside = 0, outside = 0, wrongInside = 0, wrongOutside = 0, curved = 0;
			double apart = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const Colour a = linearAt( outs[ 0 ], raster.w, x, y ), s = linearAt( outs[ 1 ], raster.w, x, y );
					const double m = a.g / s.g;
					total += m;
					const bool cornerZone = std::fabs( x + 0.5 - cx ) > hw - r - 1.0 && std::fabs( y + 0.5 - cy ) > hh - r - 1.0;
					double share;
					if( cornerZone )
					{
						share = exactCoverage( x, y, cx, cy, hw, hh, r );
						if( share > 0.0 && share < 1.0 )
						{
							curvedWorst = std::max( curvedWorst, std::fabs( m - share ) );
							++curved;
						}
					}
					else
					{
						const double ox = std::max( 0.0, std::min( x + 1.0, cx + hw ) - std::max( double( x ), cx - hw ) );
						const double oy = std::max( 0.0, std::min( y + 1.0, cy + hh ) - std::max( double( y ), cy - hh ) );
						share           = ox * oy;
						straightWorst   = std::max( straightWorst, std::fabs( m - share ) );
					}
					if( share == 1.0 && !cornerZone )
					{
						++inside;
						const double steps = relativeApart( pixelAt( outs[ 0 ], raster.w, x, y ), pixelAt( outs[ 1 ], raster.w, x, y ) );
						wrongInside += steps > drawsAgreeWithin();
						if( steps <= drawsAgreeWithin() )
							apart = std::max( apart, steps );
					}
					if( share == 0.0 )
					{
						++outside;
						wrongOutside += a.r != 0.0 || a.g != 0.0 || a.b != 0.0;
					}
				}
			const double area = 4.0 * hw * hh - ( 4.0 - kPi ) * r * r;
			Check( straightWorst <= kStraightTolerance && curvedWorst <= curvedTolerance( r ) && wrongInside == 0 && wrongOutside == 0,
			       fmt( "%dx%d %.0f x %.0f px at (%.1f, %.1f), corners %.1f px: straight edges worst %.1e (bound %.0e), %d pixels round the "
			            "corners worst %.3f (bound %.2f); %d whole pixels the whole card's (%d not; %s), %d outside untouched (%d not); "
			            "covers %.2f px^2 of %.2f",
			            raster.w, raster.h, 2 * hw, 2 * hh, cx, cy, r, straightWorst, kStraightTolerance, curved, curvedWorst,
			            curvedTolerance( r ), inside, wrongInside, agreement( apart ).c_str(), outside, wrongOutside, total, area ) );
		}
	return Verdict();
}

//===========================================================================
// --luma: Bright Ink is where the print is lighter than Threshold, by its
// luminance in light.
//===========================================================================
int runLuma( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== luma: Bright Ink covers where the clip's luminance -- Rec. 709's weights on LINEAR light, encoded back -- is "
	             "above Threshold, and ramps across Softness's band about it\n" );
	const Raster raster = kRasters.front();
	//A red ramp: code value ( x + 0.5 ) / w, nothing in green or blue, so
	//green out is the coverage times the sheen's green.
	Floats ramp( static_cast< size_t >( raster.w ) * raster.h * 4 );
	for( int y = 0; y < raster.h; ++y )
		for( int x = 0; x < raster.w; ++x )
		{
			float* o = &ramp[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( ( x + 0.5 ) / raster.w );
			o[ 1 ] = o[ 2 ] = 0.0f;
			o[ 3 ]          = 1.0f;
		}
	for( double threshold : { 0.3, 0.45 } )
		for( double softness : { 0.0, 0.1 } )
		{
			Floats outs[ 2 ];
			for( int k = 0; k < 2; ++k )
			{
				Rig* const made = rigs.At( raster.w, raster.h, &ramp );
				if( !made )
					return 1;
				Rig& rig = *made;
				measureSetup( rig );
				setPose( rig, { 0, 0, 0, 20, 0.0, 0.0 } );
				rig.Set( PT_LAMP_SIZE, ParamFromLampSize( 20.0 * kDegree ) );
				rig.Option( PT_AREA, k == 0 ? Area::BrightInk : Area::WholeCard );
				rig.Set( PT_THRESHOLD, static_cast< float >( threshold ) );
				rig.Set( PT_SOFTNESS, static_cast< float >( softness ) );
				applyPerturb( rig, perturb );
				if( !rig.Render( 1 ) )
					return 1;
				outs[ k ] = rig.Output();
			}
			const double band = InkSoftnessFromParam( static_cast< float >( softness ) );
			int wrong = 0, covered = 0;
			double worst = 0.0;
			const int y  = raster.h / 2;
			for( int x = 0; x < raster.w; ++x )
			{
				const double code  = ( x + 0.5 ) / raster.w;
				const double level = codeOf( 0.2126 * linearOf( code ) );
				double want        = band > 0.0 ? std::clamp( 0.5 + 0.5 * ( level - threshold ) / band, 0.0, 1.0 ) : ( level > threshold ? 1.0 : 0.0 );
				//Within a float's rounding of the threshold either side is right.
				if( band == 0.0 && std::fabs( level - threshold ) < 1e-5 )
					continue;
				const double got = linearAt( outs[ 0 ], raster.w, x, y ).g / linearAt( outs[ 1 ], raster.w, x, y ).g;
				worst            = std::max( worst, std::fabs( got - want ) );
				wrong += std::fabs( got - want ) > 1e-4;
				covered += got > 0.5;
			}
			Check( wrong == 0, fmt( "Threshold %.2f, Softness %.1f across a red ramp: %d of %d pixels covered, worst %.1e against the "
			                        "luminance's code (bound 1e-4), %d wrong",
			                        threshold, softness, covered, raster.w, worst, wrong ) );
		}
	return Verdict();
}


//===========================================================================
// --ink: the print over the foil. The ink is a filter the light crosses
// twice, so the foil shows through it times the ink's own colour on white:
// foil = C (1 - Ink + Ink Base) + sheen (1 - Ink + Ink C), in light, and Mix
// fades from the clip to that in light.
//===========================================================================
int runInk( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== ink: over the foil the print is a filter -- C (1 - Ink + Ink Base) + sheen (1 - Ink + Ink C), in light -- "
	             "and Mix fades to it in light\n" );
	for( const Raster& raster : kRasters )
	{
		auto render = [ & ]( const Floats& picture, double ink, double base, double mix, Floats& out ) {
			Rig* const made = rigs.At( raster.w, raster.h, &picture );
			if( !made )
				return false;
			Rig& rig = *made;
			measureSetup( rig );
			setPose( rig, { 5, 3, -10, 35, 3.0, 2.5 } );
			rig.Set( PT_LAMP_SIZE, ParamFromLampSize( 3.0 * kDegree ) );
			rig.Set( PT_LAMP_LENGTH, ParamFromLampLength( 8.0 ) );
			rig.Set( PT_INK, static_cast< float >( ink ) );
			rig.Set( PT_BASE, static_cast< float >( base ) );
			rig.Set( PT_MIX, static_cast< float >( mix ) );
			applyPerturb( rig, perturb );
			if( !rig.Render( 1 ) )
				return false;
			out = rig.Output();
			return true;
		};
		//The sheen alone: a black clip under a clear film.
		Floats sheen;
		const Floats black = flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 );
		if( !render( black, 0.0, 0.0, 1.0, sheen ) )
			return 1;
		double peak = 0.0;
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
			{
				const Colour s = linearAt( sheen, raster.w, x, y );
				peak           = std::max( { peak, s.r, s.g, s.b } );
			}
		struct Case
		{
			double r, g, b, ink, base, mix;
		};
		const Case cases[] = {
			{ 0.8, 0.3, 0.1, 0.0, 0.0, 1.0 },  { 0.8, 0.3, 0.1, 1.0, 0.0, 1.0 }, { 0.8, 0.3, 0.1, 0.6, 0.4, 1.0 },
			{ 0.2, 0.55, 0.9, 1.0, 1.0, 1.0 }, { 0.2, 0.55, 0.9, 0.6, 0.0, 0.35 },
		};
		for( const Case& c : cases )
		{
			const Floats card = flatCard( raster.w, raster.h, c.r, c.g, c.b );
			Floats out;
			if( !render( card, c.ink, c.base, c.mix, out ) )
				return 1;
			const double clip[ 3 ] = { linearOf( c.r ), linearOf( c.g ), linearOf( c.b ) };
			double worst           = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const Colour s      = linearAt( sheen, raster.w, x, y );
					const Colour g      = linearAt( out, raster.w, x, y );
					const double sh[ 3 ] = { s.r, s.g, s.b }, got[ 3 ] = { g.r, g.g, g.b };
					for( int k = 0; k < 3; ++k )
					{
						const double foil = clip[ k ] * ( 1.0 - c.ink + c.ink * c.base ) + sh[ k ] * ( 1.0 - c.ink + c.ink * clip[ k ] );
						const double want = clip[ k ] + c.mix * ( foil - clip[ k ] );
						//Relative to the brightest it gets: the encode's float rounding scales with it.
						worst = std::max( worst, std::fabs( got[ k ] - want ) / std::max( 1.0, want ) );
					}
				}
			Check( worst < 2e-4 && peak > 0.05,
			       fmt( "%dx%d print (%.2f, %.2f, %.2f), Ink %.1f, Base %.1f, Mix %.2f: worst %.1e against the filter model (bound 2e-4); "
			            "the sheen peaks at %.2f",
			            raster.w, raster.h, c.r, c.g, c.b, c.ink, c.base, c.mix, worst, peak ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --passthrough: no foil, the clip bit for bit.
//===========================================================================
int runPassthrough( const Perturb& perturb )
{
	Rigs rigs;
	std::printf( "\n=== passthrough: at Mix 0 every area and pattern gives back the clip bit for bit; with foil, every pixel outside "
	             "its area does, and alpha is never touched\n" );
	for( const Raster& raster : kRasters )
	{
		//The card, with alpha falling across it in eight-bit steps.
		Floats card = buildCard( raster.w, raster.h );
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
				card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + 3 ] = eightBit( 1.2 - 1.4 * ( x + 0.5 ) / raster.w );
		int mixWrong = 0, outsideWrong = 0, alphaWrong = 0, outsideCount = 0;
		for( int area = 0; area < static_cast< int >( Area::Count ); ++area )
			for( int pattern = 0; pattern < static_cast< int >( Pattern::Count ); ++pattern )
				for( float mix : { 0.0f, 1.0f } )
				{
					Rig* const made = rigs.At( raster.w, raster.h, &card );
					if( !made )
						return 1;
					Rig& rig = *made;
					rig.Option( PT_MOTION, Motion::Hold );
					rig.Set( PT_AREA, static_cast< float >( area ) );
					rig.Set( PT_PATTERN, static_cast< float >( pattern ) );
					rig.Set( PT_MIX, mix );
					applyPerturb( rig, perturb );
					if( !rig.Render( 1 ) )
						return 1;
					const Floats out = rig.Output();
					//Where the area has nothing, by the harness's own reckoning:
					//the window's outside a pixel clear of its edge, alpha 0.
					const double cx = 0.5 * raster.w, cy = 0.64 * raster.h, hw = 0.42 * raster.w, hh = 0.21 * raster.h;
					for( int y = 0; y < raster.h; ++y )
						for( int x = 0; x < raster.w; ++x )
						{
							const float* o = pixelAt( out, raster.w, x, y );
							const float* s = pixelAt( card, raster.w, x, y );
							alphaWrong += o[ 3 ] != s[ 3 ];
							const bool same = o[ 0 ] == s[ 0 ] && o[ 1 ] == s[ 1 ] && o[ 2 ] == s[ 2 ];
							if( mix == 0.0f )
							{
								mixWrong += !same;
								continue;
							}
							bool none = false;
							if( area == static_cast< int >( Area::ArtWindow ) )
								none = std::fabs( x + 0.5 - cx ) > hw + 1.0 || std::fabs( y + 0.5 - cy ) > hh + 1.0;
							if( area == static_cast< int >( Area::AroundArt ) )
								none = std::fabs( x + 0.5 - cx ) < hw - 12.0 && std::fabs( y + 0.5 - cy ) < hh - 12.0;
							if( area == static_cast< int >( Area::ClipAlpha ) )
								none = s[ 3 ] == 0.0f;
							if( none )
							{
								++outsideCount;
								outsideWrong += !same;
							}
						}
				}
		Check( mixWrong == 0 && outsideWrong == 0 && alphaWrong == 0 && outsideCount > 0,
		       fmt( "%dx%d every area and pattern: %d pixels changed at Mix 0, %d of %d changed outside the area, %d alphas changed",
		            raster.w, raster.h, mixWrong, outsideWrong, outsideCount, alphaWrong ) );
	}
	return Verdict();
}

//===========================================================================
// --smooth: tilting the card changes the picture smoothly.
//===========================================================================
int runSmooth( const Perturb& perturb )
{
	std::printf( "\n=== smooth: the picture is a smooth function of the tilt -- halve the tilt's step and the largest change between steps "
	             "halves (ratio near 2; at least 1.6) -- so automating Tilt never steps\n" );
	const Raster raster = kRasters.back();
	auto sweep          = [ & ]( unsigned int control, double step ) {
        Rig rig;
        if( !rig.Init( raster.w, raster.h ) )
            return -1.0;
        rig.Option( PT_MOTION, Motion::Hold );
        rig.Option( PT_AREA, Area::WholeCard );
        applyPerturb( rig, perturb );
        double biggest = 0.0;
        Floats last;
        for( double t = -0.3; t <= 0.7 + 1e-9; t += step )
        {
            rig.Set( control, ParamFromTilt( t * kDegree ) );
            if( !rig.Render( 1 ) )
                return -1.0;
            Floats out = rig.Output();
            if( !last.empty() )
                for( size_t i = 0; i < out.size(); i += 4 )
                    for( int c = 0; c < 3; ++c )
                        biggest = std::max( biggest, static_cast< double >( std::fabs( out[ i + c ] - last[ i + c ] ) ) );
            last.swap( out );
        }
        return biggest;
	};
	for( unsigned int control : { static_cast< unsigned int >( PT_TILT_X ), static_cast< unsigned int >( PT_TILT_Y ) } )
	{
		const double coarse = sweep( control, 1.0 / 32.0 ), fine = sweep( control, 1.0 / 64.0 );
		if( coarse < 0.0 || fine < 0.0 )
			return 1;
		const double ratio = coarse / std::max( fine, 1e-12 );
		Check( ratio >= 1.6 && coarse > 1e-4,
		       fmt( "%s from -0.3 to 0.7 degrees: the largest change a step, 1/32 degree %.2e, 1/64 degree %.2e, ratio %.2f",
		            InfoOf( control ).name, coarse, fine, ratio ) );
	}
	return Verdict();
}

//===========================================================================
// --motion (no GL): the card's motion.
//===========================================================================
int runMotion( const Perturb& perturb )
{
	std::printf( "\n=== motion: the phase is advanced by Rate x the frame's time, so turning Motion Rate changes the speed and never the "
	             "place; each motion moves what it says\n" );
	{
		Wobble wobble;
		wobble.SetAbsoluteForTest( perturb.absoluteMotion );
		const double dt = 1.0 / 60.0;
		double worst    = 0.0;
		double rate     = 0.3;
		double before   = 0.0;
		for( int f = 0; f < 600; ++f )
		{
			if( f == 200 )
				rate = 1.7;
			if( f == 400 )
				rate = 0.05;
			wobble.Advance( rate, dt );
			const double step = std::remainder( wobble.Phase() - before, 1.0 );
			worst             = std::max( worst, std::fabs( step - rate * dt ) );
			before            = wobble.Phase();
		}
		Check( worst < 1e-12, fmt( "600 frames, Motion Rate 0.3, then 1.7 at frame 200, then 0.05 at 400: every frame's step is Rate x dt, "
		                           "worst %.1e",
		                           worst ) );
	}
	{
		Pose set;
		set.tiltX = 3.0 * kDegree;
		set.tiltY = -2.0 * kDegree;
		set.lampX = 5.0 * kDegree;
		set.lampY = 30.0 * kDegree;
		const double depth = 10.0 * kDegree;
		int wrong          = 0;
		for( int k = 0; k < 64; ++k )
		{
			const double phase = k / 64.0, a = std::sin( 2.0 * kPi * phase ), b = std::cos( 2.0 * kPi * phase );
			const Pose hold = PoseOf( Motion::Hold, depth, phase, set ), tip = PoseOf( Motion::Tip, depth, phase, set );
			const Pose swing = PoseOf( Motion::Swing, depth, phase, set ), circle = PoseOf( Motion::Circle, depth, phase, set );
			const Pose orbit = PoseOf( Motion::LampOrbit, depth, phase, set );
			auto same        = []( const Pose& p, double tx, double ty, double lx, double ly ) {
                return std::fabs( p.tiltX - tx ) < 1e-15 && std::fabs( p.tiltY - ty ) < 1e-15 && std::fabs( p.lampX - lx ) < 1e-15
                       && std::fabs( p.lampY - ly ) < 1e-15;
			};
			wrong += !same( hold, set.tiltX, set.tiltY, set.lampX, set.lampY );
			wrong += !same( tip, set.tiltX, set.tiltY + depth * a, set.lampX, set.lampY );
			wrong += !same( swing, set.tiltX + depth * a, set.tiltY, set.lampX, set.lampY );
			wrong += !same( circle, set.tiltX + depth * a, set.tiltY + depth * b, set.lampX, set.lampY );
			wrong += !same( orbit, set.tiltX, set.tiltY, set.lampX + depth * a, set.lampY + depth * b );
		}
		Check( wrong == 0, fmt( "Hold, Tip, Swing, Circle and Lamp Orbit over a cycle: Tip moves Tilt Y alone, Swing Tilt X alone, "
		                        "Circle both a quarter cycle apart, Lamp Orbit the lamp alone (%d wrong)",
		                        wrong ) );
	}
	return Verdict();
}

//===========================================================================
// --clock: the motion keeps the host's time, whatever unit it comes in.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: the card's motion follows the clock in seconds, beats or bars, and a host whose clock has run to "
	             "499,217,238 ms renders the same take as one that started at 0 s\n" );
	const Raster raster = kRasters.back();
	auto take           = [ & ]( bool late, MotionSync sync, Floats& out, Pose& pose, double& phase ) {
        Rig rig;
        rig.bpm = 128.0;
        if( late )
        {
            rig.timeOrigin = 499217238.0;
            rig.timeUnit   = 1000.0;
            rig.clockScale = perturb.clockAsSeconds ? 1.0 : 0.001;
        }
        if( !rig.Init( raster.w, raster.h ) )
            return false;
        rig.Option( PT_MOTION, Motion::Circle );
        rig.Option( PT_MOTION_SYNC, sync );
        rig.Set( PT_MOTION_RATE, ParamFromMotionRate( 0.37 ) );
        applyPerturb( rig, perturb );
        if( !rig.Render( 130 ) )
            return false;
        out   = rig.Output();
        pose  = rig.plugin.LastPose();
        phase = rig.plugin.MotionPhase();
        return true;
	};
	for( MotionSync sync : { MotionSync::Free, MotionSync::Beat, MotionSync::Bar } )
	{
		Floats early, late;
		Pose pe, pl;
		double fe = 0.0, fl = 0.0;
		if( !take( false, sync, early, pe, fe ) || !take( true, sync, late, pl, fl ) )
			return 1;
		double worst = 0.0;
		for( size_t i = 0; i < early.size(); ++i )
			worst = std::max( worst, static_cast< double >( std::fabs( early[ i ] - late[ i ] ) ) );
		//129 frames of 1/60 s advance it (the first has no time before it).
		const double units = sync == MotionSync::Beat ? 128.0 / 60.0 : sync == MotionSync::Bar ? 128.0 / 240.0 : 1.0;
		const double rate  = MotionRateFromParam( ParamFromMotionRate( 0.37 ) );
		double want        = rate * units * 129.0 / 60.0;
		want -= std::floor( want );
		const double miss = std::fabs( std::remainder( fe - want, 1.0 ) );
		Check( worst <= 1e-6 && miss < 1e-9,
		       fmt( "%s, Circle at 0.37: after 130 frames the phase is %.6f (Rate x time %.6f); the late host's take differs by "
		            "%.1e (bound 1e-6)",
		            InfoOf( PT_MOTION_SYNC ).options[ static_cast< int >( sync ) ], fe, want, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back, for every area, and through a rebuild of the colour table\n" );
	for( Area area : { Area::ArtWindow, Area::AroundArt, Area::WholeCard, Area::BrightInk, Area::DarkInk, Area::ClipAlpha } )
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.Option( PT_AREA, area );
		const int buildsBefore = rig.plugin.TableBuilds();
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 6; ++frame )
		{
			//A resize on frame 2, and a new Groove Depth on frame 3: the colour
			//table is rebuilt under the host's state too.
			if( frame == 2 && !rig.Resize( 256, 144 ) )
				return 1;
			if( frame == 3 )
				rig.Set( PT_GROOVE, ParamFromGroove( 250.0 ) );
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, rig.width, rig.height );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, rig.width, rig.height );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == rig.width && viewport[ 3 ] == rig.height, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		const int builds = rig.plugin.TableBuilds() - buildsBefore;
		Check( problems == 0 && builds == 2,
		       fmt( "%-10s six frames, a resize on the third, a new groove on the fourth (the table built %d times, 2 wanted): viewport, "
		            "vertex array, array buffer, program, active unit, framebuffer, blend, scissor, clear colour, colour mask, eight "
		            "texture units (%d wrong:%s)",
		            InfoOf( PT_AREA ).options[ static_cast< int >( area ) ], builds, problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}


//===========================================================================
// --controls (no GL): every slider's ends and middle are what Controls.h
// says, written here as numbers, and every inverse undoes its mapping. The
// optics checks read the mappings through the plugin's own functions, so a
// wrong mapping would move the plugin and the reference together; this is
// the check that does not.
//===========================================================================
int runControls( const Perturb& )
{
	std::printf( "\n=== controls: each slider's ends and middle, against the numbers Controls.h states, and each inverse undoes its "
	             "mapping\n" );
	struct Mapping
	{
		const char* name;
		std::function< double( float ) > forward;
		std::function< float( double ) > inverse;
		double low, middle, high;///< in the units the header states
		double unit;             ///< what those units are in the plugin's
	};
	const double geo = 0.0;//marks "the geometric mean": computed below
	const std::vector< Mapping > list = {
		{ "Tilt (deg)", TiltFromParam, ParamFromTilt, -40.0, 0.0, 40.0, kDegree },
		{ "Motion Depth (deg)", MotionDepthFromParam, ParamFromMotionDepth, 0.0, 20.0, 40.0, kDegree },
		{ "Motion Rate (/s)", MotionRateFromParam, ParamFromMotionRate, 0.02, geo, 4.0, 1.0 },
		{ "Lamp X/Y (deg)", LampAngleFromParam, ParamFromLampAngle, -80.0, 0.0, 80.0, kDegree },
		{ "Lamp Size (deg)", LampSizeFromParam, ParamFromLampSize, 0.2, geo, 20.0, kDegree },
		{ "Lamp Length", LampLengthFromParam, ParamFromLampLength, 1.0, geo, 20.0, 1.0 },
		{ "Lamp Brightness", LampBrightnessFromParam, ParamFromLampBrightness, 0.0, 2.0, 4.0, 1.0 },
		{ "Pitch (nm)", PitchFromParam, ParamFromPitch, 500.0, geo, 3000.0, 1.0 },
		{ "Grating Angle (deg)", GratingAngleFromParam, ParamFromGratingAngle, 0.0, 90.0, 180.0, kDegree },
		{ "Groove Depth (nm)", GrooveFromParam, ParamFromGroove, 0.0, 200.0, 400.0, 1.0 },
		{ "Pattern Scale", PatternScaleFromParam, ParamFromPatternScale, 0.25, geo, 4.0, 1.0 },
		{ "Facet Tilt (deg)", FacetTiltFromParam, ParamFromFacetTilt, 0.0, 10.0, 20.0, kDegree },
		{ "Roughness (deg)", RoughnessFromParam, ParamFromRoughness, 0.0, 5.0, 10.0, kDegree },
		//1/distance = (1 - v)^2 / closest: closest at 0, four times it at the middle, infinity (0) at the top.
		{ "1/Viewer Distance", InverseViewerFromParam, nullptr, 1.0 / 0.6, 0.25 / 0.6, 0.0, 1.0 },
		{ "1/Lamp Distance", InverseLampFromParam, nullptr, 1.0 / 0.4, 0.25 / 0.4, 0.0, 1.0 },
		{ "Corner (of height)", CornerFromParam, nullptr, 0.0, 0.125, 0.25, 1.0 },
		{ "Softness, window (of height)", WindowSoftnessFromParam, nullptr, 0.0, 0.05, 0.1, 1.0 },
		{ "Softness, ink", InkSoftnessFromParam, nullptr, 0.0, 0.25, 0.5, 1.0 },
	};
	for( const Mapping& m : list )
	{
		const double middle = m.middle == geo && m.low > 0.0 ? std::sqrt( m.low * m.high ) : m.middle;
		const double want[ 3 ] = { m.low, middle, m.high };
		double worst           = 0.0;
		for( int k = 0; k < 3; ++k )
		{
			const double got = m.forward( 0.5f * static_cast< float >( k ) ) / m.unit;
			worst            = std::max( worst, std::fabs( got - want[ k ] ) / std::max( 1.0, std::fabs( want[ k ] ) ) );
		}
		double trip = 0.0;
		if( m.inverse )
			for( int k = 0; k <= 16; ++k )
			{
				const float v = static_cast< float >( k ) / 16.0f;
				trip          = std::max( trip, static_cast< double >( std::fabs( m.inverse( m.forward( v ) ) - v ) ) );
			}
		Check( worst < 1e-9 && trip < 1e-6, fmt( "%-28s %g, %g, %g at 0, 0.5, 1 (worst %.1e, bound 1e-9); inverse round trip %.1e (bound 1e-6)",
		                                        m.name, m.forward( 0.0f ) / m.unit, m.forward( 0.5f ) / m.unit, m.forward( 1.0f ) / m.unit,
		                                        worst, trip ) );
	}
	//The two distances' inverses: d at the middle is four times the closest, and the top is infinity.
	Check( std::fabs( ParamFromViewer( 2.4 ) - 0.5f ) < 1e-6 && ParamFromViewer( 0.0 ) == 1.0f
	           && std::fabs( ParamFromLampDistance( 1.6 ) - 0.5f ) < 1e-6 && ParamFromLampDistance( 0.0 ) == 1.0f,
	       "Viewer Distance 2.4 widths and Lamp Distance 1.6 widths are the middle of their sliders; infinity is the top" );
	return Verdict();
}

//===========================================================================
// --cues (no GL): options and events step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options and events, and ramps standard controls\n" );
	std::istringstream sheet( "0 Pattern 0\n60 Pattern 3\n0 Motion 0\n60 Motion 4\n0 Tilt X 0\n60 Tilt X 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	HolofoilPlugin plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}


//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	HolofoilPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Holofoil: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
	};
	const Case cases[] = {
		{ "defaults (Art Window, Rainbow)", []( Rig& ) {} },
		{ "Whole Card, Cracked Ice", []( Rig& r ) {
			 r.Option( PT_AREA, Area::WholeCard );
			 r.Option( PT_PATTERN, Pattern::CrackedIce );
		 } },
		{ "Whole Card, Cosmos", []( Rig& r ) {
			 r.Option( PT_AREA, Area::WholeCard );
			 r.Option( PT_PATTERN, Pattern::Cosmos );
		 } },
		{ "Whole Card, Crosshatch", []( Rig& r ) {
			 r.Option( PT_AREA, Area::WholeCard );
			 r.Option( PT_PATTERN, Pattern::Crosshatch );
		 } },
		{ "Groove Depth automated (table every frame)", []( Rig& r ) {
			 r.Option( PT_AREA, Area::WholeCard );
			 r.beforeFrame = [ &r ]( int f ) { r.Set( PT_GROOVE, 0.25f + 0.25f * static_cast< float >( f % 2 ) ); };
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 120 frames of warm-up, median and worst of 120\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			rig.bpm = 120.0;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 120 ) )
				return 1;
			constexpr int kTimed = 120;
			std::vector< double > times, cpu;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
				cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::sort( cpu.begin(), cpu.end() );
			std::printf( "  %-40s %-6s GPU median %6.2f ms, worst %6.2f  (%4.1f%% of a 60 fps frame); wall %5.2f ms\n", c.what, size.name,
			             times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ), cpu[ kTimed / 2 ] );
		}
	glDeleteQueries( 1, &query );
	return 0;
}



struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "motion", runMotion, true },
		{ "passthrough", runPassthrough, false },
		{ "grating", runGrating, false },
		{ "patterns", runPatterns, false },
		{ "mirror", runMirror, false },
		{ "groove", runGroove, false },
		{ "lamp", runLamp, false },
		{ "complement", runComplement, false },
		{ "window", runWindow, false },
		{ "luma", runLuma, false },
		{ "ink", runInk, false },
		{ "smooth", runSmooth, false },
		{ "clock", runClock, false },
		{ "state", runState, false },
		{ "cues", runCues, true },
		{ "names", runNames, true },
		{ "controls", runControls, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "motion", runMotion, "the phase as Rate x the time so far", []( Perturb& p ) { p.absoluteMotion = true; } );
	add( "passthrough", runPassthrough, "every pixel through linear light and back", []( Perturb& p ) { p.hooks = kHookRoundTrip; } );
	add( "grating", runGrating, "the lamp's direction subtracted: a grating lit from behind", []( Perturb& p ) { p.hooks = kHookMinusLamp; } );
	add( "grating", runGrating, "the grating's direction ignored: lambda = d |s|", []( Perturb& p ) { p.hooks = kHookNoDirection; } );
	add( "grating", runGrating, "the lamp's distance ignored", []( Perturb& p ) { p.hooks = kHookLampAtInfinity; } );
	add( "patterns", runPatterns, "the optics use Grating Angle everywhere, whatever is embossed", []( Perturb& p ) { p.hooks = kHookOneAngle; } );
	add( "mirror", runMirror, "the lamp's distance ignored", []( Perturb& p ) { p.hooks = kHookLampAtInfinity; } );
	add( "mirror", runMirror, "the lamp's direction subtracted", []( Perturb& p ) { p.hooks = kHookMinusLamp; } );
	add( "groove", runGroove, "the mirror keeps all its light whatever the groove", []( Perturb& p ) { p.hooks = kHookNoEfficiency; } );
	add( "lamp", runLamp, "Roughness ignored", []( Perturb& p ) { p.hooks = kHookNoRoughness; } );
	add( "complement", runComplement, "Around Art from an unsmoothed window", []( Perturb& p ) { p.hooks = kHookHardReverse; } );
	add( "window", runWindow, "the window's corners square", []( Perturb& p ) { p.hooks = kHookSquareCorners; } );
	add( "luma", runLuma, "the ink's luminance from code values", []( Perturb& p ) { p.hooks = kHookCodeLuma; } );
	add( "ink", runInk, "the foil's light added over the print, the ink filtering nothing", []( Perturb& p ) { p.hooks = kHookPlainAdd; } );
	add( "smooth", runSmooth, "the tilt to the nearest whole degree", []( Perturb& p ) { p.snapTilt = true; } );
	add( "clock", runClock, "a millisecond host taken for a seconds one", []( Perturb& p ) { p.clockAsSeconds = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/holofoil.png";
	std::vector< std::string > settings;
	int width = 720, height = 1008, frames = 90;
	double fps = 60.0, bpm = 120.0;
	std::string mode, scriptPath;
	int filmFrames = -1;
	bool sizeGiven = false, still = false, black = false;
	std::map< int, std::vector< std::string > > later;//--at FRAME "Name=V"

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "hftest -- render Holofoil offline and measure the light its foil sends\n\n"
			             "  --out PATH        render the card and write a PNG (default /tmp/holofoil.png)\n"
			             "  --frozen          (with --out) the card's art does not move\n"
			             "  --black           (with --out) a black clip: with Ink 0, the foil's own light\n"
			             "  --size WxH        render size (default 720x1008, a card)\n"
			             "  --frames N        frames before reading back (default 90)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --bpm N           the synthetic beat (default 120; 0 sends none)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --at N \"Name=V\"  (with --out) set it at frame N instead; an event is released the frame after\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the moving card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: see tools/verify.sh;  --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--bpm" && hasNext )
			bpm = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--frozen" )
			still = true;
		else if( argument == "--black" )
			black = still = true;
		else if( argument == "--at" && i + 2 < argc )
		{
			const int at = std::atoi( argv[ ++i ] );
			later[ at ].push_back( argv[ ++i ] );
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		HolofoilPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, bpm, scriptPath, 0, true, settings );
	else if( mode == "film" )
		result = runPipe( width, height, fps, bpm, scriptPath, filmFrames, false, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		rig.bpm = bpm;
		const Floats blackCard = flatCard( width, height, 0.0, 0.0, 0.0 );
		if( !rig.Init( width, height, black ? &blackCard : nullptr ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( !still )
				rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildCard( width, height, rig.Seconds( f ) ) ); };
			bool rendered = true;
			std::vector< std::pair< int, std::string > > release;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				for( const auto& r : release )
					if( r.first == f )
					{
						std::string ignored;
						applySetting( rig.plugin, r.second + "=0", ignored );
					}
				const auto due = later.find( f );
				if( due != later.end() )
					for( const std::string& setting : due->second )
					{
						std::string error;
						if( !applySetting( rig.plugin, setting, error ) )
						{
							std::fprintf( stderr, "--at %d %s: %s\n", f, setting.c_str(), error.c_str() );
							return 2;
						}
						//An event is a press: released on the next frame, as a host would.
						const std::string name = setting.substr( 0, setting.find( '=' ) );
						for( const NamedParameter& parameter : listParameters( rig.plugin ) )
							if( parameter.name == name && parameter.type == FF_TYPE_EVENT )
								release.emplace_back( f + 1, name );
					}
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), the card at tilt (%.2f, %.2f) degrees, the lamp at (%.2f, %.2f)\n",
				             outPath.c_str(), width, height, frames, fps, frames / fps, rig.plugin.LastPose().tiltX / kDegree,
				             rig.plugin.LastPose().tiltY / kDegree, rig.plugin.LastPose().lampX / kDegree, rig.plugin.LastPose().lampY / kDegree );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
