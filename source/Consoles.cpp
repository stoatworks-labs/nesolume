#include "Consoles.h"

namespace nesolume
{
namespace
{
/**
    The palette store. Order matters: each console's spec indexes into this
    with paletteFirst/paletteCount, so inserting entries mid-table means
    updating every offset after the insertion point.

    Sources, for the day someone disputes a colour:

    - Game Boy: the four reflective LCD greens everyone agrees on, darkest
      first. The real DMG panel had no colours at all, of course -- these are
      the conventional readings of its four grey levels through the green
      filter.
    - NES: the 2C02 NTSC palette as tabulated on the NesDev wiki. The PPU
      never stored RGB; it generated NTSC voltages directly, so every RGB
      rendering of it is somebody's decode. This is the decode everybody
      cites. Entries 0D-0F and the mirrored blacks are kept so the indices
      line up with the hardware's -- a nearest-colour search does not care
      about duplicates.
    - ZX Spectrum: 15 colours -- 8 basic at the 0xD7 level, 7 bright at 0xFF.
      Bright black was the same black, which is why it is not here twice.
    - Commodore 64: Pepto's measured VIC-II palette, the community standard.
*/
const unsigned char kStore[][ 3 ] = {
	// Game Boy (DMG), darkest to lightest                          [0..3]
	{ 15, 56, 15 }, { 48, 98, 48 }, { 139, 172, 15 }, { 155, 188, 15 },

	// NES 2C02, indices 0x00-0x3F                                  [4..67]
	{ 84, 84, 84 }, { 0, 30, 116 }, { 8, 16, 144 }, { 48, 0, 136 },
	{ 68, 0, 100 }, { 92, 0, 48 }, { 84, 4, 0 }, { 60, 24, 0 },
	{ 32, 42, 0 }, { 8, 58, 0 }, { 0, 64, 0 }, { 0, 60, 0 },
	{ 0, 50, 60 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
	{ 152, 150, 152 }, { 8, 76, 196 }, { 48, 50, 236 }, { 92, 30, 228 },
	{ 136, 20, 176 }, { 160, 20, 100 }, { 152, 34, 32 }, { 120, 60, 0 },
	{ 84, 90, 0 }, { 40, 114, 0 }, { 8, 124, 0 }, { 0, 118, 40 },
	{ 0, 102, 120 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
	{ 236, 238, 236 }, { 76, 154, 236 }, { 120, 124, 236 }, { 176, 98, 236 },
	{ 228, 84, 236 }, { 236, 88, 180 }, { 236, 106, 100 }, { 212, 136, 32 },
	{ 160, 170, 0 }, { 116, 196, 0 }, { 76, 208, 32 }, { 56, 204, 108 },
	{ 56, 180, 204 }, { 60, 60, 60 }, { 0, 0, 0 }, { 0, 0, 0 },
	{ 236, 238, 236 }, { 168, 204, 236 }, { 188, 188, 236 }, { 212, 178, 236 },
	{ 236, 174, 236 }, { 236, 174, 212 }, { 236, 180, 176 }, { 228, 196, 144 },
	{ 204, 210, 120 }, { 180, 222, 120 }, { 168, 226, 144 }, { 152, 226, 180 },
	{ 160, 214, 228 }, { 160, 162, 160 }, { 0, 0, 0 }, { 0, 0, 0 },

	// ZX Spectrum                                                  [68..82]
	{ 0, 0, 0 }, { 0, 0, 215 }, { 215, 0, 0 }, { 215, 0, 215 },
	{ 0, 215, 0 }, { 0, 215, 215 }, { 215, 215, 0 }, { 215, 215, 215 },
	{ 0, 0, 255 }, { 255, 0, 0 }, { 255, 0, 255 }, { 0, 255, 0 },
	{ 0, 255, 255 }, { 255, 255, 0 }, { 255, 255, 255 },

	// Commodore 64 (Pepto)                                         [83..98]
	{ 0, 0, 0 }, { 255, 255, 255 }, { 136, 57, 50 }, { 103, 182, 189 },
	{ 139, 63, 150 }, { 85, 160, 73 }, { 64, 49, 141 }, { 191, 206, 114 },
	{ 139, 84, 41 }, { 87, 66, 0 }, { 184, 105, 98 }, { 80, 80, 80 },
	{ 120, 120, 120 }, { 148, 224, 122 }, { 120, 105, 196 }, { 159, 159, 159 },
};

/**
    The machines, in the order the dropdown shows them: Custom first, then
    roughly by generation. Tile sizes are the *attribute* granularity -- the
    area forced to share colours -- not the character size. On the NES that is
    the 16x16 attribute-table area (the famous cause of its background colour
    bleed), not the 8x8 tile. On the Spectrum and C64 the 8x8 cell is both.

    The 16-bit and 32-bit machines had per-tile palette selection too, but
    with 16-colour sub-palettes the clash was mild; their 8 here gives the
    Attribute Clash control something honest to do rather than nothing.
*/
const ConsoleSpec kConsoles[] = {
	{ "Custom", 240, 8, kPaletteRGBBits, 0, 0, 0 },
	{ "Game Boy", 144, 8, kPaletteFixed, 0, 4, 0 },
	{ "NES", 240, 16, kPaletteFixed, 4, 64, 0 },
	{ "ZX Spectrum", 192, 8, kPaletteFixed, 68, 15, 0 },
	{ "Commodore 64", 200, 8, kPaletteFixed, 83, 16, 0 },
	{ "Master System", 192, 8, kPaletteRGBBits, 0, 0, 2 },
	{ "Mega Drive", 224, 8, kPaletteRGBBits, 0, 0, 3 },
	{ "SNES", 224, 8, kPaletteRGBBits, 0, 0, 5 },
	{ "PlayStation", 240, 8, kPaletteRGBBits, 0, 0, 5 },
};

constexpr int kConsoleCount = int( sizeof( kConsoles ) / sizeof( kConsoles[ 0 ] ) );

static_assert( sizeof( kStore ) / sizeof( kStore[ 0 ] ) == 99, "palette store offsets are hand-counted; recheck every paletteFirst above" );
} // namespace

int consoleCount()
{
	return kConsoleCount;
}

const ConsoleSpec& console( int index )
{
	if( index < 0 || index >= kConsoleCount )
		index = 0;
	return kConsoles[ index ];
}

const unsigned char ( *paletteStore() )[ 3 ]
{
	return kStore;
}

} // namespace nesolume
