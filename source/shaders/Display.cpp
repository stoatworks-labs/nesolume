#include "../Shaders.h"

namespace nesolume::shaders
{
/// Raster to composition, and everything that goes wrong on the way out.
///
/// The displacements -- wave, shake, torn lines, displaced blocks -- are all
/// scroll-register damage: they move *which raster pixel is shown where*, in
/// whole raster pixels (whole tiles, for blocks), and they wrap around the
/// raster the way a scroll register wraps. Nothing here changes a colour;
/// after this pass the picture is still made of exactly the texels the
/// quantiser produced, just fetched from the wrong places. That is what keeps
/// heavy glitch settings looking like a machine failing rather than a video
/// effect succeeding.
///
/// The grid is the one cosmetic: the dark boundary between fat pixels that an
/// LCD's cell gaps (or a sharp upscale of one) put there. It fades out when a
/// raster pixel is too small on screen to have a boundary worth drawing.
const char* const kDisplayFragment = R"(#version 410 core
uniform sampler2D QuantTexture;
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputMaxUV;
uniform vec2 RasterSize;
uniform float TileSize;
uniform vec2 OutputSize;

uniform float Wave;
uniform float Shake;
uniform float LineGlitch;
uniform float BlockGlitch;
uniform float GlitchKey;
uniform float Time;

uniform float Grid;
uniform float Mix;

in vec2 uv;

out vec4 fragColor;

float hash( vec2 p )
{
	return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

vec2 hash2( vec2 p )
{
	return vec2( hash( p ), hash( p + 19.19 ) );
}

void main()
{
	vec2 rc = uv * RasterSize;

	//--- Wave: a sine on the horizontal scroll, per line. -------------------
	//Squared so the first half of the control is subtle. Snapped to whole
	//pixels: fine scroll had no fractional bits.
	float waveAmp = Wave * Wave * RasterSize.x * 0.12;
	rc.x += floor( sin( rc.y / RasterSize.y * 18.85 + Time * 2.1 ) * waveAmp + 0.5 );

	//--- Shake: the whole frame knocked off its sync. -----------------------
	vec2 knock = ( hash2( vec2( GlitchKey + 7.0, GlitchKey * 3.1 ) ) - 0.5 ) * 2.0;
	rc += floor( knock * Shake * Shake * RasterSize * 0.10 + 0.5 );

	//--- Torn lines: bands whose scroll register read back garbage. ---------
	float band = floor( rc.y / 4.0 );
	if( hash( vec2( band, GlitchKey ) ) < LineGlitch * 0.4 )
	{
		float tear = ( hash( vec2( band + 31.0, GlitchKey ) ) - 0.5 ) * 2.0;
		rc.x += floor( tear * LineGlitch * RasterSize.x * 0.3 + 0.5 );
	}

	//--- Displaced blocks: tile pointers reading the wrong address. ---------
	vec2 tile = floor( rc / TileSize );
	if( hash( tile + vec2( GlitchKey * 5.3, GlitchKey ) ) < BlockGlitch * 0.35 )
	{
		vec2 jump = hash2( tile + vec2( 43.7, GlitchKey ) ) - 0.5;
		rc += floor( jump * BlockGlitch * 12.0 ) * TileSize;
	}

	//--- Fetch, wrapped like a scroll register. -----------------------------
	rc = mod( rc, RasterSize );
	ivec2 ip = ivec2( clamp( rc, vec2( 0.0 ), RasterSize - 1.0 ) );
	vec4 quantised = texelFetch( QuantTexture, ip, 0 );

	//--- The grid between fat pixels. ---------------------------------------
	//w is raster pixels per output pixel: above ~0.4 a raster pixel is only a
	//couple of output pixels and a boundary line would just darken the whole
	//picture, so the grid fades itself out.
	float w = max( RasterSize.x / OutputSize.x, RasterSize.y / OutputSize.y );
	vec2 toEdge = min( fract( rc ), 1.0 - fract( rc ) );
	float edgeDistance = min( toEdge.x, toEdge.y );
	float lineHalf = 0.05 + 0.10 * Grid;
	float onLine = 1.0 - smoothstep( lineHalf, lineHalf + w, edgeDistance );
	float resolvable = smoothstep( 0.45, 0.2, w );
	quantised.rgb *= 1.0 - Grid * onLine * resolvable * 0.85;

	//--- Back to premultiplied, and the operator's mix. ---------------------
	vec4 effect = vec4( quantised.rgb * quantised.a, quantised.a );
	vec4 original = texture( InputTexture, uv * InputMaxUV );
	fragColor = mix( original, effect, Mix );
}
)";
} // namespace nesolume::shaders
