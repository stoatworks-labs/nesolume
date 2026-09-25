#pragma once

/**
    The Amiga's display, as constraints: the colour registers, the bitplane
    modes that read them, and the interlaced fields.

    Everything here is CPU code with no GL in it, so the harness can call the
    encoder directly on short lines and compare it with an exhaustive search,
    and the OpenFX build could link it without a context.

    The facts (Amiga Hardware Reference Manual, 3rd edition, chapter 3, and
    Appendix A's COLORxx; the same ones copperlist emulates):

    - **Colour registers are 12 bits**: four bits per gun, 4,096 colours. Every
      colour the machine shows is one of those, so a gradient bands in sixteen
      steps per gun whatever mode is on.
    - **Low res is 320 pixels wide**, 256 lines PAL or 200 NTSC, non-interlaced.
      **High res is 640**, and the OCS chipset fetches at most four bitplanes
      there: sixteen colours, and neither of the six-plane modes.
    - **Five bitplanes = 32 registers.** A pixel's five bits index them
      straight; register 0 is the background.
    - **Extra Half-Brite** (six planes, low res, not HAM): a pixel value of
      32..63 shows register n-32 with each gun shifted right one bit. Half
      brightness is `v >> 1`, truncating -- 15 becomes 7, not 7.5.
    - **Hold-and-modify** (six planes, low res, HOMOD): the top two bits say
      what the low four mean. 00: a palette index into the first sixteen
      registers. 01: keep the previous pixel's red and green, set BLUE to the
      four bits. 10: set RED. 11: set GREEN. At the left edge of every line the
      "previous pixel" is the background colour, register 0.
    - **Interlace** draws 512 (PAL) or 400 (NTSC) lines as two fields of
      alternate lines, 50 or 60 fields a second.

    The field rate is 50 and 60 exactly, not the 50.08 / 60.05 the colour
    clocks give -- the same decision copperlist records, because a host's
    frame rate is nominal and 50.08 against a 50 fps composition would slip a
    field every twelve and a half seconds.
*/

#include <cstdint>
#include <vector>

namespace nesolume::amiga
{
/// One colour register: four bits per gun, 0..15.
struct Colour12
{
	uint8_t r = 0, g = 0, b = 0;

	bool operator==( const Colour12& o ) const { return r == o.r && g == o.g && b == o.b; }
	bool operator!=( const Colour12& o ) const { return !( *this == o ); }
};

inline int index12( const Colour12& c ) { return ( c.r << 8 ) | ( c.g << 4 ) | c.b; }
inline Colour12 fromIndex12( int i ) { return { uint8_t( ( i >> 8 ) & 15 ), uint8_t( ( i >> 4 ) & 15 ), uint8_t( i & 15 ) }; }

/// A gun's four bits as the 8-bit value the DAC puts out: 0x0..0xF -> 0x00..0xFF.
inline int to8( int v4 ) { return v4 * 17; }

/// Half-brite: each gun shifted right one bit, the way Denise does it.
inline Colour12 halfBrite( const Colour12& c ) { return { uint8_t( c.r >> 1 ), uint8_t( c.g >> 1 ), uint8_t( c.b >> 1 ) }; }

//---------------------------------------------------------------------------
// The plugin's options, in the order the host shows them. APPEND ONLY: saved
// compositions store the element value.
//---------------------------------------------------------------------------
enum Mode
{
	kModeOCS  = 0,///< 32 colours (16 in high res)
	kModeEHB  = 1,///< 32 + 32 at half brightness
	kModeHAM6 = 2,///< hold-and-modify
	kModeCount
};
const char* modeName( int mode );

struct Screen
{
	const char* name;
	int width;    ///< pixels per line
	int lines;    ///< non-interlaced; interlace doubles it
	int fieldHz;  ///< fields per second
	bool hires;   ///< four bitplanes at most: 16 colours, no EHB, no HAM
};
enum ScreenId
{
	kPalLowRes   = 0,
	kNtscLowRes  = 1,
	kPalHighRes  = 2,
	kNtscHighRes = 3,
	kScreenCount
};
const Screen& screen( int id );

enum PaletteChoice
{
	kPalettePerFrame = 0,///< k-means in 12-bit space, seeded from the last frame
	kPaletteFixed    = 1,///< one content-independent palette per mode
	kPaletteChoiceCount
};
const char* paletteChoiceName( int choice );

/// How many colour registers a mode on a screen actually has to choose. HAM
/// uses the first 16, EHB chooses 32 and shows 64, high res has 16 for all.
int baseRegisterCount( int mode, bool hires );

/// The mode the chipset really runs: high res cannot fetch six planes, so it
/// is OCS whatever was asked for.
int effectiveMode( int mode, bool hires );

//---------------------------------------------------------------------------
// Fields.
//---------------------------------------------------------------------------
/// floor( t x rate ), in double. The 1e-6 of a field (20 ns of host time) is
/// there because t = k / rate, multiplied back, can land one ulp under k --
/// and floor would then show field k-1. Double, not float: Resolume's clock
/// has been measured at ~499 million ms, where a float resolves 0.03 s, which
/// is more than a field.
int64_t fieldIndex( double seconds, int fieldHz );

//---------------------------------------------------------------------------
// Palettes.
//---------------------------------------------------------------------------
/// The Fixed choice's registers for a mode on a screen, `baseRegisterCount`
/// of them, register 0 first.
std::vector< Colour12 > fixedPalette( int mode, bool hires );

/// Choose `count` colour registers for a picture: weighted k-means over the
/// picture's 12-bit histogram, so the clustering happens in the space the
/// registers live in, and each centre is snapped to 12 bits at the end.
/// `seeds`, when it has `count` entries, starts the clustering from the last
/// frame's registers so a moving picture's palette moves rather than jumps.
/// With `halfBriteTwins` the 32 centres are fitted together with their
/// half-bright twins (EHB), since a pixel may land on either.
///
/// Sorted by luma afterwards, darkest first: register 0 is the background --
/// and the colour every HAM line starts from.
///
/// `rgba` is 8-bit RGBA, `pixels` of them.
std::vector< Colour12 > choosePalette( const uint8_t* rgba, int pixels, int count, bool halfBriteTwins,
                                       const std::vector< Colour12 >& seeds );

/// What the display can show for a set of base registers: EHB adds the 32
/// half-bright twins after the 32 bases, everything else shows the bases.
std::vector< Colour12 > displayPalette( const std::vector< Colour12 >& base, int effectiveMode );

//---------------------------------------------------------------------------
// Hold-and-modify.
//---------------------------------------------------------------------------
/// The error the encoder minimises for one pixel, against an 8-bit target:
/// squared gun differences weighted 3:6:1, close to the luma weights the
/// quantise shader's nearest-colour search uses. Integer, so an optimum is
/// an exact number and two searches can be compared with ==.
constexpr int kWeightR = 3, kWeightG = 6, kWeightB = 1;
inline int32_t pixelError( const Colour12& c, const uint8_t* rgb )
{
	const int dr = to8( c.r ) - rgb[ 0 ];
	const int dg = to8( c.g ) - rgb[ 1 ];
	const int db = to8( c.b ) - rgb[ 2 ];
	return kWeightR * dr * dr + kWeightG * dg * dg + kWeightB * db * db;
}

/**
    The exact HAM6 encoder for one line.

    Viterbi over the previous pixel's colour -- 4,096 states -- choosing, per
    pixel, a palette register or a modify of one gun, and minimising the summed
    `pixelError`. The line starts from `palette[ 0 ]`.

    It does not store 4,096 costs per pixel. The only things a pixel's options
    read from the one before are the best cost over each gun with the other
    two fixed (a modify of red cannot see the old red), and the best cost of
    all (a palette register can follow anything). So the state is three
    16x16 tables and one number, and each step computes the next three tables
    from them in closed form -- ~7,000 integer operations a pixel instead of
    ~60,000, with the same optimum. `--ham-optimal` checks that optimum against
    an exhaustive search of every bitplane code.

    `channelsPerModify` is 1 on the hardware. 2 lets one pixel change two guns
    and exists for the harness's negative control: with it, a hard edge
    arrives in two pixels, and `--ham-edge` must fail.

    `rgb` is `width` packed 8-bit triples; `out` receives `width` colours.
    Returns the total error.
*/
class HamEncoder
{
public:
	int64_t encodeLine( const uint8_t* rgb, int width, const std::vector< Colour12 >& palette, Colour12* out,
	                    int channelsPerModify = 1 );

private:
	std::vector< int32_t > tables;//( width + 1 ) x kTableStride
	std::vector< uint8_t > inPalette;//4096
};

/// Is `line` something HAM6 can display? Every pixel is a palette register or
/// differs from the previous pixel (register 0 before the first) in at most
/// one gun. Written separately from the encoder, so a check can use it on the
/// plugin's output.
bool hamLegal( const Colour12* line, int width, const std::vector< Colour12 >& palette );

/// Encode a whole RGBA8 frame, line by line, on `threads` threads. Alpha is
/// passed through. `negativeChannels` is the negative-control knob above.
/// Returns the summed error.
int64_t encodeHamFrame( const uint8_t* rgbaIn, uint8_t* rgbaOut, int width, int height,
                        const std::vector< Colour12 >& palette, int threads, int negativeChannels = 1 );

} // namespace nesolume::amiga
