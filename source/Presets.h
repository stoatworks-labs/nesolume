#pragma once

/**
    Factory presets: named machines (and named failures) an operator can
    reach in one gesture.

    Each entry is a *machine in a state* — a Game Boy on a train, a Spectrum
    doing its best, a cartridge that needs taking out and blowing on — not a
    random collection of slider positions. The controls this plugin exposes
    are hardware constraints and hardware faults, so a coherent look is a
    coherent story about one machine.

    The values live in the host-facing 0..1 parameter space. Element 0 of the
    host-facing dropdown is "Custom" and is not in this table: it means "the
    sliders are the truth".

    A preset covers the console, the picture controls and the faults. It does
    not cover Mix — how much of the effect is in the programme is the
    operator's business, not the preset's.
*/

namespace nesolume
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and static_asserts against kParamCount, so the
/// two lists cannot drift apart silently.
enum Param
{
	kConsole,
	kPixelSize,
	kColourDepth,
	kDither,
	kClash,
	kGrid,
	kWave,
	kShake,
	kBlockGlitch,
	kLineGlitch,
	kPaletteGlitch,
	kGarbage,
	kGlitchRate,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Console is an element index: 0 Custom / 1 Game Boy / 2 NES / 3 ZX Spectrum /
// 4 Commodore 64 / 5 Master System / 6 Mega Drive / 7 SNES / 8 PlayStation.
// Pixel Size sits at native on 0.5.
inline constexpr Preset kPresets[] = {
	// A DMG in decent light: the four greens, the cell grid you can nearly
	// count, nothing wrong with it at all.
	{ "Handheld",
	  { /*Con*/ 1, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.55f, /*Clash*/ 0.2f, /*Grid*/ 0.55f,
	    /*Wave*/ 0.0f, /*Shake*/ 0.0f, /*Block*/ 0.0f, /*Line*/ 0.0f, /*PalG*/ 0.0f, /*Junk*/ 0.0f, /*Rate*/ 0.3f } },

	// The front-room NES, working as designed — which includes the attribute
	// areas bleeding colour where sprites cross them.
	{ "Front Room NES",
	  { /*Con*/ 2, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.3f, /*Clash*/ 0.7f, /*Grid*/ 0.0f,
	    /*Wave*/ 0.0f, /*Shake*/ 0.0f, /*Block*/ 0.0f, /*Line*/ 0.0f, /*PalG*/ 0.0f, /*Junk*/ 0.0f, /*Rate*/ 0.3f } },

	// The Spectrum at full strictness: one cell, one pair of colours, and
	// everything crossing a boundary pays for it. The machine's signature.
	{ "Attribute Clash",
	  { /*Con*/ 3, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.5f, /*Clash*/ 1.0f, /*Grid*/ 0.0f,
	    /*Wave*/ 0.0f, /*Shake*/ 0.0f, /*Block*/ 0.0f, /*Line*/ 0.0f, /*PalG*/ 0.0f, /*Junk*/ 0.0f, /*Rate*/ 0.3f } },

	// A C64 on a portable telly: Pepto's browns doing their patient work.
	{ "Bedroom Micro",
	  { /*Con*/ 4, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.55f, /*Clash*/ 0.6f, /*Grid*/ 0.15f,
	    /*Wave*/ 0.0f, /*Shake*/ 0.0f, /*Block*/ 0.0f, /*Line*/ 0.0f, /*PalG*/ 0.0f, /*Junk*/ 0.0f, /*Rate*/ 0.3f } },

	// A Mega Drive with its 512 colours and its shameless dithered
	// gradients: the 16-bit look at its most confident.
	{ "16-bit Console",
	  { /*Con*/ 6, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.45f, /*Clash*/ 0.25f, /*Grid*/ 0.0f,
	    /*Wave*/ 0.0f, /*Shake*/ 0.0f, /*Block*/ 0.0f, /*Line*/ 0.0f, /*PalG*/ 0.0f, /*Junk*/ 0.0f, /*Rate*/ 0.3f } },

	// A cartridge that wants reseating: mostly fine, then a handful of tiles
	// and palette entries come back wrong, on and off.
	{ "Dirty Cartridge",
	  { /*Con*/ 2, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.35f, /*Clash*/ 0.7f, /*Grid*/ 0.0f,
	    /*Wave*/ 0.0f, /*Shake*/ 0.1f, /*Block*/ 0.45f, /*Line*/ 0.2f, /*PalG*/ 0.35f, /*Junk*/ 0.3f, /*Rate*/ 0.35f } },

	// Video memory that has stopped being told the truth: blocks land in the
	// wrong places, palettes rotate, whole bands tear sideways.
	{ "Corrupted VRAM",
	  { /*Con*/ 6, /*Px*/ 0.5f, /*Depth*/ 0.4f, /*Dither*/ 0.45f, /*Clash*/ 0.3f, /*Grid*/ 0.0f,
	    /*Wave*/ 0.1f, /*Shake*/ 0.2f, /*Block*/ 0.7f, /*Line*/ 0.5f, /*PalG*/ 0.6f, /*Junk*/ 0.5f, /*Rate*/ 0.55f } },

	// Level 256. The machine has left the map and is drawing its own working
	// memory; the picture is still, technically, being displayed.
	{ "Kill Screen",
	  { /*Con*/ 2, /*Px*/ 0.55f, /*Depth*/ 0.4f, /*Dither*/ 0.3f, /*Clash*/ 0.8f, /*Grid*/ 0.0f,
	    /*Wave*/ 0.35f, /*Shake*/ 0.5f, /*Block*/ 0.6f, /*Line*/ 0.8f, /*PalG*/ 0.5f, /*Junk*/ 0.6f, /*Rate*/ 0.75f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace nesolume
