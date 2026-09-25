#include "Amiga.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace nesolume::amiga
{
namespace
{
const Screen kScreens[] = {
	{ "PAL Low Res", 320, 256, 50, false },
	{ "NTSC Low Res", 320, 200, 60, false },
	{ "PAL High Res", 640, 256, 50, true },
	{ "NTSC High Res", 640, 200, 60, true },
};
static_assert( sizeof( kScreens ) / sizeof( kScreens[ 0 ] ) == kScreenCount, "one row per ScreenId" );

const char* const kModeNames[] = { "OCS 32 Colours", "Extra Half-Brite", "HAM6" };
static_assert( sizeof( kModeNames ) / sizeof( kModeNames[ 0 ] ) == kModeCount, "one name per Mode" );

const char* const kPaletteChoiceNames[] = { "Per Frame", "Fixed" };
static_assert( sizeof( kPaletteChoiceNames ) / sizeof( kPaletteChoiceNames[ 0 ] ) == kPaletteChoiceCount,
               "one name per PaletteChoice" );

/// Luma of a 12-bit colour, for ordering registers darkest first.
double luma( const Colour12& c )
{
	return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
}

void sortDarkestFirst( std::vector< Colour12 >& p )
{
	std::stable_sort( p.begin(), p.end(), []( const Colour12& a, const Colour12& b ) {
		const double la = luma( a ), lb = luma( b );
		if( la != lb )
			return la < lb;
		return index12( a ) < index12( b );
	} );
}

constexpr int32_t kInf = 1 << 30;

/// One step's state: the best cost with each gun free and the other two
/// fixed, and the best of all. See HamEncoder in the header.
constexpr int kAR          = 0;  //[ g * 16 + b ]  min over red
constexpr int kAG          = 256;//[ r * 16 + b ]  min over green
constexpr int kAB          = 512;//[ r * 16 + g ]  min over blue
constexpr int kM           = 768;//min over everything
constexpr int kTableStride = 772;//769, padded to a multiple of 4 ints

inline int32_t clampInf( int32_t v )
{
	return v < kInf ? v : kInf;
}
} // namespace

const char* modeName( int mode )
{
	return kModeNames[ std::clamp( mode, 0, kModeCount - 1 ) ];
}

const Screen& screen( int id )
{
	return kScreens[ std::clamp( id, 0, kScreenCount - 1 ) ];
}

const char* paletteChoiceName( int choice )
{
	return kPaletteChoiceNames[ std::clamp( choice, 0, kPaletteChoiceCount - 1 ) ];
}

int effectiveMode( int mode, bool hires )
{
	return hires ? kModeOCS : std::clamp( mode, 0, kModeCount - 1 );
}

int baseRegisterCount( int mode, bool hires )
{
	if( hires )
		return 16;
	switch( effectiveMode( mode, hires ) )
	{
	case kModeHAM6:
		return 16;
	default:
		return 32;
	}
}

int64_t fieldIndex( double seconds, int fieldHz )
{
	return static_cast< int64_t >( std::floor( seconds * static_cast< double >( fieldHz ) + 1e-6 ) );
}

//---------------------------------------------------------------------------
std::vector< Colour12 > fixedPalette( int mode, bool hires )
{
	std::vector< Colour12 > p;
	const int m = effectiveMode( mode, hires );

	if( hires )
	{
		// Sixteen: the eight corners of the cube, three greys, and five
		// half-saturated primaries and secondaries.
		for( int r : { 0, 15 } )
			for( int g : { 0, 15 } )
				for( int b : { 0, 15 } )
					p.push_back( { uint8_t( r ), uint8_t( g ), uint8_t( b ) } );
		const Colour12 extra[] = { { 4, 4, 4 }, { 8, 8, 8 }, { 11, 11, 11 }, { 15, 8, 0 }, { 0, 8, 15 },
			                       { 8, 15, 0 }, { 15, 0, 8 }, { 8, 0, 15 } };
		p.insert( p.end(), std::begin( extra ), std::end( extra ) );
	}
	else if( m == kModeHAM6 )
	{
		// Sixteen greys. The palette is where a HAM line goes when it cannot
		// afford three pixels to arrive, and a grey is one step from any
		// colour sharing its level in one gun. With no chromatic register
		// every colour is built by modification, so the fringes show most.
		for( int v = 0; v < 16; ++v )
			p.push_back( { uint8_t( v ), uint8_t( v ), uint8_t( v ) } );
	}
	else
	{
		// Thirty-two: the 27-colour cube at 0, 8 and 15 per gun, two more
		// greys, and three warm and cool mid tones the cube has none of. EHB
		// shows the same 32 and their half-bright twins.
		for( int r : { 0, 8, 15 } )
			for( int g : { 0, 8, 15 } )
				for( int b : { 0, 8, 15 } )
					p.push_back( { uint8_t( r ), uint8_t( g ), uint8_t( b ) } );
		const Colour12 extra[] = { { 4, 4, 4 }, { 11, 11, 11 }, { 15, 11, 8 }, { 8, 4, 0 }, { 0, 4, 8 } };
		p.insert( p.end(), std::begin( extra ), std::end( extra ) );
	}

	sortDarkestFirst( p );
	return p;
}

std::vector< Colour12 > displayPalette( const std::vector< Colour12 >& base, int mode )
{
	std::vector< Colour12 > shown = base;
	if( mode == kModeEHB )
		for( const Colour12& c : base )
			shown.push_back( halfBrite( c ) );
	return shown;
}

//---------------------------------------------------------------------------
std::vector< Colour12 > choosePalette( const uint8_t* rgba, int pixels, int count, bool halfBriteTwins,
                                       const std::vector< Colour12 >& seeds )
{
	// The 12-bit histogram: what the picture looks like to the registers.
	// round( v / 17 ) in integers is ( v + 8 ) / 17.
	std::vector< uint32_t > hist( 4096, 0 );
	for( int i = 0; i < pixels; ++i )
	{
		const uint8_t* p = rgba + static_cast< size_t >( i ) * 4;
		const int r = ( p[ 0 ] + 8 ) / 17, g = ( p[ 1 ] + 8 ) / 17, b = ( p[ 2 ] + 8 ) / 17;
		++hist[ ( r << 8 ) | ( g << 4 ) | b ];
	}

	struct Point
	{
		double x[ 3 ];
		double w;
		int bin;
	};
	std::vector< Point > points;
	for( int i = 0; i < 4096; ++i )
		if( hist[ i ] )
		{
			const Colour12 c = fromIndex12( i );
			points.push_back( { { double( c.r ), double( c.g ), double( c.b ) }, double( hist[ i ] ), i } );
		}

	const double kW[ 3 ] = { 0.299, 0.587, 0.114 };
	auto dist = [ & ]( const double* a, const double* b, double scaleB ) {
		double d = 0.0;
		for( int k = 0; k < 3; ++k )
		{
			const double e = a[ k ] - b[ k ] * scaleB;
			d += kW[ k ] * e * e;
		}
		return d;
	};

	std::vector< std::array< double, 3 > > centre( static_cast< size_t >( count ) );
	if( static_cast< int >( seeds.size() ) == count )
	{
		for( int j = 0; j < count; ++j )
			centre[ j ] = { double( seeds[ j ].r ), double( seeds[ j ].g ), double( seeds[ j ].b ) };
	}
	else if( points.empty() )
	{
		for( int j = 0; j < count; ++j )
			centre[ j ] = { 0.0, 0.0, 0.0 };
	}
	else
	{
		// Deterministic farthest-point start: the heaviest bin, then whichever
		// bin has the most weight times squared distance from what is chosen.
		size_t first = 0;
		for( size_t i = 1; i < points.size(); ++i )
			if( points[ i ].w > points[ first ].w )
				first = i;
		std::vector< double > nearest( points.size(), 1e30 );
		int chosen = 0;
		size_t pick = first;
		while( chosen < count )
		{
			centre[ chosen ] = { points[ pick ].x[ 0 ], points[ pick ].x[ 1 ], points[ pick ].x[ 2 ] };
			++chosen;
			double best = -1.0;
			for( size_t i = 0; i < points.size(); ++i )
			{
				nearest[ i ] = std::min( nearest[ i ], dist( points[ i ].x, centre[ chosen - 1 ].data(), 1.0 ) );
				const double score = points[ i ].w * nearest[ i ];
				if( score > best )
				{
					best = score;
					pick = i;
				}
			}
			if( best <= 0.0 )
			{
				// Fewer distinct colours than registers: the rest repeat.
				for( ; chosen < count; ++chosen )
					centre[ chosen ] = centre[ chosen % std::max( 1, static_cast< int >( points.size() ) ) ];
				break;
			}
		}
	}

	// Lloyd iterations over the weighted bins.
	for( int iteration = 0; iteration < 10 && !points.empty(); ++iteration )
	{
		std::vector< std::array< double, 3 > > sumA( count, { 0.0, 0.0, 0.0 } ), sumH( count, { 0.0, 0.0, 0.0 } );
		std::vector< double > nA( count, 0.0 ), nH( count, 0.0 );
		for( const Point& pt : points )
		{
			int bestJ    = 0;
			bool bestTwin = false;
			double best   = 1e30;
			for( int j = 0; j < count; ++j )
			{
				const double d = dist( pt.x, centre[ j ].data(), 1.0 );
				if( d < best )
				{
					best     = d;
					bestJ    = j;
					bestTwin = false;
				}
				if( halfBriteTwins )
				{
					const double dh = dist( pt.x, centre[ j ].data(), 0.5 );
					if( dh < best )
					{
						best     = dh;
						bestJ    = j;
						bestTwin = true;
					}
				}
			}
			auto& sum = bestTwin ? sumH[ bestJ ] : sumA[ bestJ ];
			for( int k = 0; k < 3; ++k )
				sum[ k ] += pt.w * pt.x[ k ];
			( bestTwin ? nH : nA )[ bestJ ] += pt.w;
		}
		for( int j = 0; j < count; ++j )
		{
			// Minimising sum (x - c)^2 over the base's pixels plus sum (y - c/2)^2
			// over its twin's gives c = ( 4 sum x + 2 sum y ) / ( 4 n + m ).
			const double den = 4.0 * nA[ j ] + nH[ j ];
			if( den <= 0.0 )
				continue;
			for( int k = 0; k < 3; ++k )
				centre[ j ][ k ] = std::clamp( ( 4.0 * sumA[ j ][ k ] + 2.0 * sumH[ j ][ k ] ) / den, 0.0, 15.0 );
		}
	}

	std::vector< Colour12 > palette( static_cast< size_t >( count ) );
	for( int j = 0; j < count; ++j )
		palette[ j ] = { uint8_t( std::clamp( static_cast< int >( std::lround( centre[ j ][ 0 ] ) ), 0, 15 ) ),
			             uint8_t( std::clamp( static_cast< int >( std::lround( centre[ j ][ 1 ] ) ), 0, 15 ) ),
			             uint8_t( std::clamp( static_cast< int >( std::lround( centre[ j ][ 2 ] ) ), 0, 15 ) ) };
	sortDarkestFirst( palette );
	return palette;
}

//---------------------------------------------------------------------------
int64_t HamEncoder::encodeLine( const uint8_t* rgb, int width, const std::vector< Colour12 >& palette, Colour12* out,
                                int channelsPerModify )
{
	if( width <= 0 )
		return 0;

	const bool twoGuns = channelsPerModify >= 2;
	const Colour12 start = palette.empty() ? Colour12{} : palette[ 0 ];

	inPalette.assign( 4096, 0 );
	for( const Colour12& p : palette )
		inPalette[ index12( p ) ] = 1;

	// Only the first table needs initialising: every later one is written in
	// full by the step that makes it. (Filling all of them was a megabyte of
	// memset per line, 256 MB a frame -- a third of the encoder's time.)
	tables.resize( static_cast< size_t >( width + 1 ) * kTableStride );
	std::fill( tables.begin(), tables.begin() + kTableStride, kInf );

	// Before the first pixel: the line starts from register 0.
	{
		int32_t* t                        = tables.data();
		t[ kAR + start.g * 16 + start.b ] = 0;
		t[ kAG + start.r * 16 + start.b ] = 0;
		t[ kAB + start.r * 16 + start.g ] = 0;
		t[ kM ]                           = 0;
	}

	auto gunErrors = [ & ]( const uint8_t* target, int32_t* er, int32_t* eg, int32_t* eb ) {
		for( int v = 0; v < 16; ++v )
		{
			const int dr = to8( v ) - target[ 0 ], dg = to8( v ) - target[ 1 ], db = to8( v ) - target[ 2 ];
			er[ v ] = kWeightR * dr * dr;
			eg[ v ] = kWeightG * dg * dg;
			eb[ v ] = kWeightB * db * db;
		}
	};
	auto mn = []( int32_t a, int32_t b ) { return a < b ? a : b; };

	//--- Forward ------------------------------------------------------------
	// Written as sixteen-wide inner loops of adds and mins with nothing else in
	// them, so the compiler vectorises them.
	for( int x = 0; x < width; ++x )
	{
		const int32_t* P  = tables.data() + static_cast< size_t >( x ) * kTableStride;
		int32_t* N        = tables.data() + static_cast< size_t >( x + 1 ) * kTableStride;
		const int32_t* AR = P + kAR;
		const int32_t* AG = P + kAG;
		const int32_t* AB = P + kAB;
		const int32_t M   = P[ kM ];

		int32_t er[ 16 ], eg[ 16 ], eb[ 16 ];
		gunErrors( rgb + static_cast< size_t >( x ) * 3, er, eg, eb );
		int32_t mr = kInf, mg = kInf, mb = kInf;
		for( int v = 0; v < 16; ++v )
		{
			mr = mn( mr, er[ v ] );
			mg = mn( mg, eg[ v ] );
			mb = mn( mb, eb[ v ] );
		}

		// The best predecessor for each way of arriving, reduced over the gun
		// the new pixel is free in:
		//   XR[b] = min_r er[r] + AG[r][b]     YR[g] = min_r er[r] + AB[r][g]
		//   XG[b] = min_g eg[g] + AR[g][b]     YG[r] = min_g eg[g] + AB[r][g]
		//   XB[g] = min_b eb[b] + AR[g][b]     YB[r] = min_b eb[b] + AG[r][b]
		int32_t XR[ 16 ], YR[ 16 ], XG[ 16 ], YG[ 16 ], XB[ 16 ], YB[ 16 ];
		for( int k = 0; k < 16; ++k )
			XR[ k ] = YR[ k ] = XG[ k ] = kInf;
		for( int r = 0; r < 16; ++r )
		{
			const int32_t e   = er[ r ];
			const int32_t* ag = AG + r * 16;
			const int32_t* ab = AB + r * 16;
			int32_t yb = kInf, yg = kInf;
			for( int k = 0; k < 16; ++k )
			{
				XR[ k ] = mn( XR[ k ], e + ag[ k ] );
				YR[ k ] = mn( YR[ k ], e + ab[ k ] );
				yb      = mn( yb, eb[ k ] + ag[ k ] );
				yg      = mn( yg, eg[ k ] + ab[ k ] );
			}
			YB[ r ] = yb;
			YG[ r ] = yg;
		}
		for( int g = 0; g < 16; ++g )
		{
			const int32_t e   = eg[ g ];
			const int32_t* ar = AR + g * 16;
			int32_t xb        = kInf;
			for( int k = 0; k < 16; ++k )
			{
				XG[ k ] = mn( XG[ k ], e + ar[ k ] );
				xb      = mn( xb, eb[ k ] + ar[ k ] );
			}
			XB[ g ] = xb;
		}

		int32_t* nAR = N + kAR;
		int32_t* nAG = N + kAG;
		int32_t* nAB = N + kAB;
		for( int i = 0; i < 16; ++i )
		{
			const int32_t* ar = AR + i * 16;
			const int32_t* ag = AG + i * 16;
			const int32_t* ab = AB + i * 16;
			int32_t* nar      = nAR + i * 16;
			int32_t* nag      = nAG + i * 16;
			int32_t* nab      = nAB + i * 16;
			const int32_t egi = eg[ i ], eri = er[ i ], yr = YR[ i ], yg = YG[ i ], yb = YB[ i ];
			for( int j = 0; j < 16; ++j )
			{
				nar[ j ] = mn( egi + eb[ j ] + mn( mn( mr + ar[ j ], XR[ j ] ), yr ), kInf );//[ g=i, b=j ]
				nag[ j ] = mn( eri + eb[ j ] + mn( mn( mg + ag[ j ], XG[ j ] ), yg ), kInf );//[ r=i, b=j ]
				nab[ j ] = mn( eri + eg[ j ] + mn( mn( mb + ab[ j ], XB[ j ] ), yb ), kInf );//[ r=i, g=j ]
			}
		}

		// The negative control's extra arrivals: two guns changed at once.
		if( twoGuns )
		{
			int32_t ARb[ 16 ], ARg[ 16 ], AGr[ 16 ];
			int32_t KR = kInf, KG = kInf, KB = kInf;
			std::fill( ARb, ARb + 16, kInf );
			std::fill( ARg, ARg + 16, kInf );
			std::fill( AGr, AGr + 16, kInf );
			for( int g = 0; g < 16; ++g )
				for( int b = 0; b < 16; ++b )
				{
					ARb[ b ] = std::min( ARb[ b ], AR[ g * 16 + b ] );
					ARg[ g ] = std::min( ARg[ g ], AR[ g * 16 + b ] );
				}
			for( int r = 0; r < 16; ++r )
				for( int b = 0; b < 16; ++b )
					AGr[ r ] = std::min( AGr[ r ], AG[ r * 16 + b ] );
			for( int v = 0; v < 16; ++v )
			{
				KR = std::min( KR, er[ v ] + AGr[ v ] );
				KG = std::min( KG, eg[ v ] + ARg[ v ] );
				KB = std::min( KB, eb[ v ] + ARb[ v ] );
			}
			for( int i = 0; i < 16; ++i )
				for( int j = 0; j < 16; ++j )
				{
					int32_t t = std::min( std::min( mr + ARb[ j ], mr + ARg[ i ] ), KR );
					nAR[ i * 16 + j ] = std::min( nAR[ i * 16 + j ], clampInf( eg[ i ] + eb[ j ] + t ) );
					t = std::min( std::min( mg + ARb[ j ], KG ), mg + AGr[ i ] );
					nAG[ i * 16 + j ] = std::min( nAG[ i * 16 + j ], clampInf( er[ i ] + eb[ j ] + t ) );
					t = std::min( std::min( KB, mb + ARg[ j ] ), mb + AGr[ i ] );
					nAB[ i * 16 + j ] = std::min( nAB[ i * 16 + j ], clampInf( er[ i ] + eg[ j ] + t ) );
				}
		}

		// A palette register can follow anything.
		for( const Colour12& p : palette )
		{
			const int32_t v = clampInf( er[ p.r ] + eg[ p.g ] + eb[ p.b ] + M );
			nAR[ p.g * 16 + p.b ] = std::min( nAR[ p.g * 16 + p.b ], v );
			nAG[ p.r * 16 + p.b ] = std::min( nAG[ p.r * 16 + p.b ], v );
			nAB[ p.r * 16 + p.g ] = std::min( nAB[ p.r * 16 + p.g ], v );
		}

		int32_t m = kInf;
		for( int i = 0; i < 256; ++i )
			m = mn( m, nAR[ i ] );
		N[ kM ] = m;
	}

	//--- Backward -----------------------------------------------------------
	// The cost of being in colour c at pixel x, from the tables before x.
	auto costAt = [ & ]( int x, int r, int g, int b ) -> int32_t {
		const int32_t* P = tables.data() + static_cast< size_t >( x ) * kTableStride;
		const uint8_t* t = rgb + static_cast< size_t >( x ) * 3;
		int32_t T        = std::min( std::min( P[ kAR + g * 16 + b ], P[ kAG + r * 16 + b ] ), P[ kAB + r * 16 + g ] );
		if( inPalette[ ( r << 8 ) | ( g << 4 ) | b ] )
			T = std::min( T, P[ kM ] );
		if( twoGuns )
		{
			int32_t rb = kInf, rg = kInf, gr = kInf;
			for( int v = 0; v < 16; ++v )
			{
				rb = std::min( rb, P[ kAR + v * 16 + b ] );//min over g: two guns r,g free
				rg = std::min( rg, P[ kAR + g * 16 + v ] );//min over b: r,b free
				gr = std::min( gr, P[ kAG + r * 16 + v ] );//min over b: g,b free
			}
			T = std::min( T, std::min( rb, std::min( rg, gr ) ) );
		}
		return clampInf( pixelError( { uint8_t( r ), uint8_t( g ), uint8_t( b ) }, t ) + T );
	};

	// The best last colour: the table after the last pixel already holds the
	// best cost per (green, blue) with red free, so find that pair, then the red.
	const int32_t* last = tables.data() + static_cast< size_t >( width ) * kTableStride;
	const int32_t best  = last[ kM ];
	Colour12 c{};
	for( int i = 0; i < 256; ++i )
		if( last[ kAR + i ] == best )
		{
			c = { 0, uint8_t( i >> 4 ), uint8_t( i & 15 ) };
			break;
		}
	for( int r = 0; r < 16; ++r )
		if( costAt( width - 1, r, c.g, c.b ) == best )
		{
			c.r = uint8_t( r );
			break;
		}
	out[ width - 1 ] = c;

	for( int x = width - 1; x >= 1; --x )
	{
		const int32_t* P  = tables.data() + static_cast< size_t >( x ) * kTableStride;
		const int32_t arrive = costAt( x, c.r, c.g, c.b ) - pixelError( c, rgb + static_cast< size_t >( x ) * 3 );

		// Which way in, then which colour came before. Every search below
		// finds a colour whose cost at x-1 equals the table entry that
		// produced `arrive`, so it always succeeds for a reachable state.
		Colour12 prev = c;
		bool found    = false;
		auto findGun  = [ & ]( int gun, int32_t want ) {
			for( int v = 0; v < 16 && !found; ++v )
			{
				Colour12 k = prev;
				( gun == 0 ? k.r : gun == 1 ? k.g : k.b ) = uint8_t( v );
				if( costAt( x - 1, k.r, k.g, k.b ) == want )
				{
					prev  = k;
					found = true;
				}
			}
		};

		if( inPalette[ index12( c ) ] && arrive == P[ kM ] )
		{
			for( int i = 0; i < 256 && !found; ++i )
				if( P[ kAR + i ] == P[ kM ] )
				{
					prev = { 0, uint8_t( i >> 4 ), uint8_t( i & 15 ) };
					findGun( 0, P[ kM ] );
				}
		}
		if( !found && arrive == P[ kAR + c.g * 16 + c.b ] )
		{
			prev = c;
			findGun( 0, arrive );
		}
		if( !found && arrive == P[ kAG + c.r * 16 + c.b ] )
		{
			prev = c;
			findGun( 1, arrive );
		}
		if( !found && arrive == P[ kAB + c.r * 16 + c.g ] )
		{
			prev = c;
			findGun( 2, arrive );
		}
		if( !found && twoGuns )
		{
			// Two guns free. Hold blue, then green, then red.
			for( int v = 0; v < 16 && !found; ++v )
				if( P[ kAR + v * 16 + c.b ] == arrive )
				{
					prev = { 0, uint8_t( v ), c.b };
					findGun( 0, arrive );
				}
			for( int v = 0; v < 16 && !found; ++v )
				if( P[ kAR + c.g * 16 + v ] == arrive )
				{
					prev = { 0, c.g, uint8_t( v ) };
					findGun( 0, arrive );
				}
			for( int v = 0; v < 16 && !found; ++v )
				if( P[ kAG + c.r * 16 + v ] == arrive )
				{
					prev = { c.r, 0, uint8_t( v ) };
					findGun( 1, arrive );
				}
		}

		c            = prev;
		out[ x - 1 ] = c;
	}

	return best;
}

bool hamLegal( const Colour12* line, int width, const std::vector< Colour12 >& palette )
{
	Colour12 prev = palette.empty() ? Colour12{} : palette[ 0 ];
	for( int x = 0; x < width; ++x )
	{
		const Colour12& c = line[ x ];
		bool ok           = std::find( palette.begin(), palette.begin() + std::min< size_t >( palette.size(), 16 ), c )
		          != palette.begin() + std::min< size_t >( palette.size(), 16 );
		const int changed = ( c.r != prev.r ) + ( c.g != prev.g ) + ( c.b != prev.b );
		ok                = ok || changed <= 1;
		if( !ok )
			return false;
		prev = c;
	}
	return true;
}

int64_t encodeHamFrame( const uint8_t* rgbaIn, uint8_t* rgbaOut, int width, int height,
                        const std::vector< Colour12 >& palette, int threads, int negativeChannels )
{
	threads = std::clamp( threads, 1, std::max( 1, height ) );
	std::vector< int64_t > errors( static_cast< size_t >( threads ), 0 );

	auto work = [ & ]( int worker ) {
		HamEncoder encoder;
		std::vector< uint8_t > rgb( static_cast< size_t >( width ) * 3 );
		std::vector< Colour12 > line( static_cast< size_t >( width ) );
		for( int y = worker; y < height; y += threads )
		{
			const uint8_t* in = rgbaIn + static_cast< size_t >( y ) * width * 4;
			uint8_t* out      = rgbaOut + static_cast< size_t >( y ) * width * 4;
			for( int x = 0; x < width; ++x )
			{
				rgb[ x * 3 + 0 ] = in[ x * 4 + 0 ];
				rgb[ x * 3 + 1 ] = in[ x * 4 + 1 ];
				rgb[ x * 3 + 2 ] = in[ x * 4 + 2 ];
			}
			errors[ worker ] += encoder.encodeLine( rgb.data(), width, palette, line.data(), negativeChannels );
			for( int x = 0; x < width; ++x )
			{
				out[ x * 4 + 0 ] = static_cast< uint8_t >( to8( line[ x ].r ) );
				out[ x * 4 + 1 ] = static_cast< uint8_t >( to8( line[ x ].g ) );
				out[ x * 4 + 2 ] = static_cast< uint8_t >( to8( line[ x ].b ) );
				out[ x * 4 + 3 ] = in[ x * 4 + 3 ];
			}
		}
	};

	if( threads == 1 )
		work( 0 );
	else
	{
		std::vector< std::thread > pool;
		pool.reserve( static_cast< size_t >( threads - 1 ) );
		for( int t = 1; t < threads; ++t )
			pool.emplace_back( work, t );
		work( 0 );
		for( std::thread& t : pool )
			t.join();
	}

	int64_t total = 0;
	for( int64_t e : errors )
		total += e;
	return total;
}

} // namespace nesolume::amiga
