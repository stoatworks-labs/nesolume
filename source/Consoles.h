#pragma once

/**
    The consoles NESolume can pretend to be.

    Each entry describes a machine's *display hardware constraints*, not a
    look: how many scanlines it drew, how big an attribute cell was, and what
    colours it could physically produce. Everything the effect does is derived
    from these three facts, which is why a new console is a table row here and
    nothing anywhere else.

    Two kinds of colour system exist and the distinction runs through the
    whole quantise stage:

      Fixed    the machine had a master palette burned into silicon (NES,
               Game Boy) or defined by the video chip's TTL levels (Spectrum,
               C64). Colour choice is a nearest-entry search in that table.
      RGBBits  the machine had an RGB DAC of n bits per channel (Master
               System 2, Mega Drive 3, SNES/PlayStation 5). Colour choice is
               per-channel quantisation; no search, no table.

    The raster is described by its height only. These machines drew a fixed
    number of lines; how wide the picture was depended on what displayed it.
    NESolume follows suit: the line count is the console's, and the width
    follows the composition's aspect so pixels stay square on screen.
*/
namespace nesolume
{
enum PaletteKind
{
	kPaletteFixed   = 0,
	kPaletteRGBBits = 1,
};

struct ConsoleSpec
{
	const char* name;
	int rasterHeight;  //!< active scanlines at native size
	int tileSize;      //!< attribute cell in raster pixels: the area that shares a palette
	PaletteKind kind;
	int paletteFirst;  //!< Fixed: index of the first entry in the palette store
	int paletteCount;  //!< Fixed: number of entries
	int bitsPerChannel;//!< RGBBits: DAC depth. 0 means "take it from the Colour Depth slider".
};

int consoleCount();
const ConsoleSpec& console( int index );

/// The master palettes, one after another, as 8-bit RGB triples. Index into
/// this with a ConsoleSpec's paletteFirst/paletteCount.
const unsigned char ( *paletteStore() )[ 3 ];

/// The largest paletteCount any console declares. The quantise shader sizes
/// its uniform array with this, and the upload asserts against it.
constexpr int kMaxPaletteSize = 64;

} // namespace nesolume
