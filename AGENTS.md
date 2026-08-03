# AGENTS.md — bringing an LLM up to speed on NESolume

Orientation for an AI assistant (or a new human) picking this project up cold.
`CLAUDE.md` holds the short command reference; this file explains the model and
the traps.

---

## 1. What this is

**Retro console video hardware** for Resolume Arena / Avenue, built on the
official Resolume **FFGL** SDK. C++/GLSL, CMake, public MIT.

The one idea to internalise before changing anything:

> **This models constraints, not looks.** A console's picture was a raster with
> not many lines, a palette with not many colours, and attribute cells that
> forced nearby pixels to share those colours. The recognisable artefacts —
> chunky dither, attribute clash, tiles landing in the wrong places — are what
> those constraints (and their corruption) do to a picture, not drawings of the
> artefacts.

Two rules follow, and both are load-bearing:

1. **Nothing may leave the palette.** Dither, clash, palette corruption and
   garbage tiles are all allowed to choose the *wrong* colour, never an
   *illegal* one. That is why the corruption in the quantise stage happens
   **before** colour choice: a real glitch scrambled indices into CRAM, not the
   DAC. `tools/verify.py` enforces this pixel-by-pixel; run it after any change
   to the quantise or display shaders.
2. **Displacement moves texels, never colours.** The display stage's wave,
   shake, torn lines and displaced blocks change *which* raster pixel is shown
   where — in whole pixels, wrapping like a scroll register — and nothing else.
   The one exception is the pixel grid, which is honest cosmetics (an LCD's
   cell gaps) and is why `verify.py` runs with Grid at 0.

## 2. The shape of it

Four shader stages. `source/Shaders.h` documents them; each lives in its own
file under `source/shaders/`.

```
Downres    full-res RGB -> the console's raster, box-filtered
Tile       one texel per attribute cell: its mean colour
Quantize   clash, corruption, dither, then the palette
Display    nearest-neighbour up, scroll damage, LCD grid, mix
```

`source/Consoles.cpp` holds the machines: raster height, attribute cell size,
and the colour system — a fixed master palette (Game Boy, NES, Spectrum, C64)
or an RGB DAC depth (Master System 2 bits, Mega Drive 3, SNES/PlayStation 5,
Custom from the slider). **A new console is a table row there and nothing
anywhere else.** The palette store offsets are hand-counted; the
`static_assert` on the store size is what stands between you and a palette
that silently reads its neighbour's colours.

### Decisions that look arbitrary and are not

- **The raster is height-only; width follows the composition's aspect.** The
  machines drew a fixed number of lines; how wide the picture was depended on
  the display. Deriving width from the composition keeps pixels square on
  screen for any aspect, which is what an operator wants from a VJ effect. If
  you want letterboxed native 4:3, that is the layer's job, not the plugin's.
- **The clash model is: your own luminance, your cell's chroma.** A cell's
  sub-palette was usually a ramp of one hue, so within a cell you could have
  your own brightness but not your own colour. The Tile stage's mean colour is
  the automatic stand-in for "what this cell mostly shows". At Clash 1 two
  objects crossing a cell share a hue exactly the way they did on a Spectrum.
- **The fixed-palette dither amplitude is 0.22 of the range.** A master
  palette has no uniform quantisation step, so the Bayer offset is scaled by a
  constant that in practice spans neighbouring ramp entries on all four fixed
  palettes. Change it and re-run `verify.py` *and look at the Game Boy card* —
  too low and dither dies on 4-entry palettes, too high and flat fields boil.
- **The glitch clock ticks at Rate × 18 Hz and a tick is a re-roll.** Events
  hold for one tick, so Rate is "how often the machine's luck changes". At
  Rate 0 the key is pinned to 0 and a glitched frame is a *still* —
  deterministic, and exactly what a crashed machine does. Sweeps rely on this:
  a glitch control is visible at frame 0 without waiting for an event.
- **Alpha is snapped to one bit** in the quantise stage. Console video had
  transparency or it did not. Soft-edge compositing comes back via Mix.

## 3. Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Add `-DCMAKE_OSX_ARCHITECTURES=arm64` for a much faster dev build.

## 4. Traps

### The macOS one that will get you

**`CMAKE_OSX_ARCHITECTURES` must be set before the first target is created.**
Set it later and CMake silently ignores it — you get an arm64-only binary that
the build log calls a success, and an Intel Resolume that quietly fails to
load the plugin.

**Always verify the artefact, never the log:**

```bash
lipo -archs build/NESolume.bundle/Contents/MacOS/NESolume
```

### GLSL reserved words

`flat` and `active` are reserved. So are `filter`, `input`, `output`,
`sample`, `common`, `partition`, `resource` and a long tail of others. Both of
the first two have already bitten sibling repos, and the failure mode is
nasty: the shader fails to compile at *runtime*, `InitGL` returns `FF_FAIL`,
and Resolume shows an effect that silently does nothing. That is what
`source/Diag.cpp` exists for — it names the stage.

### The SDK leaks its colour texture

`ffglex::FFGLFBO::Release()` deletes the framebuffer and the depth
renderbuffer, then tests `depthBufferID` a second time where it plainly meant
`colorTextureID` (SDK `b1afaf9`, `FFGLFBO.cpp`). The colour texture is never
freed.

`source/PassBuffer.{h,cpp}` subclasses around it. **Use `PassBuffer`, never
`FFGLFBO` directly** — this plugin rebuilds its buffers whenever the console,
the pixel size or the composition size changes, so the leak would be
per-adjustment, not one-off.

### `FFGLScopedFBOBinding.h` is not in the umbrella header

`FFGLSDK.h` includes every other scoped binding and omits that one. Include it
by hand.

### The plugin registers itself from a static constructor

`CFFGLPluginInfo` is a file-scope object in `NESolume.cpp` that nothing
references by name. That is why `nesolume_core` is an **OBJECT** library and
not a **STATIC** one: in an archive the linker is entitled to drop the whole
translation unit, and you get a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. Do not "tidy" it to STATIC.

### A dead control is invisible to the compiler

`tools/sweep.py` renders every parameter at both ends of its range and fails
if any of them made no difference to the picture.

```bash
python3 tools/sweep.py
```

**Run it after adding a parameter, renaming a uniform, or moving anything
between the C++ and the GLSL.** A uniform name that does not match is silently
ignored — `glGetUniformLocation` returns -1 and `glUniform` on -1 is a
documented no-op — so a control can be completely dead while everything
compiles, links, loads and renders. Nothing else here catches that.

Two sweep-specific traps, inherited from the fleet: the **baseline console
must be Custom** (Colour Depth only drives the Custom DAC and is correctly
dead against the NES), and the **About block is skipped, not swept** (the
text line and link buttons are parameters only because FFGL has no window).

## 5. Testing

There is no unit test rig and there cannot usefully be one — the output is a
picture. `tools/netest` is the substitute: a headless GL 4.1 core context
driving the **real** `NESolume` class through the **real** FFGL entry
sequence, writing a PNG. It is deterministic (time comes from the frame
counter, not the clock), so two runs produce identical pixels and a change
that was not supposed to alter the picture can be checked byte for byte.

What the default test card is for, band by band:

- **Hue × brightness field** — what every palette does with colours it does
  not have. Sparse palettes posterise it into their real entries; dither
  breaks the banding up; attribute cells show as chunky same-hue rectangles.
- **Overlapping discs on grey** — their edges cross attribute cells at every
  phase, which is where clash either recolours them or does not.
- **Grey ramp** — the quantisation levels, and what dither does between them.
- **Single-pixel checkerboard** — does the downsample average (a mid grey at
  the raster) or point-sample (a flickering mess)?

The three standing checks, all of which must pass before believing a change:

```bash
python3 tools/verify.py   # every pixel legal for its console, corruption on
python3 tools/sweep.py    # every control moves pixels
# preset N must render byte-identically to its hand-set values:
./build/netest --out /tmp/a.png --set "Preset=8"
./build/netest --out /tmp/b.png --set "Console=2" ... # values from Presets.h
cmp /tmp/a.png /tmp/b.png
```

`--pipe` reads raw RGBA frames from stdin and writes them to stdout, so real
footage goes through the chain with `ffmpeg | netest | ffmpeg`, and `--script`
automates the parameters over the sequence — same conventions as the sibling
harnesses: **stdout is the video**, so anything conversational goes to stderr,
and the vertical flips on the way in and out do **not** cancel.

## 5b. The OpenFX build

`source/ofx/NESolumeOFX.cpp` mirrors the four GPU stages on the CPU for
Resolve/Vegas/Nuke/Natron. The console table links from source and has one
home; the per-pixel machinery is duplicated with edit-both comments. The
low-res chain (downres, tile means, quantise) is precomputed once per render
and only the display stage runs in the threaded per-tile loop — do not move
the chain into `multiThreadProcessImages`, it would recompute per tile.

The hashes are the GLSL formulas in double precision: the two builds agree
constant for constant, not bit for bit, like every sibling port. Verify with
ofxprobe (see CLAUDE.md): render, `--set mix=0` identity (0 bytes), and
`--edit preset=N` against hand-set values (byte-identical). The factory is
deliberately heap-leaked in `getPluginIDs` — the fleet's exit-teardown trap.

## 6. What has never been checked

- **It has never been loaded into Resolume.** The bundle is installed to
  `~/Documents/Resolume Arena/Extra Effects/`, but nobody has launched Arena
  with it yet: parameter groups, the Console and Preset dropdowns, Arena's
  real texture sizes and premultiplication behaviour are all unconfirmed.
  Those are exactly what the offline harness cannot tell you about, because
  it supplies its own textures.
- **Whether Resolume consumes `FF_EVENT_FLAG_VALUE`** — a preset changes the
  picture regardless, but stale sliders in the inspector are possible.
- **The Windows build has never been compiled or run.** No CI is set up yet.
- **The universal macOS build has been built and `lipo`-verified, never run
  on Intel.**
- Performance figures (0.17 ms/frame at 1080p, 0.56 ms at 4K) come from one
  M4 Max, never from CI — hosted macOS runners have no GPU.

## The browser demo

`demo/` is a static page at **nesolume-demo.stoatworks-labs.com**: this
plugin's own GLSL, ported to WebGL2, running on clips generated in the page.
Deployed as a Cloudflare Worker serving `demo/` as static assets
(`wrangler.toml`), with **no build step** — what is committed is what is
served.

Three things about it are not visible from the files:

- **`demo/plugin.js` carries a second copy of the shaders AND the console
  table.** Change a shader or a palette and change both, or the page quietly
  renders the old machine.
- **`demo/vendor/` is vendored, not authored here.** The master is
  `stoatworks-backend/resolume-demo/`; fix it there and re-run its `sync.sh`.
  `sync.sh --check` reports drift.
- **Verify a deploy by content, never by status code.** A wrong page still
  answers 200.

```bash
cf-run npx wrangler deploy
curl -s 'https://nesolume-demo.stoatworks-labs.com/' | grep -o '<title>[^<]*'
```

## 7. Conventions

- Public repo. "Commit" means commit **and** push.
- Standard AI disclaimer in the README — see the fleet's disclaimer scope.

## Diagnostics

`source/Diag.{h,cpp}` is a small member of the fleet's `diag` family: a log
file only (`~/Library/Logs/nesolume/` on macOS). No crash handler (a plugin
has no business installing a process-wide signal handler inside Resolume) and
no bundle command (there is no UI to hang one off).

What it covers is the failure that actually happens — `InitGL` returning
`FF_FAIL` because a shader would not compile, which from the operator's side
looks like "the effect does nothing" with no message anywhere. With four
stages, knowing *which* one is most of the diagnosis; the GL
vendor/renderer/version strings sit next to it because that is usually the
rest of it.
