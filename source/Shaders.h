#pragma once

/**
    The GLSL for each stage of the chain.

    NESolume is not a pixelate filter with a tint on it. It is a model of what
    a retro console's video hardware could and could not do: a raster with not
    many lines, a palette with not many colours, and -- the part that makes it
    read as a machine rather than a mosaic -- attribute cells that force
    nearby pixels to share those colours. The recognisable artefacts fall out
    of the constraints. Attribute clash is not drawn; it is what happens when
    a cell is only allowed one set of colours and two objects cross it.

    The stages, in order:

      Downres   full-resolution RGB -> the console's raster, box-filtered so
                the small picture is an average, not a lucky point sample.
      Tile      one texel per attribute cell: the cell's mean colour, which
                is the "what is this cell mostly showing" the clash model
                needs.
      Quantize  the palette. Ordered dither, then either a nearest-entry
                search of a fixed master palette or per-channel bit
                truncation, with the pixel's chroma pulled toward its cell's
                before the choice is made. Palette corruption and garbage
                tiles happen here too, because they have to happen *before*
                colour choice to stay palette-legal.
      Display   raster -> composition. Nearest-neighbour scale-up, scroll-
                register damage (waves, shakes, torn lines, displaced
                blocks), the LCD grid, and the wet/dry mix.

    Downres, Tile and Quantize run at (or below) the console raster no matter
    what the composition size is, so the artefacts are resolution-independent
    and the expensive work is cheap.
*/
namespace nesolume::shaders
{
/// Shared by every pass: draws the screen quad and scales UVs by MaxUV so the
/// same program works against a host texture with padding and against our own
/// framebuffers, which have none.
extern const char* const kVertex;

extern const char* const kDownresFragment;
extern const char* const kTileFragment;
extern const char* const kQuantizeFragment;
extern const char* const kDisplayFragment;

} // namespace nesolume::shaders
