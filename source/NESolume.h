#pragma once

#include <FFGLSDK.h>

#include <chrono>

#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    NESolume -- retro console video hardware for Resolume.

    The effect is a model of constraints, not a stack of filters. A picture is
    averaged down onto a real machine's raster, forced through that machine's
    palette with its attribute cells enforced, and scaled back up as fat
    pixels. The recognisable artefacts — chunky dithered gradients, colours
    bleeding between objects, tiles landing in the wrong places — are what
    the constraints and their corruption do to a picture, not drawings of
    those artefacts.

    See Shaders.h for the stages, Consoles.h for the machines, and AGENTS.md
    for the traps.
*/
class NESolume : public CFFGLPlugin
{
public:
	NESolume();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

private:
	/// Everything the operator can reach, in the order Resolume shows them:
	/// what the machine is, what its picture looks like, how it is failing,
	/// and how much of it is in the programme.
	enum ParamID : FFUInt32
	{
		//Picture
		PT_CONSOLE,
		PT_PIXEL_SIZE,
		PT_COLOUR_DEPTH,
		PT_DITHER,
		PT_CLASH,
		PT_GRID,

		//Distortion
		PT_WAVE,
		PT_SHAKE,

		//Glitch
		PT_BLOCK_GLITCH,
		PT_LINE_GLITCH,
		PT_PALETTE_GLITCH,
		PT_GARBAGE,
		PT_GLITCH_RATE,

		//Output
		PT_MIX,

		//Preset. Declared after the real controls so their IDs — which a saved
		//composition refers to — do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ nesolume::presets::kParamCount ] = {
		PT_CONSOLE, PT_PIXEL_SIZE, PT_COLOUR_DEPTH, PT_DITHER, PT_CLASH, PT_GRID,
		PT_WAVE, PT_SHAKE, PT_BLOCK_GLITCH, PT_LINE_GLITCH, PT_PALETTE_GLITCH,
		PT_GARBAGE, PT_GLITCH_RATE
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	bool compileShaders();
	void releaseBuffers();

	/// Seconds to drive the wave and the glitch clock with. The host's
	/// timeline when there is one, so a re-render is reproducible; the wall
	/// clock when there is not, so the picture is not frozen in a host that
	/// never calls SetTime.
	float elapsedSeconds();

public:
	/// Clock test hooks. The harness DECLARES its unit rather than leaving
	/// elapsedSeconds to infer one -- an absolute time in a single frame is
	/// genuinely ambiguous, and an implicit unit is what let a thousand-times-
	/// fast bug sit here unnoticed.
	void SetClockScaleForTest( double scale );
	double ClockScaleForTest() const;

	/// Which glitch tick is showing at a given moment. `--glitch` needs it: the
	/// thing being tested is that a Rate change does NOT re-roll the machine's
	/// luck, and reading the tick either side of one says so directly, where
	/// comparing rendered frames could not -- the content of a glitch is random,
	/// so two frames differ whether or not the tick moved.
	float GlitchTickForTest( float seconds );

private:


	ffglex::FFGLShader downresShader;
	ffglex::FFGLShader tileShader;
	ffglex::FFGLShader quantizeShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	nesolume::PassBuffer downresBuffer;//the picture on the console's raster
	nesolume::PassBuffer tileBuffer;   //one texel per attribute cell: its mean colour
	nesolume::PassBuffer quantBuffer;  //...quantised to the console's colours

	//---------------------------------------------------------------------
	// The glitch tick.
	//
	// Events hold for one tick and re-roll on the next, so Rate is how often the
	// machine's luck changes. The tick used to be `floor( time * rateHz )`,
	// which means a Rate change moves it by `time * delta` -- and `time` is
	// however long the composition has been open, so nudging Rate an hour in
	// skips thousands of ticks at once. That is the same defect orrery issue #6
	// reported for its Speed control, and here it reads as the picture
	// convulsing the moment the control is touched.
	//
	// So the tick is accumulated instead: on a Rate change the count reached so
	// far is carried forward and counting continues from there at the new rate.
	// Once per change rather than per frame, so nothing accumulates.
	//
	// Winding Rate to zero now HOLDS the glitch that is showing rather than
	// snapping back to tick zero, which is what an operator reaching for the
	// bottom of the control wants. A composition that has never had the control
	// touched still starts at tick zero, so a cold load is as reproducible as it
	// ever was.
	//---------------------------------------------------------------------
	float GlitchTick( float seconds );

	float tickAnchor     = 0.0f;///< tick count already reached at `tickAnchorTime`
	float tickAnchorTime = 0.0f;///< the clock reading that count belongs to
	float tickAnchorRate = -1.0f;///< rate in force since then; < 0 until the first frame

	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen = false;
	std::chrono::steady_clock::time_point startTime;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
