// The reference side of demo/tools/check_port.mjs: the plugin's own
// source/Amiga.cpp, compiled unchanged, run on the cases the script wrote.
//
//     refamiga CASES OUT
//
// CASES is a little-endian stream of records, each starting with a tag:
//   'H' width palSize palette[3*palSize] rgb[3*width]      -> HamEncoder::encodeLine
//   'P' pixels count twins nSeeds seeds[3*nSeeds] rgba[4*pixels] -> choosePalette
//   'F'                                                    -> fixedPalette, displayPalette,
//                                                             baseRegisterCount, effectiveMode
//   'T' n seconds[n](double) hz[n](int32)                  -> fieldIndex
// Every integer is an int32; colours are one byte per gun, 0..15. OUT gets, per
// record in order: 'H' the int64 cost then width*3 guns; 'P' count*3 guns;
// 'F' every table; 'T' n int64s.
#include "Amiga.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nesolume::amiga;

namespace
{
std::vector< uint8_t > in;
size_t at = 0;
std::vector< uint8_t > out;

int32_t i32()
{
	int32_t v;
	std::memcpy( &v, in.data() + at, 4 );
	at += 4;
	return v;
}
double f64()
{
	double v;
	std::memcpy( &v, in.data() + at, 8 );
	at += 8;
	return v;
}
void put( const void* p, size_t n )
{
	const uint8_t* b = static_cast< const uint8_t* >( p );
	out.insert( out.end(), b, b + n );
}
void putColours( const std::vector< Colour12 >& p )
{
	for( const Colour12& c : p )
	{
		out.push_back( c.r );
		out.push_back( c.g );
		out.push_back( c.b );
	}
}
std::vector< Colour12 > colours( int n )
{
	std::vector< Colour12 > p( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
	{
		p[ i ] = { in[ at ], in[ at + 1 ], in[ at + 2 ] };
		at += 3;
	}
	return p;
}
} // namespace

int main( int argc, char** argv )
{
	if( argc != 3 )
	{
		std::fprintf( stderr, "usage: refamiga CASES OUT\n" );
		return 2;
	}
	FILE* f = std::fopen( argv[ 1 ], "rb" );
	if( !f )
		return 2;
	std::fseek( f, 0, SEEK_END );
	in.resize( static_cast< size_t >( std::ftell( f ) ) );
	std::fseek( f, 0, SEEK_SET );
	if( std::fread( in.data(), 1, in.size(), f ) != in.size() )
		return 2;
	std::fclose( f );

	HamEncoder encoder;
	while( at < in.size() )
	{
		const char tag = static_cast< char >( in[ at++ ] );
		if( tag == 'H' )
		{
			const int width   = i32();
			const int palSize = i32();
			const std::vector< Colour12 > palette = colours( palSize );
			const uint8_t* rgb = in.data() + at;
			at += static_cast< size_t >( width ) * 3;
			std::vector< Colour12 > line( static_cast< size_t >( width ) );
			const int64_t cost = encoder.encodeLine( rgb, width, palette, line.data() );
			put( &cost, 8 );
			putColours( line );
		}
		else if( tag == 'P' )
		{
			const int pixels = i32();
			const int count  = i32();
			const bool twins = i32() != 0;
			const int nSeeds = i32();
			const std::vector< Colour12 > seeds = colours( nSeeds );
			const uint8_t* rgba = in.data() + at;
			at += static_cast< size_t >( pixels ) * 4;
			putColours( choosePalette( rgba, pixels, count, twins, seeds ) );
		}
		else if( tag == 'F' )
		{
			for( int hires = 0; hires < 2; ++hires )
				for( int mode = -1; mode <= kModeCount; ++mode )
				{
					const int32_t e = effectiveMode( mode, hires != 0 ), n = baseRegisterCount( mode, hires != 0 );
					put( &e, 4 );
					put( &n, 4 );
					const std::vector< Colour12 > base = fixedPalette( mode, hires != 0 );
					const int32_t size = static_cast< int32_t >( base.size() );
					put( &size, 4 );
					putColours( base );
					const std::vector< Colour12 > shown = displayPalette( base, e );
					const int32_t shownSize = static_cast< int32_t >( shown.size() );
					put( &shownSize, 4 );
					putColours( shown );
				}
		}
		else if( tag == 'T' )
		{
			const int n = i32();
			std::vector< double > s( static_cast< size_t >( n ) );
			for( int i = 0; i < n; ++i )
				s[ i ] = f64();
			for( int i = 0; i < n; ++i )
			{
				const int64_t k = fieldIndex( s[ i ], i32() );
				put( &k, 8 );
			}
		}
		else
		{
			std::fprintf( stderr, "refamiga: unknown record '%c' at %zu\n", tag, at - 1 );
			return 2;
		}
	}

	FILE* o = std::fopen( argv[ 2 ], "wb" );
	if( !o || std::fwrite( out.data(), 1, out.size(), o ) != out.size() )
		return 2;
	std::fclose( o );
	return 0;
}
