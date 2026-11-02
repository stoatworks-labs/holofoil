#include "Holofoil.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace holofoil
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. A stalled host must
/// not jerk the card.
constexpr double kMaxFrameDelta = 0.25;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void setInt2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( shader.FindUniform( name ), x, y );
}

/// The pattern's natural cell, picture widths.
double cellOf( Pattern pattern )
{
	switch( pattern )
	{
	case Pattern::Crosshatch: return shaders::kHatchSpacing;
	case Pattern::Cosmos: return shaders::kCosmosCell;
	case Pattern::CrackedIce: return shaders::kIceCell;
	case Pattern::Sparkle: return shaders::kSparkleCell;
	case Pattern::Rainbow:
	case Pattern::Count: break;
	}
	return 1.0;
}

double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree.
static_assert( PT_COUNT - PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
HolofoilPlugin::HolofoilPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Motion runs in seconds (or beats), so the plugin has to know what a
	//second is.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged parameter is
	// a plain 0..1 float: SetParamInfo clamps a STANDARD default into 0..1
	// before a range could be attached (SDK b1afaf9).
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]          = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::XPos: SetParamInfo( id, info.name, FF_TYPE_XPOS, info.defaultValue ); break;
		case Kind::YPos: SetParamInfo( id, info.name, FF_TYPE_YPOS, info.defaultValue ); break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );
}

//---------------------------------------------------------------------------
FFResult HolofoilPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	};
	const Stage stages[] = {
		{ &tableShader, shaders::Pass::Table, "table" },
		{ &foilShader, shaders::Pass::Foil, "foil" },
	};
	for( const Stage& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the plugin simply
		//does nothing in Resolume. These lines are the only record of which
		//pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Holofoil: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void HolofoilPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;
	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, by way of
	//millpond, schlieren and slitscan.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				//The clock is about to jump from wall time to the host's own,
				//which can be hours in: no dt across the jump.
				settledJump = true;
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
bool HolofoilPlugin::ensureTable()
{
	//Before anything is bound for the frame: every ffglex::Scoped* binding
	//clears to 0 on exit rather than restoring, and FFGLFBO::Initialise binds
	//under one.
	if( !table.Ensure( shaders::kTableColumns, shaders::kTableHeight, GL_RGBA32F, PassBuffer::Sampling::Linear ) )
		return false;

	const double groove   = GrooveFromParam( params[ PT_GROOVE ] );
	const int tableAffect = hooks & shaders::kHookNoEfficiency;
	if( groove == tableGroove && tableAffect == tableHooks )
		return true;

	float xyzToDisplay[ 9 ];
	XyzToDisplay( xyzToDisplay );
	{
		ScopedFBOBinding fbo( table.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, shaders::kTableColumns, shaders::kTableHeight );
		ScopedShaderBinding shader( tableShader.GetGLID() );
		tableShader.Set( "GrooveDepth", static_cast< float >( groove ) );
		glUniformMatrix3fv( tableShader.FindUniform( "XyzToDisplay" ), 1, GL_FALSE, xyzToDisplay );
		tableShader.Set( "Hooks", hooks );
		quad.Draw();
	}
	tableGroove = groove;
	tableHooks  = tableAffect;
	++tableBuilds;
	return true;
}

//---------------------------------------------------------------------------
FFResult HolofoilPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	if( source.Width == 0 || source.Height == 0 )
		return FF_FAIL;
	const GLsizei width  = static_cast< GLsizei >( source.Width );
	const GLsizei height = static_cast< GLsizei >( source.Height );

	//The host's viewport and blend state, put back however this returns.
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// Time, and the card's motion.
	//---------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;

	const auto sync    = static_cast< MotionSync >( OptionIndex( params[ PT_MOTION_SYNC ], static_cast< int >( MotionSync::Count ) ) );
	const double units = sync == MotionSync::Beat ? dt * bpm / 60.0 : sync == MotionSync::Bar ? dt * bpm / 240.0 : dt;
	wobble.Advance( MotionRateFromParam( params[ PT_MOTION_RATE ] ), units );

	Pose set;
	set.tiltX = TiltFromParam( params[ PT_TILT_X ] );
	set.tiltY = TiltFromParam( params[ PT_TILT_Y ] );
	set.lampX = LampAngleFromParam( params[ PT_LAMP_X ] );
	set.lampY = LampAngleFromParam( params[ PT_LAMP_Y ] );
	const auto motion = static_cast< Motion >( OptionIndex( params[ PT_MOTION ], static_cast< int >( Motion::Count ) ) );
	Pose pose         = PoseOf( motion, MotionDepthFromParam( params[ PT_MOTION_DEPTH ] ), wobble.Phase(), set );
	if( snapTilt )
	{
		pose.tiltX = std::round( pose.tiltX / kDegree ) * kDegree;
		pose.tiltY = std::round( pose.tiltY / kDegree ) * kDegree;
	}
	lastPose = pose;

	const Place eye     = EyeInCard( pose.tiltX, pose.tiltY, InverseViewerFromParam( params[ PT_VIEWER ] ) );
	const Place lamp    = LampInCard( pose.tiltX, pose.tiltY, pose.lampX, pose.lampY, InverseLampFromParam( params[ PT_LAMP_DISTANCE ] ) );
	const Spread spread = LampSpread( pose.tiltX, pose.tiltY, pose.lampX, pose.lampY, LampSizeFromParam( params[ PT_LAMP_SIZE ] ),
	                                  LampLengthFromParam( params[ PT_LAMP_LENGTH ] ) );

	//---------------------------------------------------------------------
	// 1. The colour table, if the groove has changed.
	//---------------------------------------------------------------------
	if( !ensureTable() )
	{
		diag::error( "could not allocate the colour table" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// The grating and the area, in double, then handed over as floats.
	//---------------------------------------------------------------------
	const auto pattern  = static_cast< Pattern >( OptionIndex( params[ PT_PATTERN ], static_cast< int >( Pattern::Count ) ) );
	const auto area     = static_cast< Area >( OptionIndex( params[ PT_AREA ], static_cast< int >( Area::Count ) ) );
	const double angle  = GratingAngleFromParam( params[ PT_ANGLE ] );
	const double hatchA = angle + 45.0 * kDegree;
	const double hatchB = angle - 45.0 * kDegree;

	const double halfW  = 0.5 * clamp01( params[ PT_WINDOW_WIDTH ] ) * width;
	const double halfH  = 0.5 * clamp01( params[ PT_WINDOW_HEIGHT ] ) * height;
	const double corner = std::min( { CornerFromParam( params[ PT_CORNER ] ) * height, halfW, halfH } );
	const double ramp   = 1.0 + WindowSoftnessFromParam( params[ PT_SOFTNESS ] ) * height;

	//---------------------------------------------------------------------
	// 2. The foil, into the host's framebuffer and viewport.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( foilShader.GetGLID() );
		bindUnit( 0, source.Handle );
		bindUnit( 1, table.TextureID() );
		foilShader.Set( "InputTexture", 0 );
		foilShader.Set( "Table", 1 );
		setInt2( foilShader, "Size", width, height );
		setInt2( foilShader, "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
		foilShader.Set( "PxToWidth", static_cast< float >( 1.0 / width ) );

		foilShader.Set( "Eye", static_cast< float >( eye.x ), static_cast< float >( eye.y ), static_cast< float >( eye.z ),
		                static_cast< float >( eye.w ) );
		foilShader.Set( "Lamp", static_cast< float >( lamp.x ), static_cast< float >( lamp.y ), static_cast< float >( lamp.z ),
		                static_cast< float >( lamp.w ) );
		foilShader.Set( "LampSpread", static_cast< float >( spread.xx ), static_cast< float >( spread.xy ),
		                static_cast< float >( spread.yy ) );
		foilShader.Set( "RoughSigma", static_cast< float >( RoughnessFromParam( params[ PT_ROUGHNESS ] ) ) );
		foilShader.Set( "Brightness", static_cast< float >( LampBrightnessFromParam( params[ PT_LAMP_BRIGHTNESS ] ) ) );

		foilShader.Set( "Pitch", static_cast< float >( PitchFromParam( params[ PT_PITCH ] ) ) );
		foilShader.Set( "Direction", static_cast< float >( std::cos( angle ) ), static_cast< float >( std::sin( angle ) ) );
		foilShader.Set( "HatchA", static_cast< float >( std::cos( hatchA ) ), static_cast< float >( std::sin( hatchA ) ) );
		foilShader.Set( "HatchB", static_cast< float >( std::cos( hatchB ) ), static_cast< float >( std::sin( hatchB ) ) );
		foilShader.Set( "PatternIndex", static_cast< int >( pattern ) );
		foilShader.Set( "Cell", static_cast< float >( cellOf( pattern ) * PatternScaleFromParam( params[ PT_SCALE ] ) ) );
		foilShader.Set( "FacetSlope", static_cast< float >( std::tan( FacetTiltFromParam( params[ PT_FACET ] ) ) ) );

		foilShader.Set( "Ink", static_cast< float >( clamp01( params[ PT_INK ] ) ) );
		foilShader.Set( "Base", static_cast< float >( clamp01( params[ PT_BASE ] ) ) );

		foilShader.Set( "AreaIndex", static_cast< int >( area ) );
		foilShader.Set( "Window", static_cast< float >( clamp01( params[ PT_WINDOW_X ] ) * width ),
		                static_cast< float >( clamp01( params[ PT_WINDOW_Y ] ) * height ), static_cast< float >( halfW ),
		                static_cast< float >( halfH ) );
		foilShader.Set( "Corner", static_cast< float >( corner ) );
		foilShader.Set( "Ramp", static_cast< float >( ramp ) );
		foilShader.Set( "Threshold", static_cast< float >( clamp01( params[ PT_THRESHOLD ] ) ) );
		foilShader.Set( "Band", static_cast< float >( InkSoftnessFromParam( params[ PT_SOFTNESS ] ) ) );
		foilShader.Set( "MixAmount", static_cast< float >( clamp01( params[ PT_MIX ] ) ) );
		foilShader.Set( "Hooks", hooks );
		quad.Draw();
		unbindTextureUnits( 2 );
		glActiveTexture( GL_TEXTURE0 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult HolofoilPlugin::DeInitGL()
{
	tableShader.FreeGLResources();
	foilShader.FreeGLResources();
	quad.Release();
	table.Destroy();
	tableGroove = -1.0;
	tableHooks  = -1;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult HolofoilPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void HolofoilPlugin::SetBeatInfo( float newBpm, float newBarPhase )
{
	CFFGLPlugin::SetBeatInfo( newBpm, newBarPhase );
	if( newBpm > 0.0f && std::isfinite( newBpm ) )
		bpm = newBpm;
	if( std::isfinite( newBarPhase ) )
		barPhase = newBarPhase - std::floor( newBarPhase );
}

char* HolofoilPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult HolofoilPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult HolofoilPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float HolofoilPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

} // namespace holofoil
