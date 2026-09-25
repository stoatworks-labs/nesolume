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

## 5c. The Amiga (v1.1.0)

Allan picked this on 2026-09-25 as the last item of tranche five; the spec is
`~/Projects/resolume/specs/SPEC-nesolume-amiga.md`. It is a **minor release of
a released plugin**, so the rule above all others was: change nothing that
already shipped.

### The machine

`source/Amiga.{h,cpp}` is the machine, CPU only, no GL: the 12-bit colour
registers, the three bitplane modes, the four screens, the field clock, the
palette choice and the HAM6 encoder. The facts and their sources are in the
header (Hardware Reference Manual ch. 3 and Appendix A, the same ones
copperlist emulates). `Consoles.cpp` gains one row, `kPaletteAmiga`, at the
end.

The pipeline for the Amiga, in `NESolume::ProcessOpenGL`:

```
Downres    to 320/640 x 256/200 (x2 laced), not the composition's aspect
  read-back  the clean raster -> k-means -> the registers (or the Fixed set)
Tile       as before (16 = the bitplane fetch word; Clash is forced to 0)
Quantize   OCS/EHB/high res: the existing nearest-colour path over the registers
           HAM6: PaletteMode 2, a hand-over of the corrupted, dithered colour
  read-back  HAM6 only -> amiga::encodeHamFrame on the CPU -> upload
Display    as before, plus the laced field (Laced/Field/FlickerFixer)
```

### Decisions taken without asking

- **Appended, twice.** The Amiga is Console element 9, after the PlayStation;
  the five controls (`Amiga Mode`, `Screen Mode`, `Interlace`, `Flicker Fixer`,
  `Amiga Palette`) come **after the About block**, boreal's precedent, because
  that is the only place new ids move nothing. Resolume addresses parameters by
  name (memory `resolume-params-by-name`), so a middle insertion would restore
  there, but FFGL's ABI and other hosts are by index. Names checked unique as
  Resolume reduces them (`netest --names`: 25 parameters, 25 addresses).
  They get their own group, `Amiga`, and are inert on every other console.
- **No new presets.** A preset covering the Amiga controls would have reset
  them from every existing preset, and the OpenFX build shares the table.
  Presets.h is untouched.
- **The raster is fixed-width and stretched.** Every other machine's width
  follows the composition so pixels stay square; the Amiga's is 320 or 640
  because a HAM line's length *is* the constraint. Pixel Size still scales
  both dimensions.
- **High res is 16 colours in every mode**: OCS fetches four planes there, so
  EHB and HAM do not exist. `Amiga Mode` is honestly inert in high res.
- **Registers per frame by k-means over the 12-bit histogram**, seeded from the
  last frame's registers so a moving picture's palette drifts rather than
  jumps; a mode or count change re-seeds. Deterministic farthest-point start.
  Sorted darkest first, because register 0 is the background and the colour a
  HAM line starts from. EHB fits the 32 bases and their half twins together
  (c = (4Σx + 2Σy) / (4n + m)). **Fixed**: sixteen greys for HAM6, a 27-cube
  plus five for 32, corners plus eight for high res.
- **EHB halves by `v >> 1`**, as Denise does: 15 becomes 7.
- **HAM6's line starts from register 0** (the background), per the HRM.
- **The HAM error is integer, weighted 3:6:1** (close to the luma weights the
  shader's nearest search uses), so the optimum is an exact number and two
  searches compare with `==`.
- **HAM6 dither is one 12-bit step** of the Bayer offset, applied in the shader
  before the hand-over, so the corruption and the dither stay in their one home.
- **Interlace is a bob.** A field shows its own lines, each covering its line
  pair; the other field's lines are not drawn. A one-line detail is there in
  one field and gone in the next (25/30 Hz), edges twitter, and a uniform area
  does not move at all. Decided against a phosphor-persistence model, in which
  every pixel of every area alternates between full and decayed brightness at
  25 Hz: that is a whole-picture 25 Hz grating, which is both not what anyone
  perceived and a photosensitivity risk. The field is `floor(t x rate + 1e-6)`
  in **double**, from real elapsed time (copperlist's decision: 50 and 60
  exactly). `elapsedSeconds` now records the double it returns.
- **Output alpha stays one bit**, as for every console (Resolume's demo clips
  are DXV with alpha; the decision predates this and is unchanged).
- **The OpenFX build leaves the Amiga out.** Its Console list skips
  `kPaletteAmiga`; since the Amiga is last, nothing renumbers.
- **The laced mode's flicker was measured on the demo clips** (held still, so
  the clip's own frame cadence is not in it): at most 0.8% of white field to
  field over the whole frame and 3.8% in the worst 40-px block, against the
  flash guidelines' 10% (IntoTheGlow_02, Trinity, Cyberspace, Galactucity,
  OrganicMotions, NeonRoom2, Metalive, SpaceUniverse; OCS and HAM6). Synthetic
  one-line gratings (a laced Workbench) can do far more; that is the look.

### The HAM6 encoder

A Viterbi over the previous pixel's colour, 4,096 states, exact. It never
stores 4,096 costs: a modify of red cannot see the old red, a palette register
can follow anything, so the only things one pixel reads from the one before
are the best cost over each gun with the other two fixed (three 16x16 tables)
and the best of all. Each step computes the next three tables in closed form
from those (`Amiga.cpp`, with the algebra in comments). ~7,000 integer
operations a pixel; the backtrack re-derives each pixel's colour from the
stored tables.

**It costs** (`netest --ham-cost`, M4 Max, 16 cores, a shared machine):
HAM_COST_AGENTS The plugin uses `hardware_concurrency / 2` threads, 1..8.

### Traps this release found

- **"A hard edge takes exactly three pixels" is not what the optimum does.**
  Under squared error the exact encoder takes FIVE on a big edge — it steps two
  guns through a midpoint (5 -> 8 -> 11) because two half-errors cost less
  than one whole one. Three is the floor, not the answer. So `--ham-edge`
  measures three things: a one-level edge (no midpoint exists) arrives in
  exactly 3, a search of the codes says 3 is the fewest, and a big edge
  arrives in 5, never fewer than 3.
- **Initialising the whole backtrack table per line was a third of the
  encoder**: 321 x 772 ints = 1 MB of `fill` per line, 256 MB a frame. Only
  the first table needs it. 78 ms -> 24 ms single-threaded, with the loops
  rewritten as sixteen-wide add/min runs so they vectorise.
- **v1.0.7's harness ignores `NETEST_RENDERER`**, so a first software-renderer
  compat run compared the old build on the GPU with the new one in software
  and "found" thousands of differing bytes on every console. `tools/compat.py`
  now patches the same switch into the reference harness (the harness only;
  the plugin code is the tag's). Diagnosed by reverting both shader edits and
  still seeing the difference.
- **A lace check row on a line boundary is a coin toss.** At 320x180 an output
  row whose centre lands exactly on a raster-line boundary (row 94 of 180 on a
  400-line raster) may sample either line. The check picks rows whose centre
  is 0.2..0.8 into a line.
- **The worktree guard reads command text**: `git -C $W` is "the shared
  checkout", and a heredoc inside a blocked command never writes its file. Use
  literal paths in a zsh script; write message files with the Write tool.

### Would this hold on another rasteriser, at another raster?

Every check runs at 1280x1024 and 320x180 on the GPU and at 320x180 on Apple's
software renderer (`NETEST_RENDERER=software`, what CI gets), in verify.sh.

| Check | Holds because | Tolerance |
| --- | --- | --- |
| `--ham-edge` | The source is given at the Amiga raster (320x256) whatever the output size: the downres reads the input's size, so each raster pixel is one exact 12-bit source colour and the edge sits on a pixel boundary. Output read at raster-pixel centres. | none: colours compared as 12-bit values |
| `--ham-optimal` | CPU integers, no GL. | exact `==` |
| `--ehb`, `--palette` | Every output pixel is a register fetched from an RGBA8 texture and written through `mix(x, y, 1.0)`: float error is a few ulps, far below half an 8-bit level, so `v % 17 == 0` survives any conforming rasteriser. | 0 levels |
| `--lace` | The source is given at the laced raster; the detail row is chosen 0.2..0.8 into a raster line; comparisons are of whole rows and whole frames. | on > 200, off < 20 (of 255), uniform spread 0 |
| `--compat` | Old and new are rendered on the SAME renderer in the same run, so rasteriser differences cancel; only a change in the programs or their inputs shows. | 0 bytes |
| sweep | a control that moves < 0.5% of pixels is dead; 1280x960 so the grid is resolvable | 0.5% |

### A check that cannot fail is not a check

`netest --negative` perturbs the model and requires each check to FAIL:

| Perturbation (`NESolume::Negative`) | Check | Result |
| --- | --- | --- |
| HAM may change two guns per pixel | `--ham-edge` | caught (the one-level edge arrives in 2) |
| EHB twins rounded up, not shifted | `--ehb` | caught |
| registers one 8-bit level off the 12-bit grid | `--palette` | caught (2 lines) |
| the field never advances | `--lace` | caught (5 lines) |
| the Amiga inserted mid-list (`netest_insert`) | `tools/compat.py --negative` | caught (21 at 320x180, 41 at two sizes) |
| a greedy HAM encoder | `--ham-optimal` | loses to the exhaustive optimum on 50 of 242 lines |

**The recorded GLSL mutation** (2026-09-25): in `Display.cpp`,
`( line & 1 ) != parity` became `==` — one character. `--lace` failed seven
lines at 320x180 (the detail's phase, the 100 Hz pattern, all four
odd/even-line checks, NTSC); `--palette` still passed, as it should. Reverted.

## 6. What has never been checked

- ARENA_AGENTS_BULLET
- **Whether Resolume consumes `FF_EVENT_FLAG_VALUE`** — a preset changes the
  picture regardless, but stale sliders in the inspector are possible.
- **The Windows build is compiled by CI and has never been run on a real
  Windows host outside the Arena gate.** (This line used to say it had never
  been compiled; ci.yml and release.yml have both existed since v0.1.0.)
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

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
