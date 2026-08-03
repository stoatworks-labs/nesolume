#include "../Shaders.h"

namespace nesolume::shaders
{
/// The palette: where a true-colour pixel becomes a console colour.
///
/// Order of operations matters and is the point of the file:
///
///   1. Clash. The pixel's chroma is pulled toward its attribute cell's
///      mean chroma; luminance stays its own. That is the shape of the real
///      constraint -- a cell's sub-palette was usually a ramp of one hue, so
///      within a cell you could have your own brightness but not your own
///      colour. At Clash 0 the pull is off; at 1 the cell speaks with one
///      voice and two objects crossing it clash exactly the way they did.
///   2. Corruption. Palette-glitched cells have their channels swizzled and
///      garbage cells become noise -- *before* quantisation, so a corrupted
///      pixel still comes out as a legal colour of the selected console. A
///      real glitch scrambled indices into CRAM, not the DAC; nothing a
///      broken cartridge showed was ever outside the palette.
///   3. Dither, as an ordered 4x4 Bayer offset scaled to the quantisation
///      step, then the choice: nearest entry of the master palette, or
///      per-channel truncation to the DAC's bit depth.
///
/// Alpha is snapped to one bit. Console video had transparency or it did
/// not; a soft edge is a modern luxury the layer above can add back with Mix.
const char* const kQuantizeFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform sampler2D TileTexture;
uniform vec2 MaxUV;
uniform vec2 RasterSize;
uniform float TileSize;

uniform float PaletteMode;//0 = fixed master palette, 1 = n bits per channel
uniform float Bits;
uniform vec3 Palette[ 64 ];
uniform int PaletteCount;

uniform float Dither;
uniform float Clash;
uniform float PaletteGlitch;
uniform float Garbage;
uniform float GlitchKey;

in vec2 uv;

out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.299, 0.587, 0.114 );

const float kBayer[ 16 ] = float[ 16 ](
	 0.0,  8.0,  2.0, 10.0,
	12.0,  4.0, 14.0,  6.0,
	 3.0, 11.0,  1.0,  9.0,
	15.0,  7.0, 13.0,  5.0 );

float hash( vec2 p )
{
	return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

void main()
{
	ivec2 px = ivec2( clamp( uv * RasterSize, vec2( 0.0 ), RasterSize - 1.0 ) );
	vec4 source = texelFetch( SourceTexture, px, 0 );

	ivec2 cell = ivec2( vec2( px ) / TileSize );
	vec3 cellMean = texelFetch( TileTexture, cell, 0 ).rgb;

	//--- 1. Clash: own luminance, the cell's chroma. -----------------------
	float luma = dot( source.rgb, kLumaWeights );
	vec3 chroma = source.rgb - luma;
	vec3 cellChroma = cellMean - dot( cellMean, kLumaWeights );
	vec3 color = luma + mix( chroma, cellChroma, Clash );

	//--- 2. Corruption, while the colour is still an index-to-be. ----------
	vec2 cellId = vec2( cell );
	float corrupt = hash( cellId + vec2( GlitchKey * 13.7, GlitchKey ) );
	if( corrupt < PaletteGlitch * 0.5 )
	{
		//Three flavours of wrong CRAM, picked per cell: two channel rotations
		//and an inversion.
		float flavour = hash( cellId + vec2( 5.0, GlitchKey ) );
		if( flavour < 0.4 )
			color = color.brg;
		else if( flavour < 0.8 )
			color = color.gbr;
		else
			color = vec3( 1.0 ) - color;
	}

	float junk = hash( cellId + vec2( GlitchKey, 71.3 ) );
	if( junk < Garbage * 0.35 )
	{
		//A tile pointer into memory that never held graphics: per-pixel noise,
		//stable within one glitch interval, quantised like everything else.
		vec2 noiseSeed = vec2( px ) + vec2( GlitchKey * 7.1, GlitchKey * 3.3 );
		color = vec3( hash( noiseSeed ), hash( noiseSeed + 19.7 ), hash( noiseSeed + 41.1 ) );
	}

	//--- 3. Dither, then the choice. ---------------------------------------
	float bayer = ( kBayer[ ( px.y % 4 ) * 4 + ( px.x % 4 ) ] + 0.5 ) / 16.0 - 0.5;

	vec3 quantised;
	if( PaletteMode > 0.5 )
	{
		float levels = exp2( Bits ) - 1.0;
		vec3 dithered = clamp( color + bayer * Dither / levels, 0.0, 1.0 );
		quantised = floor( dithered * levels + 0.5 ) / levels;
	}
	else
	{
		//The dither amplitude for a master palette: there is no uniform step,
		//so use a fraction of the range that in practice spans neighbouring
		//ramp entries on all four fixed palettes.
		vec3 dithered = clamp( color + bayer * Dither * 0.22, 0.0, 1.0 );

		float best = 1e9;
		quantised = Palette[ 0 ];
		for( int i = 0; i < PaletteCount; ++i )
		{
			vec3 d = dithered - Palette[ i ];
			float dist = dot( d * d, kLumaWeights );
			if( dist < best )
			{
				best = dist;
				quantised = Palette[ i ];
			}
		}
	}

	//--- Alpha is a bit. ----------------------------------------------------
	fragColor = vec4( quantised, source.a > 0.5 ? 1.0 : 0.0 );
}
)";
} // namespace nesolume::shaders
