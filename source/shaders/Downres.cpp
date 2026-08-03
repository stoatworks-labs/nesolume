#include "../Shaders.h"

namespace nesolume::shaders
{
/// Full-resolution RGB down to the console's raster.
///
/// A box filter the width of one destination pixel, not a point sample. Point
/// sampling a 1080p frame onto a 240-line raster keeps one row in four and a
/// half and throws the rest away, so thin detail flickers in and out as it
/// drifts across the kept rows. A console never had that problem -- its
/// picture was *authored* at the raster -- and an emulator-style average is
/// the closest a downsampled photograph gets to looking authored.
const char* const kDownresFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	//One tap per source texel covered, capped so a 4K or 8K composition costs a
	//bounded amount. The cap only bites past an 8:1 reduction, and this pass
	//runs at the console raster, so even the worst case is a few million
	//fetches.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 8.0 ) ) );
	vec2 texel = MaxUV / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			//Spread the taps evenly across this destination pixel's footprint.
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, uv + f * ratio * texel );
		}
	}

	vec4 color = sum / float( taps.x * taps.y );

	//Everything downstream works in straight colour. The quantiser compares
	//colours, and a premultiplied pixel that is dark only because it is
	//transparent would otherwise be matched against the palette as a
	//legitimately dark pixel.
	if( color.a > 0.0 )
		color.rgb /= color.a;

	fragColor = color;
}
)";
} // namespace nesolume::shaders
