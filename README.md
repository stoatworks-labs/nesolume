# NESolume

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The chain has been
> verified numerically through an offline render harness that drives the real
> plugin class in a headless GL context — every rendered pixel is checked
> against its console's master palette even with corruption at full, every
> control is proven to change the picture, and presets render byte-identically
> to their hand-set values. The Amiga added in v1.1.0 is checked the same way:
> its HAM6 encoder against an exhaustive search, every pixel against the
> 12-bit registers, the interlaced fields frame by frame, and every existing
> console byte for byte against v1.0.7 (see [Status](#status)). ARENA_STATUS_LINE
> Check it in your own rig before trusting it in a show.

Retro console video hardware for [Resolume](https://resolume.com) Arena and
Avenue, as an FFGL effect: the raster, the palette, the attribute cells — and
the ways they failed.

**Video:** [The Amiga, in VIDEO_SECONDS seconds](https://www.youtube.com/watch?v=VIDEO_ID)
 · [the nine consoles, from v1.0](https://www.youtube.com/watch?v=a3zUwJ6kPfM)

![NES: the 2C02 palette with dither and attribute clash](docs/nes.png)

<sub>The harness test card through the NES: posterised into the 2C02 palette,
Bayer dither working the gradients, and the 16×16 attribute areas bleeding
hue exactly the way the hardware forced them to. All rendered by `netest`,
the repo's offline harness.</sub>

| | |
| --- | --- |
| ![Game Boy with the LCD grid](docs/gameboy.png) | ![ZX Spectrum at full attribute clash](docs/clash.png) |
| Game Boy: four greens and the LCD cell grid | ZX Spectrum, Attribute Clash at full: one cell, one hue |

![Mega Drive with corrupted VRAM](docs/corrupted.png)

<sub>The same card through a Mega Drive whose VRAM has stopped being told the
truth: torn lines, displaced tiles, rotated palettes, garbage tiles — every
pixel still a colour the machine could produce.</sub>

NESolume is not a pixelate filter with a tint on it. It averages the picture
down onto a real machine's raster, forces it through that machine's colour
system with the attribute cells enforced, and scales it back up as fat
pixels. The recognisable artefacts are **consequences** of the constraints
rather than features that were drawn on:

| What you see | Why it happens |
| --- | --- |
| **Chunky dithered gradients** | The palette has a handful of levels, so an ordered dither trades resolution for colour — the same trade every 16-bit artist made by hand. |
| **Attribute clash** | An attribute cell could use one sub-palette, usually a ramp of one hue. Within a cell you keep your own brightness but not your own colour, so two objects crossing a cell bleed into each other. |
| **Glitches that stay in palette** | Corruption happens to the *indices*, before colour choice — a real glitch scrambled CRAM and tile pointers, not the DAC. Nothing a broken cartridge showed was ever outside the palette, and nothing here is either. |
| **Blocks and torn lines that land cleanly** | Displacement is scroll-register damage: whole raster pixels, whole tiles, wrapping around the edge the way a scroll register wraps. |

Two things follow from modelling it this way. The controls interact like the
hardware did — palette corruption on a Game Boy can only choose among four
greens, and attribute clash on a machine with a generous palette is mild
because the palette is generous. And everything is resolution-independent:
the expensive work runs at the console raster, so 4K costs barely more than
1080p.

## The consoles

| Console | Raster | Colour system | Attribute cell |
| --- | --- | --- | --- |
| **Custom** | 240 lines | 1–8 bits per channel, from the Colour Depth slider | 8×8 |
| **Game Boy** | 144 | the four DMG greens | 8×8 |
| **NES** | 240 | the 2C02's 64-entry master palette | 16×16 — the real attribute-area size, and why NES clash is so chunky |
| **ZX Spectrum** | 192 | 15 colours (8 basic, 7 bright) | 8×8 — the machine that made clash famous |
| **Commodore 64** | 200 | Pepto's 16 VIC-II colours | 8×8 |
| **Master System** | 192 | 2 bits per channel (64 colours) | 8×8 |
| **Mega Drive** | 224 | 3 bits per channel (512) | 8×8 |
| **SNES** | 224 | 5 bits per channel (32,768) | 8×8 |
| **PlayStation** | 240 | 5 bits per channel | 8×8 |
| **Amiga** (v1.1.0) | 320 or 640 wide × 256 or 200 lines, doubled laced | 12-bit colour registers, chosen for the picture: 32, Extra Half-Brite's 64, or HAM6 | none — every pixel indexes the registers on its own |

The raster is the console's line count; the width follows your composition's
aspect so pixels stay square on screen. Pixel Size scales the whole raster
from quarter-size pixels to four-times chunky, with native at the centre. The
Amiga is the exception, below.

## The Amiga

Added in v1.1.0, at the end of the Console list so every saved composition
keeps its machine. The Amiga's display modes are constraints of exactly the
kind this plugin models, and one of them is famous:

| Mode | The constraint | What you see |
| --- | --- | --- |
| **OCS 32 Colours** | Five bitplanes index 32 colour registers. | A picture posterised into 32 colours chosen for it. |
| **Extra Half-Brite** | A sixth bitplane shows register *n* with every gun shifted right one bit. | 64 colours: 32 chosen, and their exact dark twins. |
| **HAM6** | Hold-and-modify: each pixel is one of 16 registers, **or the previous pixel with one gun changed**. | Colour smears rightwards off every hard edge. A change of all three guns takes at least three pixels to arrive; the encoder often spreads it wider, through midpoints, because that costs less error. That smear is the HAM fringe. |

Every colour is a **12-bit** register value — four bits a gun, 4,096 colours —
so a gradient bands in sixteen steps whatever the mode. The registers are
chosen per picture (k-means in 12-bit space, each frame starting from the
last so a moving picture's palette moves rather than jumps), or fixed.

The raster is the Amiga's own: **320 pixels by 256 lines** on a PAL low-res
screen, 200 on NTSC, 640 in high res — stretched to the composition as a
monitor set to fill would, not square, because a HAM line being 320 pixels
long *is* the constraint. High res fetches four bitplanes at most, so it is 16
colours in every mode: the chipset could not do EHB or HAM there, and neither
does this.

**Interlace** doubles the lines and shows them as two fields, one of alternate
lines at a time, 50 or 60 fields a second from real elapsed time. A detail one
line high is in one field and not the next, so it flickers at 25 or 30 Hz and
horizontal edges twitter — the laced Workbench everyone remembers. **Flicker
Fixer** is the cure the period sold: both fields woven into one steady frame.

HAM is not a per-pixel choice, so it cannot run in a shader. The line is
encoded on the CPU, exactly: a Viterbi over the previous pixel's colour whose
state is three 16×16 tables rather than 4,096 colours. On an M4 Max it costs
HAM_COST_LINE

## Controls

Grouped in the inspector as **Picture**, **Distortion**, **Glitch**,
**Output**, **Preset** and, last, **Amiga**.

### Picture

| Control | What it is |
| --- | --- |
| **Console** | The machine. Sets the raster, the colour system and the attribute cell size. |
| **Pixel Size** | Raster scale. 0.5 is the console's native raster; up is chunkier. |
| **Colour Depth** | The Custom console's DAC, 1–8 bits per channel. The real consoles ignore it — their depth is in the table. |
| **Dither** | Ordered 4×4 Bayer, scaled to the quantisation step. |
| **Attribute Clash** | How strictly a cell shares its colours. 0 is per-pixel freedom; 1 is one hue per cell, Spectrum-style. |
| **Pixel Grid** | The dark boundary between fat pixels — an LCD's cell gaps. Fades itself out when pixels get too small on screen to have one. |

### Distortion

| Control | What it is |
| --- | --- |
| **Wave** | A sine on the horizontal scroll, per line, snapped to whole pixels — fine scroll had no fractional bits. |
| **Shake** | The whole frame knocked off its sync, re-rolled on the glitch clock. |

### Glitch

| Control | What it is |
| --- | --- |
| **Block Glitch** | Tile pointers reading the wrong address: whole attribute cells displaced by whole cells. |
| **Line Glitch** | Bands whose scroll register read back garbage: horizontal tears. |
| **Palette Glitch** | CRAM corruption: cells with their channels rotated or inverted *before* quantisation, so the result is wrong but legal. |
| **Garbage** | Tile pointers into memory that never held graphics: noise tiles, quantised like everything else. |
| **Glitch Rate** | How often the machine's luck changes. At 0 the corruption is a still — a crashed machine, not a screensaver. |

### Output

| Control | What it is |
| --- | --- |
| **Mix** | Wet/dry against the untouched input. |

**Preset** holds eight factory looks, from *Handheld* to *Kill Screen*.
Picking one copies its values into the sliders; touching a covered slider
hands control back to Custom.

### Amiga

These act only when Console is **Amiga**. They sit after the About block
because appending is the only change that moves no existing parameter.

| Control | What it is |
| --- | --- |
| **Amiga Mode** | OCS 32 Colours, Extra Half-Brite or HAM6. Low res only; high res is 16 colours whichever you pick. |
| **Screen Mode** | PAL Low Res (320×256, 50 Hz), NTSC Low Res (320×200, 60 Hz), PAL High Res (640×256), NTSC High Res (640×200). |
| **Interlace** | Twice the lines, shown a field at a time. |
| **Flicker Fixer** | Weave both fields into one steady frame. Only does anything with Interlace on. |
| **Amiga Palette** | Per Frame: the registers chosen for the picture. Fixed: one content-independent set per mode (sixteen greys for HAM6, where every colour has to be built by modification and the fringes show most). |

Dither applies (at one 12-bit step for HAM6), and every glitch still happens
before colour choice, so a corrupted Amiga shows wrong registers, never an
illegal colour. Attribute Clash does nothing: the Amiga had no attribute
cells.

## Try it in your browser

**<https://nesolume-demo.stoatworks-labs.com>**

Not the plugin — the GLSL from `source/shaders/`, copied across unedited and
run in WebGL2 over clips generated in the page, with the parameters this
plugin's constructor declares. No install, and nothing you load leaves your
machine.

All four stages run, at the console raster. Pick the ZX Spectrum, turn
Attribute Clash to full and drag your own image in to watch the cells argue
about it — or check the plugin's own claim right there: every glitch control
at full on a fixed-palette console still never produces a colour the machine
could not.

It is a port, so it is not evidence about the plugin: a browser is not
Resolume, and its console table is a maintained second copy of
`source/Consoles.cpp`. The numbers worth trusting are in [Status](#status)
and come from the offline harness in this repository.

<!-- downloads:start -->

## Download

**[v1.0.7](https://github.com/stoatworks-labs/nesolume/releases/tag/v1.0.7)** — prebuilt for macOS, Windows and Linux. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`nesolume-1.0.7-macos-universal.dmg`](https://github.com/stoatworks-labs/nesolume/releases/download/v1.0.7/nesolume-1.0.7-macos-universal.dmg) | 203 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`nesolume-macos-universal.zip`](https://github.com/stoatworks-labs/nesolume/releases/latest/download/nesolume-macos-universal.zip) | 166 KB |
| Universal (Apple Silicon + Intel) · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`nesolume-ofx-macos-universal.zip`](https://github.com/stoatworks-labs/nesolume/releases/latest/download/nesolume-ofx-macos-universal.zip) | 243 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`nesolume-1.0.7-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/nesolume/releases/download/v1.0.7/nesolume-1.0.7-windows-x86_64-setup.exe) | 216 KB |
| x64 · .zip archive | [`nesolume-windows-x86_64.zip`](https://github.com/stoatworks-labs/nesolume/releases/latest/download/nesolume-windows-x86_64.zip) | 108 KB |
| x64 · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`nesolume-ofx-windows-x86_64.zip`](https://github.com/stoatworks-labs/nesolume/releases/latest/download/nesolume-ofx-windows-x86_64.zip) | 71 KB |

</details>

<details>
<summary><b>Linux</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`nesolume-ofx-linux-x86_64.zip`](https://github.com/stoatworks-labs/nesolume/releases/latest/download/nesolume-ofx-linux-x86_64.zip) | 721 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/nesolume/releases](https://github.com/stoatworks-labs/nesolume/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## OpenFX — Resolve, Vegas, Nuke, Natron

The same machine model also builds as an OpenFX plugin, so it runs in DaVinci
Resolve (Edit and Color pages, and Fusion), Vegas Pro, Nuke and Natron. The
console table is the same code; the four GPU stages are mirrored on the CPU,
constant for constant, and the glitch clock runs off the timeline frame — so
any frame renders identically however the host reaches it.

Grab the `nesolume-ofx-*` zip for your platform from the release and copy
`NESolume.ofx.bundle` into the standard OpenFX folder, then restart the host:

```
macOS    /Library/OFX/Plugins/
Windows  C:\Program Files\Common Files\OFX\Plugins\
```

## Install

Drop the plugin into Resolume's plugin folder and restart it:

- **macOS** — `~/Documents/Resolume Arena/Extra Effects/` (or `Resolume Avenue`)
- **Windows** — `%USERPROFILE%\Documents\Resolume Arena\Extra Effects\`

It appears in the effects list as **SW NESolume**.

## Build

```bash
git clone --recurse-submodules https://github.com/stoatworks-labs/nesolume.git
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

macOS produces a universal (arm64 + x86_64) `NESolume.bundle`; Windows a
`.dll`. `cmake --install build` drops the bundle straight into the Resolume
folder above.

## Looking at it without Resolume

`netest` renders the real chain to a PNG through a headless GL context. It
drives the actual plugin class through the actual FFGL entry sequence, so it
is testing the shipped code rather than a copy of it.

```bash
./build/netest --out /tmp/frame.png --width 1920 --height 1080
```

```bash
./build/netest --list
```

```bash
./build/netest --out /tmp/crash.png --set "Console=6" --set "Block Glitch=0.7" --set "Palette Glitch=0.6"
```

Its default test card is built to make wrong answers visible rather than to
look nice: a hue-by-brightness field for the palette, overlapping discs
crossing attribute cells for the clash, a grey ramp for the levels, a
single-pixel checkerboard for the downsample.

`--pipe` puts real footage through the chain instead, reading raw RGBA frames
from stdin and writing them to stdout:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/netest --pipe --width 1920 --height 1080 --script params.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - out.mp4
```

`--script` is parameter automation — a text file of `frame  Parameter Name
value` lines, linearly interpolated between keys.

## Status

Verified through the offline harness on an M4 Max:

- **Adding the Amiga changed nothing else.** `tools/compat.py` renders 71
  configurations — every console, the faults, both ends of every old control,
  every preset, a 40-frame glitch run, the pipe mode with a cue sheet —
  through v1.0.7's own harness, built from its tag, and through this one, at
  1280×720 and 320×180: every byte identical, on the GPU and on Apple's
  software renderer. The 20 parameters v1.0.7 declared keep their index,
  name, type, default, group and option values, read against `oxbow probe`
  of the released v1.0.7 bundle. With the Amiga inserted mid-list instead of
  appended, the same check fails (41 differences).
- **HAM6 is optimal.** The per-line encoder's error equals an exhaustive
  search over every one of the 64 six-bit codes per pixel on 242 random lines
  of 1 to 6 pixels, its output is always displayable HAM6, and a greedy
  encoder loses to it on 50 of them.
- **A HAM edge arrives as the hardware allows.** Through the plugin, on
  PAL low res over the grey registers: an edge of one level in all three guns
  takes exactly 3 pixels (and a search of the codes says 3 is the fewest);
  a big edge, (2,9,5) to (15,3,11), takes 5 — never fewer than 3; an edge
  onto a register takes 1, at the edge. Letting HAM change two guns at once
  makes the check fail.
- **Every Amiga pixel is 12-bit and legal**, in all six mode and screen
  combinations with every fault on: OCS and high res stay on their registers,
  EHB on its 32 or their exact half-brite twins (and it really uses both),
  every HAM line sampled is displayable HAM6. Rounding the twins instead of
  shifting them, or nudging the registers off the 12-bit grid, fails it.
- **Interlace behaves.** In laced mode a changed odd line moves nothing in an
  even field and does move the odd one, and the other way round; a one-line
  detail is on, off, on at 50 fields/s and on, on, off, off sampled at 100 Hz;
  a uniform area does not move at all; the flicker fixer holds the detail
  steady; NTSC alternates at 60. A field clock that never advances fails it.
  On eight of Resolume's demo clips held still, laced mode changes at most
  0.8% of white field to field over the whole frame, 3.8% in the worst 40-px
  block.
- Every check holds at 1280×1024 and 320×180, on the GPU and on the software
  renderer, and one changed character in the interlace GLSL fails seven of
  them.
- **Every pixel is a legal colour of its console**, checked pixel-by-pixel
  against the master palettes with dither, clash and every corruption control
  switched on (`tools/verify.py`): all 4 Game Boy greens, 54 distinct NES
  colours out of the 2C02's 64 entries, all 15 Spectrum colours, all 16 C64
  colours, and the Custom console at 1 bit landing on exactly the 8 corners
  of the RGB cube.
- **All 20 controls demonstrably do something.** `tools/sweep.py` renders
  every parameter at both ends of its range and fails if any made no
  difference — the only way to catch a uniform name that does not match
  between the C++ and the GLSL, since that fails silently.
- **Presets are honest.** A preset renders byte-identically to setting its
  values by hand.
- **The chain is deterministic.** Two runs of the harness produce
  byte-identical frames; time comes from the host clock, so a re-render
  reproduces its glitches.
- **Cost is 0.17 ms/frame at 1080p and 0.56 ms at 4K.** The quantising runs
  at the console raster, so it barely scales with composition size. Both
  figures are from one machine, not from CI.

Not verified:

- ARENA_STATUS_BULLET
- **The Amiga is not in the OpenFX build.** Its HAM encoder and field clock
  are wired into the FFGL chain only; the OpenFX Console list stops at the
  PlayStation.
- **The OpenFX build renders and proves itself through ofxprobe** (identity
  at Mix 0 is exact, presets render byte-identically to hand-set values,
  every output pixel is palette-legal) — but it has never been loaded into a
  real Resolve.
- **The Windows build has never been run.** It compiles: the tag-triggered
  release workflow builds the FFGL DLL and the OpenFX bundle on
  `windows-latest`, packages an NSIS installer, and every release since v0.1.0
  has shipped all three. Nobody has loaded any of them into a Windows host.
- **The universal macOS build has been built and `lipo`-verified, never run
  on an Intel machine.**

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).

Built on the [Resolume FFGL SDK](https://github.com/resolume/ffgl).
