#include "../Shaders.h"

namespace nesolume::shaders
{
/// One texel per attribute cell: the cell's mean colour.
///
/// This is the summary the clash model works from. A real console assigned
/// each attribute cell one sub-palette, chosen by whoever drew the screen to
/// suit what the cell mostly contained; the mean is the honest automatic
/// stand-in for "what this cell mostly contains". The quantise stage pulls
/// every pixel's chroma toward its cell's mean before choosing a colour,
/// which is what makes two objects crossing a cell share colours they should
/// not -- attribute clash, arrived at rather than painted.
///
/// Runs at the tile grid, so the cost is a few hundred texels regardless of
/// composition size.
const char* const kTileFragment = R"(#version 410 core
uniform sampler2D SourceTexture;
uniform vec2 MaxUV;
uniform vec2 RasterSize;
uniform vec2 TileGridSize;
uniform float TileSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	//The raster-pixel origin of the cell this texel summarises.
	vec2 cellOrigin = floor( uv * TileGridSize ) * TileSize;

	//Up to 8x8 taps spread across the cell. An 8x8 cell is sampled exactly;
	//the NES's 16x16 attribute area is sampled every other pixel, which is
	//plenty for a mean.
	int taps = int( min( TileSize, 8.0 ) );

	vec3 sum = vec3( 0.0 );
	for( int y = 0; y < taps; ++y )
	{
		for( int x = 0; x < taps; ++x )
		{
			vec2 p = cellOrigin + ( vec2( x, y ) + 0.5 ) / float( taps ) * TileSize;
			p = min( p, RasterSize - 0.5 );//edge cells overhang the raster
			sum += texture( SourceTexture, p / RasterSize ).rgb;
		}
	}

	fragColor = vec4( sum / float( taps * taps ), 1.0 );
}
)";
} // namespace nesolume::shaders
