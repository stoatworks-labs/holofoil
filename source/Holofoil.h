#pragma once

#include "Card.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace holofoil
{
/**
    The plugin.

    One pass to the host: every pixel of the clip is a point on a foil card,
    and what it sends to the eye is the grating equation's answer for the
    eye, the lamp and the pattern's grating there (Shaders.h). A second pass
    builds the colour table the first reads, and only when Groove Depth
    changes. Card.h has the frame and the motion; AGENTS.md has why.
*/
class HolofoilPlugin : public CFFGLPlugin
{
public:
	HolofoilPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `hftest` can measure what the shipping passes produced,
	// and show that its checks fail against a wrong model.
	//-------------------------------------------------------------------

	/// The offline harness DECLARES its clock unit (seconds) rather than
	/// letting the plugin vote on it against the wall clock -- a harness
	/// renders hundreds of frames a second, which the vote would read as
	/// milliseconds.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}
	/// The --smooth negative control: the tilt to the nearest whole degree,
	/// as a stepped control would give it.
	void SetSnapTiltForTest( bool snap )
	{
		snapTilt = snap;
	}
	/// The --motion negative control.
	void SetAbsoluteMotionForTest( bool absolute )
	{
		wobble.SetAbsoluteForTest( absolute );
	}

	/// Where the card and the lamp were last frame, after Motion.
	const Pose& LastPose() const
	{
		return lastPose;
	}
	double MotionPhase() const
	{
		return wobble.Phase();
	}
	/// How many times the colour table has been built.
	int TableBuilds() const
	{
		return tableBuilds;
	}

private:
	void UpdateClock();
	bool ensureTable();

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader tableShader;
	ffglex::FFGLShader foilShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer table;           ///< RGBA32F, kTableColumns x kTableHeight
	double tableGroove = -1.0;  ///< the Groove Depth it was built for
	int tableHooks     = -1;    ///< and the hooks
	int tableBuilds    = 0;

	Wobble wobble;
	Pose lastPose;

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;

	//-------------------------------------------------------------------
	// The beat, as SetBeatInfo last said.
	//-------------------------------------------------------------------
	float bpm      = 120.0f;
	float barPhase = 0.0f;

	int hooks     = 0;
	bool snapTilt = false;
};

} // namespace holofoil
