/**
    The Amiga checks: netest --ham-edge / --ham-optimal / --ehb / --palette /
    --lace / --negative / --names / --params / --ham-cost.

    Every rendered check drives the REAL NESolume class through its real
    ProcessOpenGL, and reads the property out of the picture it produced. The
    one exception is --ham-optimal, which is about the encoder as a function
    and calls the same `amiga::HamEncoder` the plugin calls.

    The source picture is given at the Amiga's own raster (320 x 256, or 512
    laced) and the output at whatever --size says: the plugin's downres reads
    the input's size, not the viewport's, so a source can be exact at the
    raster while the output is CI's 320x180. That is what lets a check about
    one raster line hold at any output size. See AGENTS.md, "Would this hold
    on another rasteriser, at another raster?".
*/

#include "NESolume.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using nesolume::amiga::Colour12;
namespace amiga = nesolume::amiga;

namespace
{
constexpr float kAmigaConsole = 9.0f;//appended at the end of the list

//---------------------------------------------------------------------------
/// One plugin instance, one input texture, one output framebuffer.
//---------------------------------------------------------------------------
struct Rig
{
	NESolume plugin;
	int inW, inH, outW, outH;
	GLuint inTex = 0, outTex = 0, fbo = 0;
	FFGLTextureStruct in{};
	FFGLTextureStruct* ins[ 1 ] = { &in };
	ProcessOpenGLStruct proc{};
	bool ok = false;

	Rig( int iw, int ih, int ow, int oh ) : inW( iw ), inH( ih ), outW( ow ), outH( oh )
	{
		glGenTextures( 1, &inTex );
		glBindTexture( GL_TEXTURE_2D, inTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, inW, inH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glGenTextures( 1, &outTex );
		glBindTexture( GL_TEXTURE_2D, outTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return;
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( outW ), static_cast< FFUInt32 >( outH ) };
		if( plugin.InitGL( &vp ) != FF_SUCCESS )
			return;
		in.Width = in.HardwareWidth = static_cast< FFUInt32 >( inW );
		in.Height = in.HardwareHeight = static_cast< FFUInt32 >( inH );
		in.Handle                     = inTex;
		proc.numInputTextures         = 1;
		proc.inputTextures            = ins;
		proc.HostFBO                  = fbo;
		plugin.SetClockScaleForTest( 1.0 );
		ok = true;
	}

	~Rig()
	{
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &outTex );
		glDeleteTextures( 1, &inTex );
	}

	int param( const std::string& name )
	{
		for( unsigned i = 0; i < plugin.GetNumParams(); ++i )
			if( name == plugin.GetParamName( i ) )
				return static_cast< int >( i );
		std::fprintf( stderr, "netest: no parameter '%s'\n", name.c_str() );
		std::exit( 2 );
	}

	void set( const std::string& name, float v ) { plugin.SetFloatParameter( static_cast< unsigned >( param( name ) ), v ); }

	/// Top-down RGBA in.
	void upload( const std::vector< uint8_t >& topDown )
	{
		std::vector< uint8_t > flipped( topDown.size() );
		const size_t row = static_cast< size_t >( inW ) * 4;
		for( int y = 0; y < inH; ++y )
			std::memcpy( flipped.data() + y * row, topDown.data() + ( inH - 1 - y ) * row, row );
		glBindTexture( GL_TEXTURE_2D, inTex );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, inW, inH, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// Top-down RGBA out.
	std::vector< uint8_t > render( double seconds )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( seconds );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, outW, outH );
		glClearColor( 0, 0, 0, 0 );
		glClear( GL_COLOR_BUFFER_BIT );
		plugin.ProcessOpenGL( &proc );
		std::vector< uint8_t > px( static_cast< size_t >( outW ) * outH * 4 ), top( px.size() );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, outW, outH, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
		const size_t row = static_cast< size_t >( outW ) * 4;
		for( int y = 0; y < outH; ++y )
			std::memcpy( top.data() + y * row, px.data() + ( outH - 1 - y ) * row, row );
		return top;
	}

	/// The output pixel at the centre of raster pixel (i, j from the top), for
	/// a raster of rw x rh stretched over the output.
	const uint8_t* rasterPixel( const std::vector< uint8_t >& out, int rw, int rh, int i, int j ) const
	{
		const int x = std::min( outW - 1, static_cast< int >( ( i + 0.5 ) * outW / rw ) );
		const int y = std::min( outH - 1, static_cast< int >( ( j + 0.5 ) * outH / rh ) );
		return out.data() + ( static_cast< size_t >( y ) * outW + x ) * 4;
	}
};

/// A clean Amiga: no dither, no faults, no grid, fully mixed.
void amigaBaseline( Rig& rig, int mode, int screen, int palette )
{
	rig.set( "Console", kAmigaConsole );
	rig.set( "Amiga Mode", static_cast< float >( mode ) );
	rig.set( "Screen Mode", static_cast< float >( screen ) );
	rig.set( "Amiga Palette", static_cast< float >( palette ) );
	rig.set( "Interlace", 0.0f );
	rig.set( "Flicker Fixer", 0.0f );
	for( const char* n : { "Dither", "Pixel Grid", "Wave", "Shake", "Block Glitch", "Line Glitch", "Palette Glitch",
	                       "Garbage" } )
		rig.set( n, 0.0f );
	rig.set( "Pixel Size", 0.5f );
	rig.set( "Mix", 1.0f );
}

bool is12( const uint8_t* p )
{
	return p[ 0 ] % 17 == 0 && p[ 1 ] % 17 == 0 && p[ 2 ] % 17 == 0;
}

Colour12 as12( const uint8_t* p )
{
	return { uint8_t( p[ 0 ] / 17 ), uint8_t( p[ 1 ] / 17 ), uint8_t( p[ 2 ] / 17 ) };
}

void fill( std::vector< uint8_t >& img, int w, int x0, int y0, int x1, int y1, Colour12 c )
{
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			uint8_t* p = img.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
			p[ 0 ]     = uint8_t( amiga::to8( c.r ) );
			p[ 1 ]     = uint8_t( amiga::to8( c.g ) );
			p[ 2 ]     = uint8_t( amiga::to8( c.b ) );
			p[ 3 ]     = 255;
		}
}

struct Report
{
	int failures = 0;
	bool quiet   = false;
	void line( const char* group, const std::string& what, bool ok, const std::string& detail = "" )
	{
		if( !quiet )
			std::printf( "%-9s %-58s %s%s%s\n", group, what.c_str(), ok ? "ok" : "FAILED", detail.empty() ? "" : "  ",
			             detail.c_str() );
		if( !ok )
			++failures;
	}
};

std::string fmt( const char* f, double a, double b = 0, double c = 0 )
{
	char buf[ 256 ];
	std::snprintf( buf, sizeof buf, f, a, b, c );
	return buf;
}

//---------------------------------------------------------------------------
// --ham-edge
//---------------------------------------------------------------------------
/// Measure how many pixels a hard edge from `a` to `b` takes to arrive on
/// line 128 of a HAM6 picture over the Fixed (grey) registers. Returns the
/// arrival in pixels (first pixel off `a` to first pixel on `b`, inclusive),
/// and the first departure in `departure`; -1 if `b` never arrives.
int measureEdge( int outW, int outH, Colour12 a, Colour12 b, NESolume::Negative neg, int& departure,
                 bool& cleanEitherSide, std::vector< Colour12 >& palette )
{
	const int rw = 320, rh = 256;
	Rig rig( rw, rh, outW, outH );
	if( !rig.ok )
		return -2;
	rig.plugin.SetNegativeForTest( neg );
	amigaBaseline( rig, amiga::kModeHAM6, amiga::kPalLowRes, amiga::kPaletteFixed );
	std::vector< uint8_t > src( static_cast< size_t >( rw ) * rh * 4 );
	fill( src, rw, 0, 0, 160, rh, a );
	fill( src, rw, 160, 0, rw, rh, b );
	rig.upload( src );
	const auto out = rig.render( 0.0 );
	palette        = rig.plugin.AmigaBasePaletteForTest();

	std::vector< Colour12 > line( rw );
	for( int i = 0; i < rw; ++i )
		line[ i ] = as12( rig.rasterPixel( out, rw, rh, i, 128 ) );

	// From x = 100 (A has long since arrived from register 0), the first pixel
	// that is not A, and the first that is B.
	departure  = -1;
	int arrive = -1;
	for( int x = 100; x < rw; ++x )
	{
		if( departure < 0 && line[ x ] != a )
			departure = x;
		if( line[ x ] == b )
		{
			arrive = x;
			break;
		}
	}
	cleanEitherSide = departure >= 0 && arrive >= 0;
	for( int x = 100; x < departure && cleanEitherSide; ++x )
		cleanEitherSide = line[ x ] == a;
	for( int x = std::max( arrive, 0 ); x < rw && cleanEitherSide; ++x )
		cleanEitherSide = line[ x ] == b;
	return arrive < 0 || departure < 0 ? -1 : arrive - departure + 1;
}

/// The fewest pixels HAM6 can take from colour a to colour b, over the 64
/// six-bit codes with these registers: a breadth-first search of the 4,096
/// colours. The constraint itself, with no encoder in it.
int fewestPixels( Colour12 a, Colour12 b, const std::vector< Colour12 >& pal, int gunsPerPixel );

int checkHamEdge( int w, int h, Report& rep, NESolume::Negative neg = NESolume::Negative::None )
{
	const std::string at = std::to_string( w ) + "x" + std::to_string( h );
	int departure        = 0;
	bool clean           = false;
	std::vector< Colour12 > pal;
	auto isRegister = [ & ]( Colour12 c ) { return std::find( pal.begin(), pal.end(), c ) != pal.end(); };

	// 1. The edge where there is nowhere to go but straight there: one level
	// per gun, so no gun has a midpoint to be stepped through. Neither colour
	// is a grey, and neither shares two guns with one, so no register is a
	// short cut either.
	const Colour12 a1 = { 6, 9, 5 }, b1 = { 7, 8, 6 };
	const int three   = measureEdge( w, h, a1, b1, neg, departure, clean, pal );
	rep.line( "ham-edge", "a one-level edge in all three guns, neither a register (" + at + ")",
	          !isRegister( a1 ) && !isRegister( b1 ) );
	rep.line( "ham-edge", "  arrives in exactly 3 pixels", three == 3,
	          fmt( "measured %.0f, leaving A at x=%.0f (edge at 160)", three, departure ) );
	rep.line( "ham-edge", "  steady A before it and steady B after it", clean );
	const int bfs = fewestPixels( a1, b1, pal, 1 );
	rep.line( "ham-edge", "  and 3 is the fewest any HAM6 line can take (search of the codes)", bfs == 3,
	          fmt( "%.0f", bfs ) );

	// 2. A big hard edge, off the palette. The exact optimum under squared
	// error does NOT arrive in three: it steps two guns through a midpoint,
	// spreading the edge over more pixels -- that smear is the HAM fringe. What
	// must hold is the bound: never fewer than three.
	const Colour12 a2 = { 2, 9, 5 }, b2 = { 15, 3, 11 };
	const int big     = measureEdge( w, h, a2, b2, neg, departure, clean, pal );
	rep.line( "ham-edge", "a big edge off the palette never arrives in fewer than 3", big >= 3 && !isRegister( b2 ),
	          fmt( "measured %.0f pixels, leaving A at x=%.0f", big, departure ) );
	rep.line( "ham-edge", "  steady A before it and steady B after it", clean );

	// 3. Onto a register: one pixel, at the edge.
	const Colour12 grey = { 12, 12, 12 };
	const int one       = measureEdge( w, h, a2, grey, neg, departure, clean, pal );
	rep.line( "ham-edge", "an edge onto a register (grey 12) arrives in 1 pixel, at the edge",
	          one == 1 && departure == 160 && isRegister( grey ), fmt( "measured %.0f, at x=%.0f", one, departure ) );
	rep.line( "ham-edge", "  steady A before it and steady grey after it", clean );
	return rep.failures;
}

int fewestPixels( Colour12 a, Colour12 b, const std::vector< Colour12 >& pal, int gunsPerPixel )
{
	std::vector< int > dist( 4096, -1 );
	std::vector< int > queue = { amiga::index12( a ) };
	dist[ queue[ 0 ] ]       = 0;
	for( size_t q = 0; q < queue.size(); ++q )
	{
		const Colour12 c = amiga::fromIndex12( queue[ q ] );
		auto visit       = [ & ]( Colour12 n ) {
			const int k = amiga::index12( n );
			if( dist[ k ] < 0 )
			{
				dist[ k ] = dist[ queue[ q ] ] + 1;
				queue.push_back( k );
			}
		};
		for( const Colour12& p : pal )
			visit( p );
		for( int v = 0; v < 16; ++v )
		{
			visit( { uint8_t( v ), c.g, c.b } );
			visit( { c.r, uint8_t( v ), c.b } );
			visit( { c.r, c.g, uint8_t( v ) } );
			if( gunsPerPixel >= 2 )
				for( int u = 0; u < 16; ++u )
				{
					visit( { uint8_t( v ), uint8_t( u ), c.b } );
					visit( { uint8_t( v ), c.g, uint8_t( u ) } );
					visit( { c.r, uint8_t( v ), uint8_t( u ) } );
				}
		}
	}
	return dist[ amiga::index12( b ) ];
}

//---------------------------------------------------------------------------
// --ham-optimal
//---------------------------------------------------------------------------
struct Pcg
{
	uint64_t state = 0x853c49e6748fea9bULL;
	uint32_t next()
	{
		const uint64_t old = state;
		state              = old * 6364136223846793005ULL + 1442695040888963407ULL;
		const uint32_t xs  = static_cast< uint32_t >( ( ( old >> 18u ) ^ old ) >> 27u );
		const uint32_t rot = static_cast< uint32_t >( old >> 59u );
		return ( xs >> rot ) | ( xs << ( ( -rot ) & 31 ) );
	}
	int below( int n ) { return static_cast< int >( next() % static_cast< uint32_t >( n ) ); }
};

/// Apply one of the 64 six-bit HAM codes to the previous colour.
/// 00xxxx register, 01xxxx blue, 10xxxx red, 11xxxx green (HRM ch. 3).
Colour12 applyCode( int code, Colour12 prev, const std::vector< Colour12 >& pal )
{
	const int v = code & 15;
	switch( code >> 4 )
	{
	case 0:
		return pal[ v ];
	case 1:
		prev.b = uint8_t( v );
		return prev;
	case 2:
		prev.r = uint8_t( v );
		return prev;
	default:
		prev.g = uint8_t( v );
		return prev;
	}
}

/// Every one of the 64^L bitplane codes, depth first, pruned only where the
/// partial error already reaches the best found -- errors are non-negative,
/// so the pruning cannot lose the optimum.
void exhaustive( const std::vector< uint8_t >& rgb, int x, Colour12 prev, int64_t cost, const std::vector< Colour12 >& pal,
                 int64_t& best )
{
	const int L = static_cast< int >( rgb.size() / 3 );
	if( cost >= best )
		return;
	if( x == L )
	{
		best = cost;
		return;
	}
	for( int code = 0; code < 64; ++code )
	{
		const Colour12 c = applyCode( code, prev, pal );
		exhaustive( rgb, x + 1, c, cost + amiga::pixelError( c, &rgb[ x * 3 ] ), pal, best );
	}
}

/// The obvious encoder: each pixel's best code given the one before. Here to
/// prove the comparison can fail -- a check that can only say "equal" is
/// not a check.
int64_t greedy( const std::vector< uint8_t >& rgb, const std::vector< Colour12 >& pal )
{
	const int L   = static_cast< int >( rgb.size() / 3 );
	Colour12 prev = pal[ 0 ];
	int64_t total = 0;
	for( int x = 0; x < L; ++x )
	{
		int32_t best = INT32_MAX;
		Colour12 pick;
		for( int code = 0; code < 64; ++code )
		{
			const Colour12 c = applyCode( code, prev, pal );
			const int32_t e  = amiga::pixelError( c, &rgb[ x * 3 ] );
			if( e < best )
			{
				best = e;
				pick = c;
			}
		}
		total += best;
		prev = pick;
	}
	return total;
}

int checkHamOptimal( Report& rep )
{
	Pcg rng;
	amiga::HamEncoder encoder;
	const int counts[] = { 0, 60, 60, 60, 40, 16, 6 };
	int lines = 0, equal = 0, legal = 0, consistent = 0, greedyWorse = 0;
	auto t0   = std::chrono::steady_clock::now();
	for( int L = 1; L <= 6; ++L )
		for( int n = 0; n < counts[ L ]; ++n )
		{
			std::vector< Colour12 > pal( 16 );
			for( auto& c : pal )
				c = { uint8_t( rng.below( 16 ) ), uint8_t( rng.below( 16 ) ), uint8_t( rng.below( 16 ) ) };
			std::vector< uint8_t > rgb( static_cast< size_t >( L ) * 3 );
			for( int x = 0; x < L; ++x )
			{
				// Half the lines aim near a register, half anywhere, so both
				// kinds of step are exercised.
				if( n % 2 == 0 )
					for( int k = 0; k < 3; ++k )
						rgb[ x * 3 + k ] = uint8_t( rng.below( 256 ) );
				else
				{
					const Colour12 p = pal[ rng.below( 16 ) ];
					const int base[ 3 ] = { amiga::to8( p.r ), amiga::to8( p.g ), amiga::to8( p.b ) };
					for( int k = 0; k < 3; ++k )
						rgb[ x * 3 + k ] = uint8_t( std::clamp( base[ k ] + rng.below( 81 ) - 40, 0, 255 ) );
				}
			}

			std::vector< Colour12 > out( L );
			const int64_t dp = encoder.encodeLine( rgb.data(), L, pal, out.data() );
			int64_t best     = INT64_MAX;
			exhaustive( rgb, 0, pal[ 0 ], 0, pal, best );

			int64_t recomputed = 0;
			for( int x = 0; x < L; ++x )
				recomputed += amiga::pixelError( out[ x ], &rgb[ x * 3 ] );

			++lines;
			equal += dp == best;
			legal += amiga::hamLegal( out.data(), L, pal );
			consistent += recomputed == dp;
			greedyWorse += greedy( rgb, pal ) > best;
		}
	const double secs = std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count();
	rep.line( "ham-opt", "DP error == exhaustive search over every 6-bit code", equal == lines,
	          fmt( "%.0f of %.0f lines, lengths 1..6 (%.1f s)", equal, lines, secs ) );
	rep.line( "ham-opt", "  and the DP's line is displayable HAM6", legal == lines, fmt( "%.0f of %.0f", legal, lines ) );
	rep.line( "ham-opt", "  and its error, recomputed from its pixels, is what it returned", consistent == lines,
	          fmt( "%.0f of %.0f", consistent, lines ) );
	rep.line( "ham-opt", "the comparison can fail: greedy loses to the optimum somewhere", greedyWorse > 0,
	          fmt( "greedy strictly worse on %.0f of %.0f", greedyWorse, lines ) );

	// Full-width lines: no exhaustive search, but optimal can never lose.
	int neverWorse = 0;
	const int wide = 40;
	for( int n = 0; n < wide; ++n )
	{
		std::vector< Colour12 > pal( 16 );
		for( auto& c : pal )
			c = { uint8_t( rng.below( 16 ) ), uint8_t( rng.below( 16 ) ), uint8_t( rng.below( 16 ) ) };
		std::vector< uint8_t > rgb( 320 * 3 );
		// A smooth random walk, like a picture's line rather than noise.
		int v[ 3 ] = { rng.below( 256 ), rng.below( 256 ), rng.below( 256 ) };
		for( int x = 0; x < 320; ++x )
			for( int k = 0; k < 3; ++k )
			{
				v[ k ]           = std::clamp( v[ k ] + rng.below( 41 ) - 20 + ( rng.below( 40 ) == 0 ? rng.below( 256 ) - 128 : 0 ), 0, 255 );
				rgb[ x * 3 + k ] = uint8_t( v[ k ] );
			}
		std::vector< Colour12 > out( 320 );
		const int64_t dp = encoder.encodeLine( rgb.data(), 320, pal, out.data() );
		neverWorse += dp <= greedy( rgb, pal ) && amiga::hamLegal( out.data(), 320, pal );
	}
	rep.line( "ham-opt", "320-pixel lines: legal, and never worse than greedy", neverWorse == wide,
	          fmt( "%.0f of %.0f", neverWorse, wide ) );
	return rep.failures;
}

//---------------------------------------------------------------------------
// The test card at output size, for the legality checks.
//---------------------------------------------------------------------------
std::vector< uint8_t > card( int w, int h )
{
	std::vector< uint8_t > img( static_cast< size_t >( w ) * h * 4 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const double u = double( x ) / w, v = double( y ) / h;
			uint8_t* p     = img.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
			if( v < 0.5 )
			{
				// Hue across, brightness down, like the main card.
				const double hh = u * 6.0, bright = 1.0 - v * 1.6;
				const double c = bright, xx = c * ( 1.0 - std::fabs( std::fmod( hh, 2.0 ) - 1.0 ) );
				double r = 0, g = 0, b = 0;
				if( hh < 1 ) { r = c; g = xx; }
				else if( hh < 2 ) { r = xx; g = c; }
				else if( hh < 3 ) { g = c; b = xx; }
				else if( hh < 4 ) { g = xx; b = c; }
				else if( hh < 5 ) { r = xx; b = c; }
				else { r = c; b = xx; }
				p[ 0 ] = uint8_t( std::lround( r * 255 ) );
				p[ 1 ] = uint8_t( std::lround( g * 255 ) );
				p[ 2 ] = uint8_t( std::lround( b * 255 ) );
			}
			else if( v < 0.75 )
			{
				// Saturated blocks with hard edges: where HAM fringes.
				const int k = int( u * 8 );
				const uint8_t cols[ 8 ][ 3 ] = { { 230, 30, 30 }, { 30, 200, 60 }, { 40, 60, 230 }, { 240, 220, 40 },
					                             { 20, 200, 220 }, { 210, 40, 200 }, { 250, 250, 250 }, { 10, 10, 10 } };
				p[ 0 ] = cols[ k ][ 0 ];
				p[ 1 ] = cols[ k ][ 1 ];
				p[ 2 ] = cols[ k ][ 2 ];
			}
			else
			{
				const uint8_t g = uint8_t( std::lround( u * 255 ) );
				p[ 0 ] = p[ 1 ] = p[ 2 ] = g;
			}
			p[ 3 ] = 255;
		}
	return img;
}

void faultsOn( Rig& rig )
{
	rig.set( "Palette Glitch", 0.6f );
	rig.set( "Garbage", 0.5f );
	rig.set( "Block Glitch", 0.5f );
	rig.set( "Line Glitch", 0.5f );
	rig.set( "Wave", 0.3f );
	rig.set( "Shake", 0.3f );
	rig.set( "Dither", 0.6f );
}

//---------------------------------------------------------------------------
// --ehb
//---------------------------------------------------------------------------
int checkEhb( int w, int h, Report& rep, NESolume::Negative neg = NESolume::Negative::None )
{
	const std::string at = std::to_string( w ) + "x" + std::to_string( h );
	Rig rig( w, h, w, h );
	if( !rig.ok )
	{
		rep.line( "ehb", "context", false );
		return rep.failures;
	}
	rig.plugin.SetNegativeForTest( neg );
	amigaBaseline( rig, amiga::kModeEHB, amiga::kPalLowRes, amiga::kPalettePerFrame );
	rig.set( "Dither", 0.35f );
	rig.upload( card( w, h ) );
	std::vector< uint8_t > out;
	for( int f = 0; f < 3; ++f )
		out = rig.render( f / 60.0 );
	const auto base = rig.plugin.AmigaBasePaletteForTest();

	std::set< int > bases, twins;
	for( const Colour12& c : base )
		bases.insert( amiga::index12( c ) );
	for( const Colour12& c : base )
		twins.insert( amiga::index12( amiga::halfBrite( c ) ) );

	size_t bad = 0, twinOnly = 0, onBase = 0;
	std::set< int > used;
	for( size_t i = 0; i < out.size(); i += 4 )
	{
		if( !is12( &out[ i ] ) )
		{
			++bad;
			continue;
		}
		const int k = amiga::index12( as12( &out[ i ] ) );
		used.insert( k );
		if( bases.count( k ) )
			++onBase;
		else if( twins.count( k ) )
			++twinOnly;
		else
			++bad;
	}
	rep.line( "ehb", "32 base registers (" + at + ")", base.size() == 32, fmt( "%.0f", double( base.size() ) ) );
	rep.line( "ehb", "every pixel a register, or one shifted right a bit per gun", bad == 0,
	          fmt( "%.0f of %.0f pixels outside", double( bad ), double( out.size() / 4 ) ) );
	rep.line( "ehb", "  and the twins are really used (not a 32-colour picture)", twinOnly > 0 && onBase > 0,
	          fmt( "%.0f px on a twin, %.0f on a base, %.0f distinct", double( twinOnly ), double( onBase ), double( used.size() ) ) );
	return rep.failures;
}

//---------------------------------------------------------------------------
// --palette
//---------------------------------------------------------------------------
int checkPalette( int w, int h, Report& rep, NESolume::Negative neg = NESolume::Negative::None )
{
	const std::string at = std::to_string( w ) + "x" + std::to_string( h );
	struct Case
	{
		const char* name;
		int mode, screen, palette;
		bool laced;
	};
	const Case cases[] = {
		{ "OCS 32, PAL low res", amiga::kModeOCS, amiga::kPalLowRes, amiga::kPalettePerFrame, false },
		{ "EHB, PAL low res", amiga::kModeEHB, amiga::kPalLowRes, amiga::kPalettePerFrame, false },
		{ "HAM6, PAL low res", amiga::kModeHAM6, amiga::kPalLowRes, amiga::kPalettePerFrame, false },
		{ "HAM6, NTSC laced", amiga::kModeHAM6, amiga::kNtscLowRes, amiga::kPalettePerFrame, true },
		{ "high res (16), fixed", amiga::kModeHAM6, amiga::kPalHighRes, amiga::kPaletteFixed, false },
		{ "OCS 32, fixed, NTSC high res laced", amiga::kModeOCS, amiga::kNtscHighRes, amiga::kPaletteFixed, true },
	};
	for( const Case& c : cases )
	{
		if( neg != NESolume::Negative::None && c.mode != amiga::kModeOCS )
			continue;
		Rig rig( w, h, w, h );
		if( !rig.ok )
		{
			rep.line( "palette", "context", false );
			return rep.failures;
		}
		rig.plugin.SetNegativeForTest( neg );
		amigaBaseline( rig, c.mode, c.screen, c.palette );
		rig.set( "Interlace", c.laced ? 1.0f : 0.0f );
		faultsOn( rig );
		rig.upload( card( w, h ) );
		std::vector< uint8_t > out;
		for( int f = 0; f < 3; ++f )
			out = rig.render( f / 60.0 );
		size_t off12 = 0, offShown = 0;
		const auto shown = rig.plugin.AmigaDisplayPaletteForTest();
		std::set< int > legalSet;
		for( const Colour12& k : shown )
			legalSet.insert( amiga::index12( k ) );
		const bool hires  = amiga::screen( c.screen ).hires;
		const bool isHam  = amiga::effectiveMode( c.mode, hires ) == amiga::kModeHAM6;
		for( size_t i = 0; i < out.size(); i += 4 )
		{
			if( !is12( &out[ i ] ) )
				++off12;
			else if( !isHam && !legalSet.count( amiga::index12( as12( &out[ i ] ) ) ) )
				++offShown;
		}
		rep.line( "palette", std::string( c.name ) + ", faults on: every pixel 12-bit (" + at + ")", off12 == 0,
		          fmt( "%.0f of %.0f off the 4-bit grid", double( off12 ), double( out.size() / 4 ) ) );
		if( !isHam )
			rep.line( "palette", "  and every pixel one of the registers shown", offShown == 0,
			          fmt( "%.0f outside, %.0f registers", double( offShown ), double( shown.size() ) ) );
	}

	if( neg != NESolume::Negative::None )
		return rep.failures;

	// HAM legality out of the picture: faults that move texels off, so the
	// raster's lines read back in order.
	for( int laced = 0; laced <= 1; ++laced )
	{
		const int rw = 320, rh = laced ? 512 : 256;
		Rig rig( w, h, w, h );
		amigaBaseline( rig, amiga::kModeHAM6, amiga::kPalLowRes, amiga::kPalettePerFrame );
		rig.set( "Interlace", laced ? 1.0f : 0.0f );
		rig.set( "Flicker Fixer", 1.0f );//every raster line shown, in order
		rig.set( "Dither", 0.35f );
		rig.set( "Palette Glitch", 0.6f );//before colour choice: still legal
		rig.set( "Garbage", 0.5f );
		rig.upload( card( w, h ) );
		std::vector< uint8_t > out;
		for( int f = 0; f < 3; ++f )
			out = rig.render( f / 60.0 );
		const auto pal = rig.plugin.AmigaBasePaletteForTest();
		const int step = std::max( 1, static_cast< int >( std::ceil( double( rh ) / h ) ) );
		int linesChecked = 0, linesLegal = 0;
		for( int j = 0; j < rh; j += step )
		{
			std::vector< Colour12 > line( rw );
			for( int i = 0; i < rw; ++i )
				line[ i ] = as12( rig.rasterPixel( out, rw, rh, i, j ) );
			++linesChecked;
			linesLegal += amiga::hamLegal( line.data(), rw, pal );
		}
		rep.line( "palette", std::string( "HAM6 " ) + ( laced ? "laced" : "PAL" ) + ": every sampled line is displayable HAM6",
		          linesLegal == linesChecked && linesChecked > 0, fmt( "%.0f of %.0f lines", linesLegal, linesChecked ) );
	}
	return rep.failures;
}

//---------------------------------------------------------------------------
// --lace
//---------------------------------------------------------------------------
int checkLace( int w, int h, Report& rep, NESolume::Negative neg = NESolume::Negative::None )
{
	const std::string at = std::to_string( w ) + "x" + std::to_string( h );
	const int rw = 320, rh = 512;//PAL low res, laced: the source is the raster

	// The output row whose raster line is even, nearest the middle: the one
	// line the detail sits on.
	// Not a row whose centre falls on a line boundary: there the rasteriser
	// may round either way (the software renderer steps uv.y ~1e-5 off).
	auto evenRow = []( int h, int rh, int& line ) {
		for( int y = h / 2; y < h; ++y )
		{
			const double v = ( y + 0.5 ) * rh / h;
			const double f = v - std::floor( v );
			line           = static_cast< int >( v );
			if( ( line & 1 ) == 0 && f > 0.2 && f < 0.8 )
				return y;
		}
		return -1;
	};
	int d        = 0;
	const int y0 = evenRow( h, rh, d );
	// A row inside the uniform grey band at the top.
	const int yArea = static_cast< int >( h * 24.0 / rh );

	const Colour12 white = { 15, 15, 15 }, grey = { 8, 8, 8 }, black = { 0, 0, 0 }, red = { 15, 0, 0 };
	std::vector< uint8_t > s1( static_cast< size_t >( rw ) * rh * 4 );
	fill( s1, rw, 0, 0, rw, rh, black );
	fill( s1, rw, 0, 0, rw, 64, grey );
	fill( s1, rw, 0, d, rw, d + 1, white );

	// The same picture with only odd lines changed, and with only even ones.
	std::vector< uint8_t > sOdd = s1, sEven = s1;
	for( int y = 300; y < 400; ++y )
		fill( ( y & 1 ) ? sOdd : sEven, rw, 40, y, 280, y + 1, red );

	auto rowMean = []( const std::vector< uint8_t >& out, int w, int y ) {
		double s = 0;
		for( int x = 0; x < w; ++x )
			s += out[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
		return s / w;
	};

	auto setup = [ & ]( Rig& rig, bool fixer ) {
		rig.plugin.SetNegativeForTest( neg );
		amigaBaseline( rig, amiga::kModeOCS, amiga::kPalLowRes, amiga::kPaletteFixed );
		rig.set( "Interlace", 1.0f );
		rig.set( "Flicker Fixer", fixer ? 1.0f : 0.0f );
	};

	// 1. A one-line detail, one sample per field at 50 fields/s.
	Rig rig( rw, rh, w, h );
	if( !rig.ok )
	{
		rep.line( "lace", "context", false );
		return rep.failures;
	}
	setup( rig, false );
	rig.upload( s1 );
	std::vector< double > detail, area;
	for( int k = 0; k < 8; ++k )
	{
		const auto out = rig.render( k / 50.0 );
		detail.push_back( rowMean( out, w, y0 ) );
		area.push_back( rowMean( out, w, yArea ) );
	}
	bool alternates = true;
	for( int k = 0; k < 8; ++k )
		alternates = alternates && ( k % 2 == 0 ? detail[ k ] > 200.0 : detail[ k ] < 20.0 );
	rep.line( "lace", "a one-line detail is on in even fields, off in odd (" + at + ")", alternates,
	          fmt( "row %.0f = raster line %.0f: %.0f / %.0f", y0, d, detail[ 0 ] ) + fmt( " / %.0f ...", detail[ 1 ] ) );

	// Twice per field: the pattern must be on,on,off,off -- the field comes
	// from elapsed time at 50 Hz, not from the frame count.
	std::vector< double > twice;
	for( int k = 0; k < 8; ++k )
		twice.push_back( rowMean( rig.render( 1.0 + k / 100.0 ), w, y0 ) );
	bool timeDriven = true;
	for( int k = 0; k < 8; ++k )
		timeDriven = timeDriven && ( ( k / 2 ) % 2 == 0 ? twice[ k ] > 200.0 : twice[ k ] < 20.0 );
	rep.line( "lace", "  sampled at 100 Hz it is on,on,off,off: 25 Hz = 50 fields/s / 2", timeDriven );

	double areaSpread = 0;
	for( double a : area )
		areaSpread = std::max( areaSpread, std::fabs( a - area[ 0 ] ) );
	rep.line( "lace", "a uniform area does not flicker", areaSpread == 0.0 && area[ 0 ] > 100.0,
	          fmt( "row %.0f spread %.3f levels at %.0f", yArea, areaSpread, area[ 0 ] ) );

	// 2. Alternate lines change on alternate fields.
	auto differs = [ & ]( const std::vector< uint8_t >& a, const std::vector< uint8_t >& b, double t, bool fixer ) {
		Rig ra( rw, rh, w, h ), rb( rw, rh, w, h );
		setup( ra, fixer );
		setup( rb, fixer );
		ra.upload( a );
		rb.upload( b );
		const auto oa = ra.render( t ), ob = rb.render( t );
		size_t n = 0;
		for( size_t i = 0; i < oa.size(); ++i )
			n += oa[ i ] != ob[ i ];
		return n;
	};
	const size_t oddInEven  = differs( s1, sOdd, 0.0, false );
	const size_t oddInOdd   = differs( s1, sOdd, 1.0 / 50.0, false );
	const size_t evenInEven = differs( s1, sEven, 0.0, false );
	const size_t evenInOdd  = differs( s1, sEven, 1.0 / 50.0, false );
	rep.line( "lace", "odd lines changed: nothing moves in an even field", oddInEven == 0,
	          fmt( "%.0f bytes", double( oddInEven ) ) );
	rep.line( "lace", "  and the odd field shows the change", oddInOdd > 0, fmt( "%.0f bytes", double( oddInOdd ) ) );
	rep.line( "lace", "even lines changed: nothing moves in an odd field", evenInOdd == 0,
	          fmt( "%.0f bytes", double( evenInOdd ) ) );
	rep.line( "lace", "  and the even field shows the change", evenInEven > 0, fmt( "%.0f bytes", double( evenInEven ) ) );

	// 3. The flicker fixer.
	Rig fixed( rw, rh, w, h );
	setup( fixed, true );
	fixed.upload( s1 );
	double lo = 1e9, hi = -1;
	for( int k = 0; k < 8; ++k )
	{
		const double m = rowMean( fixed.render( k / 50.0 ), w, y0 );
		lo             = std::min( lo, m );
		hi             = std::max( hi, m );
	}
	rep.line( "lace", "flicker fixer: the detail is steady, and there", hi - lo == 0.0 && lo > 200.0,
	          fmt( "%.0f..%.0f over 8 fields", lo, hi ) );
	const size_t fixerBoth = differs( s1, sOdd, 0.0, true );
	rep.line( "lace", "  and an even field shows the odd lines too (woven)", fixerBoth > 0,
	          fmt( "%.0f bytes", double( fixerBoth ) ) );

	// 4. NTSC: 400 lines, 60 fields a second.
	{
		const int nh = 400;
		int nd        = 0;
		const int ny0 = evenRow( h, nh, nd );
		std::vector< uint8_t > n1( static_cast< size_t >( rw ) * nh * 4 );
		fill( n1, rw, 0, 0, rw, nh, black );
		fill( n1, rw, 0, nd, rw, nd + 1, white );
		Rig nr( rw, nh, w, h );
		setup( nr, false );
		nr.set( "Screen Mode", float( amiga::kNtscLowRes ) );
		nr.upload( n1 );
		bool ok = true;
		for( int k = 0; k < 8; ++k )
		{
			const double m = rowMean( nr.render( 2.0 + k / 60.0 ), w, ny0 );
			ok             = ok && ( k % 2 == 0 ? m > 200.0 : m < 20.0 );
		}
		rep.line( "lace", "NTSC: the detail alternates at 60 fields/s (30 Hz)", ok );
	}

	// 5. Not laced: no fields at all.
	{
		std::vector< uint8_t > p1( static_cast< size_t >( rw ) * 256 * 4 );
		fill( p1, rw, 0, 0, rw, 256, black );
		fill( p1, rw, 0, 128, rw, 129, white );
		Rig pr( rw, 256, w, h );
		setup( pr, false );
		pr.set( "Interlace", 0.0f );
		pr.upload( p1 );
		const auto a = pr.render( 0.0 ), b = pr.render( 1.0 / 50.0 );
		rep.line( "lace", "progressive (Interlace off): consecutive fields identical", a == b );
	}
	return rep.failures;
}

//---------------------------------------------------------------------------
// Negative controls: each must make its check FAIL.
//---------------------------------------------------------------------------
int checkNegatives( int w, int h )
{
	int wrong = 0;
	auto expectFail = [ & ]( const char* what, const std::function< int( Report& ) >& run ) {
		Report r;
		r.quiet            = true;
		const int failures = run( r );
		const bool caught  = failures > 0;
		std::printf( "negative  %-58s %s  (%d failed lines)\n", what, caught ? "caught" : "NOT CAUGHT", failures );
		wrong += !caught;
	};
	expectFail( "HAM may change two guns per pixel -> --ham-edge", [ & ]( Report& r ) {
		return checkHamEdge( w, h, r, NESolume::Negative::TwoGunModify );
	} );
	expectFail( "EHB twins rounded, not shifted -> --ehb", [ & ]( Report& r ) {
		return checkEhb( w, h, r, NESolume::Negative::RoundedHalfBrite );
	} );
	expectFail( "registers one 8-bit level off the 12-bit grid -> --palette", [ & ]( Report& r ) {
		return checkPalette( w, h, r, NESolume::Negative::EightBitPalette );
	} );
	expectFail( "the field never advances -> --lace", [ & ]( Report& r ) {
		return checkLace( w, h, r, NESolume::Negative::FrozenField );
	} );
	std::printf( "%s\n", wrong == 0 ? "negative: every perturbation caught" : "negative: FAILURES" );
	return wrong;
}

//---------------------------------------------------------------------------
// --ham-cost
//---------------------------------------------------------------------------
int hamCost( int w, int h )
{
	// The encoder alone, on the card's raster, at the thread count the plugin
	// uses and on one thread.
	const int threads = std::clamp( static_cast< int >( std::thread::hardware_concurrency() ) / 2, 1, 8 );
	for( int lines : { 256, 512 } )
	{
		const auto img = card( 320, lines );
		std::vector< uint8_t > out( img.size() );
		const auto pal = amiga::choosePalette( img.data(), 320 * lines, 16, false, {} );
		for( int t : { 1, threads } )
		{
			double best = 1e9;
			for( int rep = 0; rep < 5; ++rep )
			{
				const auto t0 = std::chrono::steady_clock::now();
				amiga::encodeHamFrame( img.data(), out.data(), 320, lines, pal, t );
				best = std::min( best, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
			}
			std::printf( "ham-cost  encoder 320x%d on %d thread%s: %.2f ms (best of 5)\n", lines, t, t == 1 ? "" : "s", best );
		}
	}

	// The whole frame through the plugin at the output size, CPU part and wall.
	for( int laced = 0; laced <= 1; ++laced )
		for( int mode : { amiga::kModeOCS, amiga::kModeHAM6 } )
		{
			Rig rig( w, h, w, h );
			amigaBaseline( rig, mode, amiga::kPalLowRes, amiga::kPalettePerFrame );
			rig.set( "Dither", 0.35f );
			rig.set( "Interlace", laced ? 1.0f : 0.0f );
			rig.upload( card( w, h ) );
			double cpu = 0, wall = 0;
			const int n = 20;
			rig.render( 0 );
			for( int f = 1; f <= n; ++f )
			{
				const auto t0 = std::chrono::steady_clock::now();
				rig.render( f / 60.0 );//includes this harness's own read-back
				wall += std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
				cpu += rig.plugin.AmigaCpuMsForTest();
			}
			std::printf( "ham-cost  plugin %-5s %s at %dx%d: %.2f ms/frame on the CPU path (read-backs + palette + encode), "
			             "%.2f ms/frame wall incl. the harness read-back\n",
			             mode == amiga::kModeHAM6 ? "HAM6" : "OCS", laced ? "laced" : "PAL  ", w, h, cpu / n, wall / n );
		}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int runAmigaNames( NESolume& plugin )
{
	// Resolume addresses a parameter by its name lower-cased with the spaces
	// taken out (memory: resolume-params-by-name), and two that reduce to the
	// same address are ONE parameter to it. So the reduced names must be unique.
	std::map< std::string, std::string > seen;
	int clashes = 0;
	for( unsigned i = 0; i < plugin.GetNumParams(); ++i )
	{
		std::string key;
		for( const char* p = plugin.GetParamName( i ); *p; ++p )
			if( *p != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( *p ) ) );
		if( seen.count( key ) )
		{
			std::printf( "names     '%s' and '%s' are both /%s to Resolume  FAILED\n", seen[ key ].c_str(),
			             plugin.GetParamName( i ), key.c_str() );
			++clashes;
		}
		seen[ key ] = plugin.GetParamName( i );
	}
	std::printf( "names     %u parameters, %zu distinct Resolume addresses  %s\n", plugin.GetNumParams(), seen.size(),
	             clashes == 0 ? "ok" : "FAILED" );
	return clashes == 0 ? 0 : 1;
}

int runAmigaParams( NESolume& plugin )
{
	// The same shape `oxbow probe` prints, so tools/compat.py can read the
	// previous release's table (probed from its released bundle) and this one
	// with one parser.
	std::printf( "params:      %u\n", plugin.GetNumParams() );
	for( unsigned i = 0; i < plugin.GetNumParams(); ++i )
	{
		const unsigned type = plugin.GetParamType( i );
		const std::string group = plugin.GetParamGroup( i );
		if( type == FF_TYPE_TEXT )
			std::printf( "  [%2u] %-16s type=%-3u default=0 range=0..1\n", i, plugin.GetParamName( i ), type );
		else
			std::printf( "  [%2u] %-16s type=%-3u default=%g range=0..1 %s%s\n", i, plugin.GetParamName( i ), type,
			             plugin.GetFloatParameter( i ), group.empty() ? "" : " group=", group.c_str() );
		for( unsigned e = 0; type == FF_TYPE_OPTION && e < plugin.GetNumParamElements( i ); ++e )
		{
			// The SDK hands the element's float value back in an integer's bits.
			const FFUInt32 bits = plugin.GetParamElementDefault( i, e ).UIntValue;
			float value         = 0.0f;
			std::memcpy( &value, &bits, sizeof value );
			std::printf( "        - %-20s = %g\n", plugin.GetParamElementName( i, e ), value );
		}
	}
	return 0;
}

int runAmigaCheck( const std::string& which, int w, int h )
{
	Report rep;
	if( which == "--ham-edge" )
		checkHamEdge( w, h, rep );
	else if( which == "--ham-optimal" )
		checkHamOptimal( rep );
	else if( which == "--ehb" )
		checkEhb( w, h, rep );
	else if( which == "--palette" )
		checkPalette( w, h, rep );
	else if( which == "--lace" )
		checkLace( w, h, rep );
	else if( which == "--negative" )
		return checkNegatives( w, h ) == 0 ? 0 : 1;
	else if( which == "--ham-cost" )
		return hamCost( w, h );
	std::printf( "%s: %s\n", which.c_str() + 2, rep.failures == 0 ? "all ok" : "FAILURES" );
	return rep.failures == 0 ? 0 : 1;
}
