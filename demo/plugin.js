/**
 * NESolume — browser demo.
 *
 * `VERTEX` and the four fragment shaders are the plugin's own GLSL from
 * `source/shaders/`, copied across unedited. The CONSOLES table below is a
 * second copy of `source/Consoles.cpp` — the demo cannot include a C++ file,
 * and *nothing enforces that they agree*. Change a palette or a raster in the
 * plugin and change it here too, or the page quietly goes on rendering the
 * old machine.
 *
 * The chain is the plugin's: downres to the console raster, tile means,
 * quantise, display. The two invariants ride along with it — corruption
 * happens before colour choice so a glitch cannot leave the palette, and
 * displacement moves whole raster pixels — because they are properties of
 * the shaders, not of the host around them.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/shaders/*.cpp
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
`;

const DOWNRES = `#version 410 core
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
`;

const TILE = `#version 410 core
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
`;

const QUANTIZE = `#version 410 core
uniform sampler2D SourceTexture;
uniform sampler2D TileTexture;
uniform vec2 MaxUV;
uniform vec2 RasterSize;
uniform float TileSize;

uniform float PaletteMode;//0 = fixed master palette, 1 = n bits per channel
uniform float Bits;
uniform vec3 Palette[ 64 ];
uniform int PaletteCount;

uniform float Dither;
uniform float Clash;
uniform float PaletteGlitch;
uniform float Garbage;
uniform float GlitchKey;

in vec2 uv;

out vec4 fragColor;

const vec3 kLumaWeights = vec3( 0.299, 0.587, 0.114 );

const float kBayer[ 16 ] = float[ 16 ](
	 0.0,  8.0,  2.0, 10.0,
	12.0,  4.0, 14.0,  6.0,
	 3.0, 11.0,  1.0,  9.0,
	15.0,  7.0, 13.0,  5.0 );

float hash( vec2 p )
{
	return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

void main()
{
	ivec2 px = ivec2( clamp( uv * RasterSize, vec2( 0.0 ), RasterSize - 1.0 ) );
	vec4 source = texelFetch( SourceTexture, px, 0 );

	ivec2 cell = ivec2( vec2( px ) / TileSize );
	vec3 cellMean = texelFetch( TileTexture, cell, 0 ).rgb;

	//--- 1. Clash: own luminance, the cell's chroma. -----------------------
	float luma = dot( source.rgb, kLumaWeights );
	vec3 chroma = source.rgb - luma;
	vec3 cellChroma = cellMean - dot( cellMean, kLumaWeights );
	vec3 color = luma + mix( chroma, cellChroma, Clash );

	//--- 2. Corruption, while the colour is still an index-to-be. ----------
	vec2 cellId = vec2( cell );
	float corrupt = hash( cellId + vec2( GlitchKey * 13.7, GlitchKey ) );
	if( corrupt < PaletteGlitch * 0.5 )
	{
		//Three flavours of wrong CRAM, picked per cell: two channel rotations
		//and an inversion.
		float flavour = hash( cellId + vec2( 5.0, GlitchKey ) );
		if( flavour < 0.4 )
			color = color.brg;
		else if( flavour < 0.8 )
			color = color.gbr;
		else
			color = vec3( 1.0 ) - color;
	}

	float junk = hash( cellId + vec2( GlitchKey, 71.3 ) );
	if( junk < Garbage * 0.35 )
	{
		//A tile pointer into memory that never held graphics: per-pixel noise,
		//stable within one glitch interval, quantised like everything else.
		vec2 noiseSeed = vec2( px ) + vec2( GlitchKey * 7.1, GlitchKey * 3.3 );
		color = vec3( hash( noiseSeed ), hash( noiseSeed + 19.7 ), hash( noiseSeed + 41.1 ) );
	}

	//--- 3. Dither, then the choice. ---------------------------------------
	float bayer = ( kBayer[ ( px.y % 4 ) * 4 + ( px.x % 4 ) ] + 0.5 ) / 16.0 - 0.5;

	vec3 quantised;
	if( PaletteMode > 0.5 )
	{
		float levels = exp2( Bits ) - 1.0;
		vec3 dithered = clamp( color + bayer * Dither / levels, 0.0, 1.0 );
		quantised = floor( dithered * levels + 0.5 ) / levels;
	}
	else
	{
		//The dither amplitude for a master palette: there is no uniform step,
		//so use a fraction of the range that in practice spans neighbouring
		//ramp entries on all four fixed palettes.
		vec3 dithered = clamp( color + bayer * Dither * 0.22, 0.0, 1.0 );

		float best = 1e9;
		quantised = Palette[ 0 ];
		for( int i = 0; i < PaletteCount; ++i )
		{
			vec3 d = dithered - Palette[ i ];
			float dist = dot( d * d, kLumaWeights );
			if( dist < best )
			{
				best = dist;
				quantised = Palette[ i ];
			}
		}
	}

	//--- Alpha is a bit. ----------------------------------------------------
	fragColor = vec4( quantised, source.a > 0.5 ? 1.0 : 0.0 );
}
`;

const DISPLAY = `#version 410 core
uniform sampler2D QuantTexture;
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputMaxUV;
uniform vec2 RasterSize;
uniform float TileSize;
uniform vec2 OutputSize;

uniform float Wave;
uniform float Shake;
uniform float LineGlitch;
uniform float BlockGlitch;
uniform float GlitchKey;
uniform float Time;

uniform float Grid;
uniform float Mix;

in vec2 uv;

out vec4 fragColor;

float hash( vec2 p )
{
	return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

vec2 hash2( vec2 p )
{
	return vec2( hash( p ), hash( p + 19.19 ) );
}

void main()
{
	vec2 rc = uv * RasterSize;

	//--- Wave: a sine on the horizontal scroll, per line. -------------------
	//Squared so the first half of the control is subtle. Snapped to whole
	//pixels: fine scroll had no fractional bits.
	float waveAmp = Wave * Wave * RasterSize.x * 0.12;
	rc.x += floor( sin( rc.y / RasterSize.y * 18.85 + Time * 2.1 ) * waveAmp + 0.5 );

	//--- Shake: the whole frame knocked off its sync. -----------------------
	vec2 knock = ( hash2( vec2( GlitchKey + 7.0, GlitchKey * 3.1 ) ) - 0.5 ) * 2.0;
	rc += floor( knock * Shake * Shake * RasterSize * 0.10 + 0.5 );

	//--- Torn lines: bands whose scroll register read back garbage. ---------
	float band = floor( rc.y / 4.0 );
	if( hash( vec2( band, GlitchKey ) ) < LineGlitch * 0.4 )
	{
		float tear = ( hash( vec2( band + 31.0, GlitchKey ) ) - 0.5 ) * 2.0;
		rc.x += floor( tear * LineGlitch * RasterSize.x * 0.3 + 0.5 );
	}

	//--- Displaced blocks: tile pointers reading the wrong address. ---------
	vec2 tile = floor( rc / TileSize );
	if( hash( tile + vec2( GlitchKey * 5.3, GlitchKey ) ) < BlockGlitch * 0.35 )
	{
		vec2 jump = hash2( tile + vec2( 43.7, GlitchKey ) ) - 0.5;
		rc += floor( jump * BlockGlitch * 12.0 ) * TileSize;
	}

	//--- Fetch, wrapped like a scroll register. -----------------------------
	rc = mod( rc, RasterSize );
	ivec2 ip = ivec2( clamp( rc, vec2( 0.0 ), RasterSize - 1.0 ) );
	vec4 quantised = texelFetch( QuantTexture, ip, 0 );

	//--- The grid between fat pixels. ---------------------------------------
	//w is raster pixels per output pixel: above ~0.4 a raster pixel is only a
	//couple of output pixels and a boundary line would just darken the whole
	//picture, so the grid fades itself out.
	float w = max( RasterSize.x / OutputSize.x, RasterSize.y / OutputSize.y );
	vec2 toEdge = min( fract( rc ), 1.0 - fract( rc ) );
	float edgeDistance = min( toEdge.x, toEdge.y );
	float lineHalf = 0.05 + 0.10 * Grid;
	float onLine = 1.0 - smoothstep( lineHalf, lineHalf + w, edgeDistance );
	float resolvable = smoothstep( 0.45, 0.2, w );
	quantised.rgb *= 1.0 - Grid * onLine * resolvable * 0.85;

	//--- Back to premultiplied, and the operator's mix. ---------------------
	vec4 effect = vec4( quantised.rgb * quantised.a, quantised.a );
	vec4 original = texture( InputTexture, uv * InputMaxUV );
	fragColor = mix( original, effect, Mix );
}
`;

//---------------------------------------------------------------------------
// Second copy of source/Consoles.cpp. Edit both or the page renders the old
// machine. Palettes are 8-bit triples exactly as in the C++.
//---------------------------------------------------------------------------

const GAME_BOY = [
  [15, 56, 15], [48, 98, 48], [139, 172, 15], [155, 188, 15],
];

const NES = [
  [84, 84, 84], [0, 30, 116], [8, 16, 144], [48, 0, 136],
  [68, 0, 100], [92, 0, 48], [84, 4, 0], [60, 24, 0],
  [32, 42, 0], [8, 58, 0], [0, 64, 0], [0, 60, 0],
  [0, 50, 60], [0, 0, 0], [0, 0, 0], [0, 0, 0],
  [152, 150, 152], [8, 76, 196], [48, 50, 236], [92, 30, 228],
  [136, 20, 176], [160, 20, 100], [152, 34, 32], [120, 60, 0],
  [84, 90, 0], [40, 114, 0], [8, 124, 0], [0, 118, 40],
  [0, 102, 120], [0, 0, 0], [0, 0, 0], [0, 0, 0],
  [236, 238, 236], [76, 154, 236], [120, 124, 236], [176, 98, 236],
  [228, 84, 236], [236, 88, 180], [236, 106, 100], [212, 136, 32],
  [160, 170, 0], [116, 196, 0], [76, 208, 32], [56, 204, 108],
  [56, 180, 204], [60, 60, 60], [0, 0, 0], [0, 0, 0],
  [236, 238, 236], [168, 204, 236], [188, 188, 236], [212, 178, 236],
  [236, 174, 236], [236, 174, 212], [236, 180, 176], [228, 196, 144],
  [204, 210, 120], [180, 222, 120], [168, 226, 144], [152, 226, 180],
  [160, 214, 228], [160, 162, 160], [0, 0, 0], [0, 0, 0],
];

const SPECTRUM = [
  [0, 0, 0], [0, 0, 215], [215, 0, 0], [215, 0, 215],
  [0, 215, 0], [0, 215, 215], [215, 215, 0], [215, 215, 215],
  [0, 0, 255], [255, 0, 0], [255, 0, 255], [0, 255, 0],
  [0, 255, 255], [255, 255, 0], [255, 255, 255],
];

const C64 = [
  [0, 0, 0], [255, 255, 255], [136, 57, 50], [103, 182, 189],
  [139, 63, 150], [85, 160, 73], [64, 49, 141], [191, 206, 114],
  [139, 84, 41], [87, 66, 0], [184, 105, 98], [80, 80, 80],
  [120, 120, 120], [148, 224, 122], [120, 105, 196], [159, 159, 159],
];

const CONSOLES = [
  { name: 'Custom', rasterHeight: 240, tileSize: 8, bits: 0 },
  { name: 'Game Boy', rasterHeight: 144, tileSize: 8, palette: GAME_BOY },
  { name: 'NES', rasterHeight: 240, tileSize: 16, palette: NES },
  { name: 'ZX Spectrum', rasterHeight: 192, tileSize: 8, palette: SPECTRUM },
  { name: 'Commodore 64', rasterHeight: 200, tileSize: 8, palette: C64 },
  { name: 'Master System', rasterHeight: 192, tileSize: 8, bits: 2 },
  { name: 'Mega Drive', rasterHeight: 224, tileSize: 8, bits: 3 },
  { name: 'SNES', rasterHeight: 224, tileSize: 8, bits: 5 },
  { name: 'PlayStation', rasterHeight: 240, tileSize: 8, bits: 5 },
];

const sizeFactorFromParam = (v) => 2 ** ((v - 0.5) * 4);
const bitsFromParam = (v) => 1 + Math.round(v * 7);

//---------------------------------------------------------------------------
// The renderer: the four passes, exactly as ProcessOpenGL runs them.
//---------------------------------------------------------------------------

function createRenderer(gl, quad) {
  const downres = new Program(gl, VERTEX, DOWNRES, 'downres');
  const tile = new Program(gl, VERTEX, TILE, 'tile');
  const quantize = new Program(gl, VERTEX, QUANTIZE, 'quantize');
  const display = new Program(gl, VERTEX, DISPLAY, 'display');

  const downresBuffer = new PassBuffer(gl, { filter: 'linear' });
  const tileBuffer = new PassBuffer(gl, { filter: 'nearest' });
  const quantBuffer = new PassBuffer(gl, { filter: 'nearest' });

  const paletteData = new Float32Array(64 * 3);

  return {
    render({ input, params, width, height, time }) {
      const con = CONSOLES[Math.round(params.get('console'))] ?? CONSOLES[0];

      const factor = sizeFactorFromParam(params.get('pixelSize'));
      const rasterH = Math.min(2048, Math.max(8, Math.round(con.rasterHeight / factor)));
      const rasterW = Math.min(4096, Math.max(8, Math.round((rasterH * width) / height)));
      const tileGridW = Math.ceil(rasterW / con.tileSize);
      const tileGridH = Math.ceil(rasterH / con.tileSize);

      downresBuffer.ensure(rasterW, rasterH, gl.RGBA8);
      tileBuffer.ensure(tileGridW, tileGridH, gl.RGBA16F);
      quantBuffer.ensure(rasterW, rasterH, gl.RGBA8);

      const rateHz = params.get('glitchRate') * 18;
      const glitchKey = rateHz > 0 ? Math.floor(time * rateHz) : 0;

      // 1. Down onto the console's raster.
      downresBuffer.bind();
      downres.use();
      bindTexture(gl, 0, input.texture);
      downres.setSampler('InputTexture', 0);
      downres.set('MaxUV', 1, 1);
      downres.set('InputSize', input.width, input.height);
      downres.set('TargetSize', rasterW, rasterH);
      quad.draw();

      // 2. One texel per attribute cell.
      tileBuffer.bind();
      tile.use();
      bindTexture(gl, 0, downresBuffer.texture);
      tile.setSampler('SourceTexture', 0);
      tile.set('MaxUV', 1, 1);
      tile.set('RasterSize', rasterW, rasterH);
      tile.set('TileGridSize', tileGridW, tileGridH);
      tile.set('TileSize', con.tileSize);
      quad.draw();

      // 3. Through the palette.
      quantBuffer.bind();
      quantize.use();
      bindTexture(gl, 0, downresBuffer.texture);
      bindTexture(gl, 1, tileBuffer.texture);
      quantize.setSampler('SourceTexture', 0);
      quantize.setSampler('TileTexture', 1);
      quantize.set('MaxUV', 1, 1);
      quantize.set('RasterSize', rasterW, rasterH);
      quantize.set('TileSize', con.tileSize);

      const fixed = Array.isArray(con.palette);
      quantize.set('PaletteMode', fixed ? 0 : 1);
      quantize.set('Bits', con.bits > 0 ? con.bits : bitsFromParam(params.get('colourDepth')));
      paletteData.fill(0);
      if (fixed) {
        for (let i = 0; i < con.palette.length; i += 1) {
          paletteData[i * 3 + 0] = con.palette[i][0] / 255;
          paletteData[i * 3 + 1] = con.palette[i][1] / 255;
          paletteData[i * 3 + 2] = con.palette[i][2] / 255;
        }
      }
      quantize.setArray('Palette', paletteData, 3);
      quantize.setInt('PaletteCount', fixed ? con.palette.length : 1);

      quantize.set('Dither', params.get('dither'));
      quantize.set('Clash', params.get('clash'));
      quantize.set('PaletteGlitch', params.get('paletteGlitch'));
      quantize.set('Garbage', params.get('garbage'));
      quantize.set('GlitchKey', glitchKey);
      quad.draw();

      // 4. Back up to the composition, damaged on the way.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      display.use();
      bindTexture(gl, 0, quantBuffer.texture);
      bindTexture(gl, 1, input.texture);
      display.setSampler('QuantTexture', 0);
      display.setSampler('InputTexture', 1);
      display.set('MaxUV', 1, 1);
      display.set('InputMaxUV', 1, 1);
      display.set('RasterSize', rasterW, rasterH);
      display.set('TileSize', con.tileSize);
      display.set('OutputSize', width, height);

      display.set('Wave', params.get('wave'));
      display.set('Shake', params.get('shake'));
      display.set('LineGlitch', params.get('lineGlitch'));
      display.set('BlockGlitch', params.get('blockGlitch'));
      display.set('GlitchKey', glitchKey);
      display.set('Time', time);

      display.set('Grid', params.get('grid'));
      display.set('Mix', params.get('mix'));
      quad.draw();
    },
  };
}

//---------------------------------------------------------------------------

const pct = (v) => `${Math.round(v * 100)}%`;

mountDemo({
  name: 'NESolume',
  pluginId: 'NE01',
  tagline:
    'Retro console video hardware, modelled as constraints rather than drawn as a look: a raster with not many lines, a palette with not many colours, attribute cells that force neighbours to share them — and the ways all three fail.',
  repo: 'https://github.com/stoatworks-labs/nesolume',
  page: 'https://stoatworks-labs.com/software/nesolume/',

  needFloat: true,
  showBackdrop: true,

  params: [
    {
      id: 'console', name: 'Console', type: 'option', default: 2, group: 'Picture',
      elements: CONSOLES.map((c) => c.name),
      hint: 'The machine: sets the raster, the colour system and the attribute cell size.',
    },
    {
      id: 'pixelSize', name: 'Pixel Size', type: 'standard', default: 0.5, group: 'Picture',
      display: (v) => `${sizeFactorFromParam(v).toFixed(2)}× native`,
      hint: "Raster scale. 0.5 is the console's native line count; up is chunkier.",
    },
    {
      id: 'colourDepth', name: 'Colour Depth', type: 'standard', default: 0.4, group: 'Picture',
      display: (v) => `${bitsFromParam(v)} bits/ch`,
      hint: "The Custom console's DAC. The real consoles ignore it — their depth is in the table.",
    },
    {
      id: 'dither', name: 'Dither', type: 'standard', default: 0.35, group: 'Picture',
      display: pct,
      hint: 'Ordered 4×4 Bayer, scaled to the quantisation step.',
    },
    {
      id: 'clash', name: 'Attribute Clash', type: 'standard', default: 0.5, group: 'Picture',
      display: pct,
      hint: 'How strictly a cell shares its colours: your own brightness, the cell’s hue.',
    },
    {
      id: 'grid', name: 'Pixel Grid', type: 'standard', default: 0, group: 'Picture',
      display: pct,
      hint: "The dark boundary between fat pixels — an LCD's cell gaps. Fades out when pixels get too small to have one.",
    },

    {
      id: 'wave', name: 'Wave', type: 'standard', default: 0, group: 'Distortion',
      display: pct,
      hint: 'A sine on the horizontal scroll, per line, snapped to whole pixels.',
    },
    {
      id: 'shake', name: 'Shake', type: 'standard', default: 0, group: 'Distortion',
      display: pct,
      hint: 'The whole frame knocked off its sync, re-rolled on the glitch clock.',
    },

    {
      id: 'blockGlitch', name: 'Block Glitch', type: 'standard', default: 0, group: 'Glitch',
      display: pct,
      hint: 'Tile pointers reading the wrong address: whole cells displaced by whole cells.',
    },
    {
      id: 'lineGlitch', name: 'Line Glitch', type: 'standard', default: 0, group: 'Glitch',
      display: pct,
      hint: 'Bands whose scroll register read back garbage: horizontal tears.',
    },
    {
      id: 'paletteGlitch', name: 'Palette Glitch', type: 'standard', default: 0, group: 'Glitch',
      display: pct,
      hint: 'CRAM corruption: channels rotated or inverted before quantisation — wrong but legal.',
    },
    {
      id: 'garbage', name: 'Garbage', type: 'standard', default: 0, group: 'Glitch',
      display: pct,
      hint: 'Tile pointers into memory that never held graphics: noise tiles, quantised like everything else.',
    },
    {
      id: 'glitchRate', name: 'Glitch Rate', type: 'standard', default: 0.3, group: 'Glitch',
      display: (v) => `${(v * 18).toFixed(1)} Hz`,
      hint: "How often the machine's luck changes. At 0 the corruption is a still — a crashed machine, not a screensaver.",
    },

    {
      id: 'mix', name: 'Mix', type: 'standard', default: 1, group: 'Output',
      display: pct,
      hint: 'Wet/dry against the untouched input.',
    },
  ],

  sources: ['scene', 'bars', 'ramp', 'detail', 'alpha', 'spot'],

  presets: {
    'Handheld': { console: 1, pixelSize: 0.5, dither: 0.55, clash: 0.2, grid: 0.55, wave: 0, shake: 0, blockGlitch: 0, lineGlitch: 0, paletteGlitch: 0, garbage: 0, glitchRate: 0.3 },
    'Front Room NES': { console: 2, pixelSize: 0.5, dither: 0.3, clash: 0.7, grid: 0, wave: 0, shake: 0, blockGlitch: 0, lineGlitch: 0, paletteGlitch: 0, garbage: 0, glitchRate: 0.3 },
    'Attribute Clash': { console: 3, pixelSize: 0.5, dither: 0.5, clash: 1, grid: 0, wave: 0, shake: 0, blockGlitch: 0, lineGlitch: 0, paletteGlitch: 0, garbage: 0, glitchRate: 0.3 },
    'Bedroom Micro': { console: 4, pixelSize: 0.5, dither: 0.55, clash: 0.6, grid: 0.15, wave: 0, shake: 0, blockGlitch: 0, lineGlitch: 0, paletteGlitch: 0, garbage: 0, glitchRate: 0.3 },
    '16-bit Console': { console: 6, pixelSize: 0.5, dither: 0.45, clash: 0.25, grid: 0, wave: 0, shake: 0, blockGlitch: 0, lineGlitch: 0, paletteGlitch: 0, garbage: 0, glitchRate: 0.3 },
    'Dirty Cartridge': { console: 2, pixelSize: 0.5, dither: 0.35, clash: 0.7, grid: 0, wave: 0, shake: 0.1, blockGlitch: 0.45, lineGlitch: 0.2, paletteGlitch: 0.35, garbage: 0.3, glitchRate: 0.35 },
    'Corrupted VRAM': { console: 6, pixelSize: 0.5, dither: 0.45, clash: 0.3, grid: 0, wave: 0.1, shake: 0.2, blockGlitch: 0.7, lineGlitch: 0.5, paletteGlitch: 0.6, garbage: 0.5, glitchRate: 0.55 },
    'Kill Screen': { console: 2, pixelSize: 0.55, dither: 0.3, clash: 0.8, grid: 0, wave: 0.35, shake: 0.5, blockGlitch: 0.6, lineGlitch: 0.8, paletteGlitch: 0.5, garbage: 0.6, glitchRate: 0.75 },
  },

  differences: [
    'The palettes and rasters here are a second copy of the plugin’s console table, maintained by hand. The plugin’s own copy is the one its harness checks pixel-by-pixel for palette legality; nothing checks this one.',
    'One of the plugin’s claims is checkable right here: turn every glitch control to full on a fixed-palette console and count the colours — corruption recolours cells, but never produces a colour the machine could not.',
    'The raster’s width follows this page’s canvas aspect, exactly as it follows the composition’s in the host.',
  ],

  createRenderer,
});
