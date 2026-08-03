/// The OpenFX build of NESolume, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// Same model as the FFGL build: the console table lives once, in
/// Consoles.cpp, and this file links it rather than copying it. What *is*
/// mirrored here is the per-pixel machinery of the four shader stages —
/// downres box filter, tile means, clash/corruption/dither/quantise, and the
/// display stage's scroll damage — because the GPU did that work per fragment
/// and here it runs on the CPU. When editing a stage's GLSL, edit the
/// matching function here too; the palettes and rasters have only one home.
///
/// The chain runs at the console raster whatever the frame size is, so the
/// expensive stages are precomputed once per render into small buffers and
/// the per-pixel loop is only the display stage. OFX time is in frames; the
/// glitch clock wants seconds, so it divides by the clip frame rate — which
/// makes any frame render identically however the host reaches it.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Consoles.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.nesolume";
constexpr const char* kPluginName       = "NESolume";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Retro console video hardware: raster, palette, attribute cells and "
	"their glitches.\n\n"
	"Averages the picture down onto a real machine's raster, forces it "
	"through that machine's colour system with the attribute cells "
	"enforced, and scales it back up as fat pixels. Corruption happens to "
	"the indices, before colour choice, so a glitch can never leave the "
	"palette; displacement moves whole raster pixels and wraps like a "
	"scroll register.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset        = "preset";
constexpr const char* kParamConsole       = "console";
constexpr const char* kParamPixelSize     = "pixelSize";
constexpr const char* kParamColourDepth   = "colourDepth";
constexpr const char* kParamDither        = "dither";
constexpr const char* kParamClash         = "attributeClash";
constexpr const char* kParamGrid          = "pixelGrid";
constexpr const char* kParamWave          = "wave";
constexpr const char* kParamShake         = "shake";
constexpr const char* kParamBlockGlitch   = "blockGlitch";
constexpr const char* kParamLineGlitch    = "lineGlitch";
constexpr const char* kParamPaletteGlitch = "paletteGlitch";
constexpr const char* kParamGarbage       = "garbage";
constexpr const char* kParamGlitchRate    = "glitchRate";
constexpr const char* kParamMix           = "mix";

/// Everything one frame's render needs, in the units the shaders use.
struct Settings
{
	int consoleIndex     = 2;
	double pixelSize     = 0.5;
	double colourDepth   = 0.4;
	double dither        = 0.35;
	double clash         = 0.5;
	double grid          = 0.0;
	double wave          = 0.0;
	double shake         = 0.0;
	double blockGlitch   = 0.0;
	double lineGlitch    = 0.0;
	double paletteGlitch = 0.0;
	double garbage       = 0.0;
	double glitchRate    = 0.3;
	double mix           = 1.0;
	double seconds       = 0.0;
};

//---------------------------------------------------------------------------
// Mirrors of the GLSL helpers. Same constants; double where GLSL had float,
// which changes individual hash values but not their statistics — the two
// builds agree constant for constant, not bit for bit, exactly like the
// sibling ports.
//---------------------------------------------------------------------------
inline double fract( double v )
{
	return v - std::floor( v );
}

inline double hash( double x, double y )
{
	return fract( std::sin( x * 127.1 + y * 311.7 ) * 43758.5453 );
}

inline void hash2( double x, double y, double& outX, double& outY )
{
	outX = hash( x, y );
	outY = hash( x + 19.19, y + 19.19 );
}

/// GLSL mod(): x - y*floor(x/y), correct for negatives, which std::fmod isn't.
inline double glslMod( double x, double y )
{
	return x - y * std::floor( x / y );
}

constexpr double kBayer[ 16 ] = {
	0.0, 8.0, 2.0, 10.0,
	12.0, 4.0, 14.0, 6.0,
	3.0, 11.0, 1.0, 9.0,
	15.0, 7.0, 13.0, 5.0
};

constexpr double kLumaR = 0.299, kLumaG = 0.587, kLumaB = 0.114;

/// The low-resolution chain — downres, tile means, quantise — computed once
/// per render. Straight (unpremultiplied) colour throughout, like the GPU
/// buffers; alpha is already snapped to its one bit.
struct RasterChain
{
	int rasterW = 0;
	int rasterH = 0;
	int tileSize = 8;
	std::vector<double> quant;//rasterW*rasterH*4, straight RGBA
	double glitchKey = 0.0;
};

class NESolumeProcessorBase : public OFX::ImageProcessor
{
public:
	explicit NESolumeProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( OFX::Image* src, const Settings& s, bool premultipliedValue )
	{
		srcImg        = src;
		settings      = s;
		premultiplied = premultipliedValue;

		const OfxRectI b = src->getBounds();
		srcW             = b.x2 - b.x1;
		srcH             = b.y2 - b.y1;

		buildChain();
	}

protected:
	OFX::Image* srcImg = nullptr;
	Settings settings;
	bool premultiplied = false;
	int srcW           = 0;
	int srcH           = 0;
	RasterChain chain;

	/// One source texel as straight RGBA in 0..1. Must be given by the
	/// concrete PIX type, so the template below provides it.
	virtual void sourceTexel( int x, int y, double out[ 4 ] ) const = 0;

private:
	/// Stages 1–3 of the GPU chain, run once. See the matching GLSL files.
	void buildChain()
	{
		using namespace nesolume;

		const ConsoleSpec& con = console( settings.consoleIndex );

		// The raster: line count from the console scaled by Pixel Size, width
		// following the frame's aspect — same derivation as ProcessOpenGL.
		const double par = srcImg->getPixelAspectRatio() > 0.0 ? srcImg->getPixelAspectRatio() : 1.0;
		const double sizeFactor = std::exp2( ( settings.pixelSize - 0.5 ) * 4.0 );
		const int rasterH = std::clamp( int( std::lround( con.rasterHeight / sizeFactor ) ), 8, 2048 );
		const int rasterW = std::clamp( int( std::lround( rasterH * double( srcW ) * par / double( srcH ) ) ), 8, 4096 );

		chain.rasterW  = rasterW;
		chain.rasterH  = rasterH;
		chain.tileSize = con.tileSize;

		const double rateHz = settings.glitchRate * 18.0;
		chain.glitchKey     = rateHz > 0.0 ? std::floor( settings.seconds * rateHz ) : 0.0;
		const double key    = chain.glitchKey;

		//--- 1. Downres: a box the size of one raster pixel. -----------------
		//The CPU can afford the exact box where the GPU used a capped tap
		//grid; both are "average the footprint", which is the property the
		//single-pixel checkerboard in the harness card checks.
		std::vector<double> raster( size_t( rasterW ) * rasterH * 4, 0.0 );
		{
			const double ratioX = double( srcW ) / rasterW;
			const double ratioY = double( srcH ) / rasterH;
			for( int y = 0; y < rasterH; ++y )
			{
				const int y0 = int( y * ratioY );
				const int y1 = std::max( y0 + 1, std::min( int( std::ceil( ( y + 1 ) * ratioY ) ), srcH ) );
				for( int x = 0; x < rasterW; ++x )
				{
					const int x0 = int( x * ratioX );
					const int x1 = std::max( x0 + 1, std::min( int( std::ceil( ( x + 1 ) * ratioX ) ), srcW ) );

					double sum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
					for( int sy = y0; sy < y1; ++sy )
					{
						for( int sx = x0; sx < x1; ++sx )
						{
							double texel[ 4 ];
							sourceTexel( sx, sy, texel );
							//Average premultiplied — the correct filter where
							//alpha varies — then straighten below.
							sum[ 0 ] += texel[ 0 ] * texel[ 3 ];
							sum[ 1 ] += texel[ 1 ] * texel[ 3 ];
							sum[ 2 ] += texel[ 2 ] * texel[ 3 ];
							sum[ 3 ] += texel[ 3 ];
						}
					}
					const double n = double( ( x1 - x0 ) * ( y1 - y0 ) );
					double* out    = &raster[ ( size_t( y ) * rasterW + x ) * 4 ];
					const double a = sum[ 3 ] / n;
					out[ 0 ] = a > 0.0 ? sum[ 0 ] / n / a : 0.0;
					out[ 1 ] = a > 0.0 ? sum[ 1 ] / n / a : 0.0;
					out[ 2 ] = a > 0.0 ? sum[ 2 ] / n / a : 0.0;
					out[ 3 ] = a;
				}
			}
		}

		//--- 2. Tile means. --------------------------------------------------
		const int tileGridW = ( rasterW + con.tileSize - 1 ) / con.tileSize;
		const int tileGridH = ( rasterH + con.tileSize - 1 ) / con.tileSize;
		std::vector<double> tileMean( size_t( tileGridW ) * tileGridH * 3, 0.0 );
		for( int ty = 0; ty < tileGridH; ++ty )
		{
			for( int tx = 0; tx < tileGridW; ++tx )
			{
				double sum[ 3 ] = { 0.0, 0.0, 0.0 };
				int counted     = 0;
				for( int y = ty * con.tileSize; y < std::min( ( ty + 1 ) * con.tileSize, rasterH ); ++y )
				{
					for( int x = tx * con.tileSize; x < std::min( ( tx + 1 ) * con.tileSize, rasterW ); ++x )
					{
						const double* p = &raster[ ( size_t( y ) * rasterW + x ) * 4 ];
						sum[ 0 ] += p[ 0 ];
						sum[ 1 ] += p[ 1 ];
						sum[ 2 ] += p[ 2 ];
						++counted;
					}
				}
				double* out = &tileMean[ ( size_t( ty ) * tileGridW + tx ) * 3 ];
				out[ 0 ] = sum[ 0 ] / counted;
				out[ 1 ] = sum[ 1 ] / counted;
				out[ 2 ] = sum[ 2 ] / counted;
			}
		}

		//--- 3. Clash, corruption, dither, the palette. ----------------------
		//Mirrors Quantize.cpp step for step; the order is the invariant — a
		//corrupted pixel is corrupted before colour choice, so it can never
		//leave the palette.
		const bool bitsMode = con.kind == kPaletteRGBBits;
		const int bits      = con.bitsPerChannel > 0
		                          ? con.bitsPerChannel
		                          : 1 + int( std::lround( settings.colourDepth * 7.0 ) );
		const double levels = std::exp2( double( bits ) ) - 1.0;

		const auto* store = paletteStore();

		chain.quant.assign( size_t( rasterW ) * rasterH * 4, 0.0 );
		for( int y = 0; y < rasterH; ++y )
		{
			for( int x = 0; x < rasterW; ++x )
			{
				const double* src = &raster[ ( size_t( y ) * rasterW + x ) * 4 ];
				const int cx      = x / con.tileSize;
				const int cy      = y / con.tileSize;
				const double* tm  = &tileMean[ ( size_t( cy ) * tileGridW + cx ) * 3 ];

				const double luma     = src[ 0 ] * kLumaR + src[ 1 ] * kLumaG + src[ 2 ] * kLumaB;
				const double tileLuma = tm[ 0 ] * kLumaR + tm[ 1 ] * kLumaG + tm[ 2 ] * kLumaB;

				double c[ 3 ];
				for( int i = 0; i < 3; ++i )
				{
					const double chroma     = src[ i ] - luma;
					const double tileChroma = tm[ i ] - tileLuma;
					c[ i ]                  = luma + chroma + ( tileChroma - chroma ) * settings.clash;
				}

				const double corrupt = hash( cx + key * 13.7, cy + key );
				if( corrupt < settings.paletteGlitch * 0.5 )
				{
					const double flavour = hash( cx + 5.0, cy + key );
					if( flavour < 0.4 )
					{
						const double t = c[ 0 ];//rgb -> brg
						c[ 0 ] = c[ 2 ];
						c[ 2 ] = c[ 1 ];
						c[ 1 ] = t;
					}
					else if( flavour < 0.8 )
					{
						const double t = c[ 0 ];//rgb -> gbr
						c[ 0 ] = c[ 1 ];
						c[ 1 ] = c[ 2 ];
						c[ 2 ] = t;
					}
					else
					{
						c[ 0 ] = 1.0 - c[ 0 ];
						c[ 1 ] = 1.0 - c[ 1 ];
						c[ 2 ] = 1.0 - c[ 2 ];
					}
				}

				const double junk = hash( cx + key, cy + 71.3 );
				if( junk < settings.garbage * 0.35 )
				{
					c[ 0 ] = hash( x + key * 7.1, y + key * 3.3 );
					c[ 1 ] = hash( x + key * 7.1 + 19.7, y + key * 3.3 + 19.7 );
					c[ 2 ] = hash( x + key * 7.1 + 41.1, y + key * 3.3 + 41.1 );
				}

				const double bayer = ( kBayer[ ( y % 4 ) * 4 + ( x % 4 ) ] + 0.5 ) / 16.0 - 0.5;

				double* out = &chain.quant[ ( size_t( y ) * rasterW + x ) * 4 ];
				if( bitsMode )
				{
					for( int i = 0; i < 3; ++i )
					{
						const double dithered = std::clamp( c[ i ] + bayer * settings.dither / levels, 0.0, 1.0 );
						out[ i ]              = std::floor( dithered * levels + 0.5 ) / levels;
					}
				}
				else
				{
					double dithered[ 3 ];
					for( int i = 0; i < 3; ++i )
						dithered[ i ] = std::clamp( c[ i ] + bayer * settings.dither * 0.22, 0.0, 1.0 );

					double best   = 1e9;
					int bestEntry = 0;
					for( int i = 0; i < con.paletteCount; ++i )
					{
						const double dr = dithered[ 0 ] - store[ con.paletteFirst + i ][ 0 ] / 255.0;
						const double dg = dithered[ 1 ] - store[ con.paletteFirst + i ][ 1 ] / 255.0;
						const double db = dithered[ 2 ] - store[ con.paletteFirst + i ][ 2 ] / 255.0;
						const double dist = dr * dr * kLumaR + dg * dg * kLumaG + db * db * kLumaB;
						if( dist < best )
						{
							best      = dist;
							bestEntry = i;
						}
					}
					out[ 0 ] = store[ con.paletteFirst + bestEntry ][ 0 ] / 255.0;
					out[ 1 ] = store[ con.paletteFirst + bestEntry ][ 1 ] / 255.0;
					out[ 2 ] = store[ con.paletteFirst + bestEntry ][ 2 ] / 255.0;
				}

				out[ 3 ] = src[ 3 ] > 0.5 ? 1.0 : 0.0;
			}
		}
	}
};

template<class PIX, int nComponents, int maxValue>
class NESolumeProcessor : public NESolumeProcessorBase
{
public:
	explicit NESolumeProcessor( OFX::ImageEffect& effect ) :
		NESolumeProcessorBase( effect )
	{
	}

	/// Stage 4 — the display pass — per output pixel. Mirrors Display.cpp:
	/// scroll damage in whole raster pixels, wrap, nearest fetch, grid, mix.
	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI dstBounds = _dstImg->getBounds();
		const double outW        = double( dstBounds.x2 - dstBounds.x1 );
		const double outH        = double( dstBounds.y2 - dstBounds.y1 );

		const int rasterW  = chain.rasterW;
		const int rasterH  = chain.rasterH;
		const double key   = chain.glitchKey;
		const double tSize = double( chain.tileSize );

		const double waveAmp = settings.wave * settings.wave * rasterW * 0.12;
		double knockX, knockY;
		hash2( key + 7.0, key * 3.1, knockX, knockY );
		const double shakeX = std::floor( ( knockX - 0.5 ) * 2.0 * settings.shake * settings.shake * rasterW * 0.10 + 0.5 );
		const double shakeY = std::floor( ( knockY - 0.5 ) * 2.0 * settings.shake * settings.shake * rasterH * 0.10 + 0.5 );

		const double w = std::max( rasterW / outW, rasterH / outH );
		const double lineHalf   = 0.05 + 0.10 * settings.grid;
		const double resolvable = std::clamp( ( 0.45 - w ) / 0.25, 0.0, 1.0 );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				double rcx = ( x - dstBounds.x1 + 0.5 ) / outW * rasterW;
				double rcy = ( y - dstBounds.y1 + 0.5 ) / outH * rasterH;

				//Wave, per line, snapped to whole pixels.
				rcx += std::floor( std::sin( rcy / rasterH * 18.85 + settings.seconds * 2.1 ) * waveAmp + 0.5 );

				//Shake, whole frame.
				rcx += shakeX;
				rcy += shakeY;

				//Torn lines.
				const double band = std::floor( rcy / 4.0 );
				if( hash( band, key ) < settings.lineGlitch * 0.4 )
				{
					const double tear = ( hash( band + 31.0, key ) - 0.5 ) * 2.0;
					rcx += std::floor( tear * settings.lineGlitch * rasterW * 0.3 + 0.5 );
				}

				//Displaced blocks, whole tiles.
				const double tileX = std::floor( rcx / tSize );
				const double tileY = std::floor( rcy / tSize );
				if( hash( tileX + key * 5.3, tileY + key ) < settings.blockGlitch * 0.35 )
				{
					double jumpX, jumpY;
					hash2( tileX + 43.7, tileY + key, jumpX, jumpY );
					rcx += std::floor( ( jumpX - 0.5 ) * settings.blockGlitch * 12.0 ) * tSize;
					rcy += std::floor( ( jumpY - 0.5 ) * settings.blockGlitch * 12.0 ) * tSize;
				}

				//Wrap like a scroll register, fetch nearest.
				rcx = glslMod( rcx, double( rasterW ) );
				rcy = glslMod( rcy, double( rasterH ) );
				const int ix = std::clamp( int( rcx ), 0, rasterW - 1 );
				const int iy = std::clamp( int( rcy ), 0, rasterH - 1 );
				const double* q = &chain.quant[ ( size_t( iy ) * rasterW + ix ) * 4 ];

				double r = q[ 0 ], g = q[ 1 ], b = q[ 2 ];
				const double a = q[ 3 ];

				//The grid between fat pixels.
				if( settings.grid > 0.0 && resolvable > 0.0 )
				{
					const double fx     = fract( rcx );
					const double fy     = fract( rcy );
					const double toEdge = std::min( std::min( fx, 1.0 - fx ), std::min( fy, 1.0 - fy ) );
					const double onLine = 1.0 - std::clamp( ( toEdge - lineHalf ) / w, 0.0, 1.0 );
					const double darken = 1.0 - settings.grid * onLine * resolvable * 0.85;
					r *= darken;
					g *= darken;
					b *= darken;
				}

				//Mix premultiplied, like the shader, then match the dst's
				//alpha convention.
				double original[ 4 ];
				sourceTexel( x - dstBounds.x1, y - dstBounds.y1, original );
				const double m = settings.mix;

				double outR = original[ 0 ] * original[ 3 ] + ( r * a - original[ 0 ] * original[ 3 ] ) * m;
				double outG = original[ 1 ] * original[ 3 ] + ( g * a - original[ 1 ] * original[ 3 ] ) * m;
				double outB = original[ 2 ] * original[ 3 ] + ( b * a - original[ 2 ] * original[ 3 ] ) * m;
				double outA = original[ 3 ] + ( a - original[ 3 ] ) * m;

				if( premultiplied || nComponents == 3 )
				{
					outR = std::min( outR, outA );
					outG = std::min( outG, outA );
					outB = std::min( outB, outA );
				}
				else if( outA > 0.0 )
				{
					outR /= outA;
					outG /= outA;
					outB /= outA;
				}

				dstPix[ 0 ] = quantise( outR );
				dstPix[ 1 ] = quantise( outG );
				dstPix[ 2 ] = quantise( outB );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( outA );
			}
		}
	}

private:
	/// One source texel as STRAIGHT RGBA in 0..1, (0,0) at the image origin.
	void sourceTexel( int x, int y, double out[ 4 ] ) const override
	{
		const OfxRectI b = srcImg->getBounds();
		x                = std::clamp( x, 0, srcW - 1 );
		y                = std::clamp( y, 0, srcH - 1 );
		const PIX* srcPix = static_cast<const PIX*>( srcImg->getPixelAddress( b.x1 + x, b.y1 + y ) );
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( premultiplied && nComponents == 4 && out[ 3 ] > 0.0 )
		{
			out[ 0 ] /= out[ 3 ];
			out[ 1 ] /= out[ 3 ];
			out[ 2 ] /= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class NESolumePlugin : public OFX::ImageEffect
{
public:
	explicit NESolumePlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip       = fetchClip( kOfxImageEffectOutputClipName );
		srcClip       = fetchClip( kOfxImageEffectSimpleSourceClipName );
		preset        = fetchChoiceParam( kParamPreset );
		consoleParam  = fetchChoiceParam( kParamConsole );
		pixelSize     = fetchDoubleParam( kParamPixelSize );
		colourDepth   = fetchDoubleParam( kParamColourDepth );
		dither        = fetchDoubleParam( kParamDither );
		clash         = fetchDoubleParam( kParamClash );
		grid          = fetchDoubleParam( kParamGrid );
		wave          = fetchDoubleParam( kParamWave );
		shake         = fetchDoubleParam( kParamShake );
		blockGlitch   = fetchDoubleParam( kParamBlockGlitch );
		lineGlitch    = fetchDoubleParam( kParamLineGlitch );
		paletteGlitch = fetchDoubleParam( kParamPaletteGlitch );
		garbage       = fetchDoubleParam( kParamGarbage );
		glitchRate    = fetchDoubleParam( kParamGlitchRate );
		mixParam      = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src( srcClip->fetchImage( args.time ) );

		const Settings s = settingsAtTime( args.time );
		const bool premultiplied =
			srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run<NESolumeProcessor<unsigned char, 4, 255>>( args, dst.get(), src.get(), s, premultiplied )
				: run<NESolumeProcessor<unsigned char, 3, 255>>( args, dst.get(), src.get(), s, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run<NESolumeProcessor<unsigned short, 4, 65535>>( args, dst.get(), src.get(), s, premultiplied )
				: run<NESolumeProcessor<unsigned short, 3, 65535>>( args, dst.get(), src.get(), s, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run<NESolumeProcessor<float, 4, 1>>( args, dst.get(), src.get(), s, premultiplied )
				: run<NESolumeProcessor<float, 3, 1>>( args, dst.get(), src.get(), s, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		using namespace nesolume::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset — same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setIfChanged( consoleParam, p.v[ kConsole ] );
			setIfChanged( pixelSize, p.v[ kPixelSize ] );
			setIfChanged( colourDepth, p.v[ kColourDepth ] );
			setIfChanged( dither, p.v[ kDither ] );
			setIfChanged( clash, p.v[ kClash ] );
			setIfChanged( grid, p.v[ kGrid ] );
			setIfChanged( wave, p.v[ kWave ] );
			setIfChanged( shake, p.v[ kShake ] );
			setIfChanged( blockGlitch, p.v[ kBlockGlitch ] );
			setIfChanged( lineGlitch, p.v[ kLineGlitch ] );
			setIfChanged( paletteGlitch, p.v[ kPaletteGlitch ] );
			setIfChanged( garbage, p.v[ kGarbage ] );
			setIfChanged( glitchRate, p.v[ kGlitchRate ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamConsole && differs( consoleParam, p.v[ kConsole ] ) ) ||
			( paramName == kParamPixelSize && differs( pixelSize, p.v[ kPixelSize ] ) ) ||
			( paramName == kParamColourDepth && differs( colourDepth, p.v[ kColourDepth ] ) ) ||
			( paramName == kParamDither && differs( dither, p.v[ kDither ] ) ) ||
			( paramName == kParamClash && differs( clash, p.v[ kClash ] ) ) ||
			( paramName == kParamGrid && differs( grid, p.v[ kGrid ] ) ) ||
			( paramName == kParamWave && differs( wave, p.v[ kWave ] ) ) ||
			( paramName == kParamShake && differs( shake, p.v[ kShake ] ) ) ||
			( paramName == kParamBlockGlitch && differs( blockGlitch, p.v[ kBlockGlitch ] ) ) ||
			( paramName == kParamLineGlitch && differs( lineGlitch, p.v[ kLineGlitch ] ) ) ||
			( paramName == kParamPaletteGlitch && differs( paletteGlitch, p.v[ kPaletteGlitch ] ) ) ||
			( paramName == kParamGarbage && differs( garbage, p.v[ kGarbage ] ) ) ||
			( paramName == kParamGlitchRate && differs( glitchRate, p.v[ kGlitchRate ] ) );

		if( covered )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		// Mix at zero is the untouched input, whatever the machine is doing.
		if( mixParam->getValueAtTime( args.time ) <= 0.0 )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

private:
	static void setIfChanged( OFX::DoubleParam* p, float v )
	{
		if( differs( p, v ) )
			p->setValue( double( v ) );
	}
	static void setIfChanged( OFX::ChoiceParam* p, float v )
	{
		if( differs( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}
	static bool differs( OFX::DoubleParam* p, float v )
	{
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool differs( OFX::ChoiceParam* p, float v )
	{
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}

	Settings settingsAtTime( double t ) const
	{
		Settings s;
		int consoleValue = 2;
		consoleParam->getValueAtTime( t, consoleValue );
		s.consoleIndex  = consoleValue;
		s.pixelSize     = pixelSize->getValueAtTime( t );
		s.colourDepth   = colourDepth->getValueAtTime( t );
		s.dither        = dither->getValueAtTime( t );
		s.clash         = clash->getValueAtTime( t );
		s.grid          = grid->getValueAtTime( t );
		s.wave          = wave->getValueAtTime( t );
		s.shake         = shake->getValueAtTime( t );
		s.blockGlitch   = blockGlitch->getValueAtTime( t );
		s.lineGlitch    = lineGlitch->getValueAtTime( t );
		s.paletteGlitch = paletteGlitch->getValueAtTime( t );
		s.garbage       = garbage->getValueAtTime( t );
		s.glitchRate    = glitchRate->getValueAtTime( t );
		s.mix           = mixParam->getValueAtTime( t );

		// OFX time is the timeline frame; the glitch clock wants seconds, so
		// any frame renders identically however the host reaches it.
		const double fps = srcClip->getFrameRate() > 0.0 ? srcClip->getFrameRate() : 30.0;
		s.seconds        = t / fps;
		return s;
	}

	template<class Processor>
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
			  const Settings& s, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( src, s, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip               = nullptr;
	OFX::Clip* srcClip               = nullptr;
	OFX::ChoiceParam* preset         = nullptr;
	OFX::ChoiceParam* consoleParam   = nullptr;
	OFX::DoubleParam* pixelSize      = nullptr;
	OFX::DoubleParam* colourDepth    = nullptr;
	OFX::DoubleParam* dither         = nullptr;
	OFX::DoubleParam* clash          = nullptr;
	OFX::DoubleParam* grid           = nullptr;
	OFX::DoubleParam* wave           = nullptr;
	OFX::DoubleParam* shake          = nullptr;
	OFX::DoubleParam* blockGlitch    = nullptr;
	OFX::DoubleParam* lineGlitch     = nullptr;
	OFX::DoubleParam* paletteGlitch  = nullptr;
	OFX::DoubleParam* garbage        = nullptr;
	OFX::DoubleParam* glitchRate     = nullptr;
	OFX::DoubleParam* mixParam       = nullptr;

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( NESolumePluginFactory, {}, {} );

void NESolumePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The raster is derived from the whole frame and the display stage reads
	// anywhere in it (a torn line reaches across the picture), so no tiles;
	// frames are still independent of each other and of render order.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void NESolumePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so
	// the two inspectors read identically and the docs cover both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Factory machines and failures. Picking one sets the controls; "
	                      "editing any of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < nesolume::presets::kCount; ++i )
		presetParam->appendOption( nesolume::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* picture = desc.defineGroupParam( "Picture" );
	picture->setLabels( "Picture", "Picture", "Picture" );

	OFX::ChoiceParamDescriptor* consoleChoice = desc.defineChoiceParam( kParamConsole );
	consoleChoice->setLabels( "Console", "Console", "Console" );
	consoleChoice->setHint( "The machine: sets the raster, the colour system and the attribute cell size." );
	for( int i = 0; i < nesolume::consoleCount(); ++i )
		consoleChoice->appendOption( nesolume::console( i ).name );
	consoleChoice->setDefault( 2 );//NES
	consoleChoice->setParent( *picture );
	page->addChild( *consoleChoice );

	defineSlider( desc, page, kParamPixelSize, "Pixel Size",
	              "Raster scale. 0.5 is the console's native raster; up is chunkier.", 0.5 )
		->setParent( *picture );
	defineSlider( desc, page, kParamColourDepth, "Colour Depth",
	              "The Custom console's DAC, 1-8 bits per channel. The real consoles "
	              "ignore it - their depth is in the table.",
	              0.4 )
		->setParent( *picture );
	defineSlider( desc, page, kParamDither, "Dither",
	              "Ordered 4x4 Bayer, scaled to the quantisation step.", 0.35 )
		->setParent( *picture );
	defineSlider( desc, page, kParamClash, "Attribute Clash",
	              "How strictly a cell shares its colours. 0 is per-pixel freedom; "
	              "1 is one hue per cell, Spectrum-style.",
	              0.5 )
		->setParent( *picture );
	defineSlider( desc, page, kParamGrid, "Pixel Grid",
	              "The dark boundary between fat pixels - an LCD's cell gaps. Fades "
	              "itself out when pixels get too small on screen to have one.",
	              0.0 )
		->setParent( *picture );

	OFX::GroupParamDescriptor* distortion = desc.defineGroupParam( "Distortion" );
	distortion->setLabels( "Distortion", "Distortion", "Distortion" );

	defineSlider( desc, page, kParamWave, "Wave",
	              "A sine on the horizontal scroll, per line, snapped to whole pixels.", 0.0 )
		->setParent( *distortion );
	defineSlider( desc, page, kParamShake, "Shake",
	              "The whole frame knocked off its sync, re-rolled on the glitch clock.", 0.0 )
		->setParent( *distortion );

	OFX::GroupParamDescriptor* glitch = desc.defineGroupParam( "Glitch" );
	glitch->setLabels( "Glitch", "Glitch", "Glitch" );

	defineSlider( desc, page, kParamBlockGlitch, "Block Glitch",
	              "Tile pointers reading the wrong address: whole attribute cells "
	              "displaced by whole cells.",
	              0.0 )
		->setParent( *glitch );
	defineSlider( desc, page, kParamLineGlitch, "Line Glitch",
	              "Bands whose scroll register read back garbage: horizontal tears.", 0.0 )
		->setParent( *glitch );
	defineSlider( desc, page, kParamPaletteGlitch, "Palette Glitch",
	              "CRAM corruption: cells with their channels rotated or inverted "
	              "before quantisation, so the result is wrong but legal.",
	              0.0 )
		->setParent( *glitch );
	defineSlider( desc, page, kParamGarbage, "Garbage",
	              "Tile pointers into memory that never held graphics: noise tiles, "
	              "quantised like everything else.",
	              0.0 )
		->setParent( *glitch );
	defineSlider( desc, page, kParamGlitchRate, "Glitch Rate",
	              "How often the machine's luck changes. At 0 the corruption is a "
	              "still - a crashed machine, not a screensaver.",
	              0.3 )
		->setParent( *glitch );

	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	defineSlider( desc, page, kParamMix, "Mix",
	              "Wet/dry against the untouched input.", 1.0 )
		->setParent( *output );
}

OFX::ImageEffect* NESolumePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new NESolumePlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static NESolumePluginFactory* factory =
		new NESolumePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
