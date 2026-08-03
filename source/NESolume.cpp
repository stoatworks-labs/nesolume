#include "NESolume.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Consoles.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace nesolume;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< NESolume >,                            // Create method
	"NE01",                                               // Plugin unique ID of maximum length 4.
	"NESolume",                                           // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"Retro console video hardware: raster, palette, attribute cells and their glitches", // Plugin description
	"NESolume FFGL effect"                                // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

NESolume::NESolume() :
	startTime( std::chrono::steady_clock::now() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The wave and the glitch clock drift, so the effect needs a clock. Resolume
	//provides one; asking for it means a re-render of the same composition
	//produces the same glitches rather than whatever the wall clock happened to
	//say.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They are set to a working NES rather than to nothing: the raster, the
	// 2C02 palette, a little dither, the attribute areas doing their thing.
	// The faults default to off — a machine that is failing before anyone
	// touches a slider is a machine nobody trusts — but the machine itself
	// must show up on the first frame. An effect that does nothing until
	// eight sliders are moved is an effect nobody finds out is any good.
	//---------------------------------------------------------------------
	params[ PT_CONSOLE ]        = 2.0f;//NES
	params[ PT_PIXEL_SIZE ]     = 0.5f;//native raster
	params[ PT_COLOUR_DEPTH ]   = 0.4f;//~4 bits per channel, Custom only
	params[ PT_DITHER ]         = 0.35f;
	params[ PT_CLASH ]          = 0.5f;
	params[ PT_GRID ]           = 0.0f;

	params[ PT_WAVE ]           = 0.0f;
	params[ PT_SHAKE ]          = 0.0f;

	params[ PT_BLOCK_GLITCH ]   = 0.0f;
	params[ PT_LINE_GLITCH ]    = 0.0f;
	params[ PT_PALETTE_GLITCH ] = 0.0f;
	params[ PT_GARBAGE ]        = 0.0f;
	params[ PT_GLITCH_RATE ]    = 0.3f;

	params[ PT_MIX ]            = 1.0f;

	params[ PT_PRESET ]         = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration. Grouped, because an ungrouped list of fifteen in somebody
	// else's inspector is unusable.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_CONSOLE, "Console", consoleCount(), params[ PT_CONSOLE ] );
	for( int i = 0; i < consoleCount(); ++i )
		SetParamElementInfo( PT_CONSOLE, i, console( i ).name, static_cast< float >( i ) );

	SetParamInfof( PT_PIXEL_SIZE, "Pixel Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_COLOUR_DEPTH, "Colour Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_DITHER, "Dither", FF_TYPE_STANDARD );
	SetParamInfof( PT_CLASH, "Attribute Clash", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRID, "Pixel Grid", FF_TYPE_STANDARD );

	SetParamInfof( PT_WAVE, "Wave", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHAKE, "Shake", FF_TYPE_STANDARD );

	SetParamInfof( PT_BLOCK_GLITCH, "Block Glitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_LINE_GLITCH, "Line Glitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_PALETTE_GLITCH, "Palette Glitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_GARBAGE, "Garbage", FF_TYPE_STANDARD );
	SetParamInfof( PT_GLITCH_RATE, "Glitch Rate", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, "" );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = PT_CONSOLE; i <= PT_GRID; ++i )
		SetParamGroup( i, "Picture" );
	for( FFUInt32 i = PT_WAVE; i <= PT_SHAKE; ++i )
		SetParamGroup( i, "Distortion" );
	for( FFUInt32 i = PT_BLOCK_GLITCH; i <= PT_GLITCH_RATE; ++i )
		SetParamGroup( i, "Glitch" );
	SetParamGroup( PT_MIX, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created NESolume effect" );

	diag::init();
}

FFResult NESolume::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

float NESolume::elapsedSeconds() const
{
	if( hostTimeSeen )
		return static_cast< float >( hostTime );

	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration< float >( now - startTime ).count();
}

bool NESolume::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &downresShader, shaders::kDownresFragment, "downres" },
		{ &tileShader, shaders::kTileFragment, "tile" },
		{ &quantizeShader, shaders::kQuantizeFragment, "quantize" },
		{ &displayShader, shaders::kDisplayFragment, "display" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was.
			diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "NESolume: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult NESolume::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile
	//it is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "NESolume: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult NESolume::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const float outputWidth  = std::max( 1.0f, static_cast< float >( hostViewport[ 2 ] ) );
	const float outputHeight = std::max( 1.0f, static_cast< float >( hostViewport[ 3 ] ) );

	const ConsoleSpec& con = console( static_cast< int >( params[ PT_CONSOLE ] + 0.5f ) );

	//The raster. Line count is the console's scaled by Pixel Size (0.5 is
	//native, the ends are quarter and four-times pixels); width follows the
	//composition's aspect so pixels are square on screen. See AGENTS.md for
	//why width is not the console's.
	const float sizeFactor = std::exp2( ( params[ PT_PIXEL_SIZE ] - 0.5f ) * 4.0f );
	const int rasterH = std::clamp( static_cast< int >( std::lround( con.rasterHeight / sizeFactor ) ), 8, 2048 );
	const int rasterW = std::clamp( static_cast< int >( std::lround( rasterH * outputWidth / outputHeight ) ), 8, 4096 );

	const int tileGridW = ( rasterW + con.tileSize - 1 ) / con.tileSize;
	const int tileGridH = ( rasterH + con.tileSize - 1 ) / con.tileSize;

	//8-bit everywhere: every buffer holds either an average of an 8-bit
	//picture or a palette entry. The tile means get 16-bit float only so the
	//clash arithmetic is not quantised before it runs.
	if( !downresBuffer.Ensure( rasterW, rasterH, GL_RGBA8 )
	    || !tileBuffer.Ensure( tileGridW, tileGridH, GL_RGBA16F )
	    || !quantBuffer.Ensure( rasterW, rasterH, GL_RGBA8 ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	const float time = elapsedSeconds();

	//The glitch clock. Events hold for one tick and re-roll on the next, so
	//Rate is how often the machine's luck changes. At zero the tick never
	//advances and a glitched frame is a still — deterministic, and exactly
	//what a crashed machine does.
	const float rateHz = params[ PT_GLITCH_RATE ] * 18.0f;
	const float glitchKey = rateHz > 0.0f ? std::floor( time * rateHz ) : 0.0f;

	//------------------------------------------------------------------
	// 1. Down onto the console's raster, box-filtered.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( downresBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		downresBuffer.ResizeViewPort();
		ScopedShaderBinding shader( downresShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		downresShader.Set( "InputTexture", 0 );
		downresShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		downresShader.Set( "InputSize", static_cast< float >( input.Width ), static_cast< float >( input.Height ) );
		downresShader.Set( "TargetSize", static_cast< float >( rasterW ), static_cast< float >( rasterH ) );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 2. One texel per attribute cell: its mean colour.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( tileBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		tileBuffer.ResizeViewPort();
		ScopedShaderBinding shader( tileShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( downresBuffer.GetTextureInfo().Handle );

		tileShader.Set( "SourceTexture", 0 );
		tileShader.Set( "MaxUV", 1.0f, 1.0f );
		tileShader.Set( "RasterSize", static_cast< float >( rasterW ), static_cast< float >( rasterH ) );
		tileShader.Set( "TileGridSize", static_cast< float >( tileGridW ), static_cast< float >( tileGridH ) );
		tileShader.Set( "TileSize", static_cast< float >( con.tileSize ) );
		quad.Draw();
	}

	//------------------------------------------------------------------
	// 3. Through the palette.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( quantBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		quantBuffer.ResizeViewPort();
		ScopedShaderBinding shader( quantizeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, downresBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, tileBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		quantizeShader.Set( "SourceTexture", 0 );
		quantizeShader.Set( "TileTexture", 1 );
		quantizeShader.Set( "MaxUV", 1.0f, 1.0f );
		quantizeShader.Set( "RasterSize", static_cast< float >( rasterW ), static_cast< float >( rasterH ) );
		quantizeShader.Set( "TileSize", static_cast< float >( con.tileSize ) );

		quantizeShader.Set( "PaletteMode", con.kind == kPaletteRGBBits ? 1.0f : 0.0f );
		const int bits = con.bitsPerChannel > 0
		                     ? con.bitsPerChannel
		                     : 1 + static_cast< int >( std::lround( params[ PT_COLOUR_DEPTH ] * 7.0f ) );
		quantizeShader.Set( "Bits", static_cast< float >( bits ) );

		//The master palette, uploaded whole. 64 vec3s is nothing per frame,
		//and uploading unconditionally means a console change needs no
		//bookkeeping.
		{
			float palette[ kMaxPaletteSize * 3 ] = {};
			const auto* store = paletteStore();
			for( int i = 0; i < con.paletteCount && i < kMaxPaletteSize; ++i )
			{
				palette[ i * 3 + 0 ] = store[ con.paletteFirst + i ][ 0 ] / 255.0f;
				palette[ i * 3 + 1 ] = store[ con.paletteFirst + i ][ 1 ] / 255.0f;
				palette[ i * 3 + 2 ] = store[ con.paletteFirst + i ][ 2 ] / 255.0f;
			}
			glUniform3fv( quantizeShader.FindUniform( "Palette" ), kMaxPaletteSize, palette );
			quantizeShader.Set( "PaletteCount", std::max( con.paletteCount, 1 ) );
		}

		quantizeShader.Set( "Dither", params[ PT_DITHER ] );
		quantizeShader.Set( "Clash", params[ PT_CLASH ] );
		quantizeShader.Set( "PaletteGlitch", params[ PT_PALETTE_GLITCH ] );
		quantizeShader.Set( "Garbage", params[ PT_GARBAGE ] );
		quantizeShader.Set( "GlitchKey", glitchKey );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
	}

	//------------------------------------------------------------------
	// 4. Back up to the composition, damaged on the way.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, quantBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE0 );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		displayShader.Set( "QuantTexture", 0 );
		displayShader.Set( "InputTexture", 1 );
		displayShader.Set( "MaxUV", 1.0f, 1.0f );
		displayShader.Set( "InputMaxUV", maxCoords.s, maxCoords.t );
		displayShader.Set( "RasterSize", static_cast< float >( rasterW ), static_cast< float >( rasterH ) );
		displayShader.Set( "TileSize", static_cast< float >( con.tileSize ) );
		displayShader.Set( "OutputSize", outputWidth, outputHeight );

		displayShader.Set( "Wave", params[ PT_WAVE ] );
		displayShader.Set( "Shake", params[ PT_SHAKE ] );
		displayShader.Set( "LineGlitch", params[ PT_LINE_GLITCH ] );
		displayShader.Set( "BlockGlitch", params[ PT_BLOCK_GLITCH ] );
		displayShader.Set( "GlitchKey", glitchKey );
		displayShader.Set( "Time", time );

		displayShader.Set( "Grid", params[ PT_GRID ] );
		displayShader.Set( "Mix", params[ PT_MIX ] );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

void NESolume::releaseBuffers()
{
	downresBuffer.Destroy();
	tileBuffer.Destroy();
	quantBuffer.Destroy();
}

FFResult NESolume::DeInitGL()
{
	downresShader.FreeGLResources();
	tileShader.FreeGLResources();
	quantizeShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();
	releaseBuffers();

	return FF_SUCCESS;
}

FFResult NESolume::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters —
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void NESolume::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float NESolume::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

FFResult NESolume::SetTextParameter( unsigned int index, const char* )
{
	// The About text line is display-only, but the SDK's FF_INSTANTIATE_GL
	// pushes every declared default into a fresh instance — including this
	// one, through here — and destroys the instance on the first FF_FAIL. The
	// base SetTextParameter returns FF_FAIL, so without this the plugin
	// silently fails to load in Resolume. See the fleet's instantiate-sweep
	// trap.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, nullptr );
}

char* NESolume::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}
