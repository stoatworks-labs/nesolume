# nesolume

Retro console video hardware (raster, palette, attribute cells and their
glitches) as an FFGL effect for Resolume Arena/Avenue. C++/GLSL, CMake MODULE
→ universal `.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the quantise or display shaders.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/netest --out /tmp/frame.png`
- List parameters: `./build/netest --list`
- Palette legality: `python3 tools/verify.py` (every pixel must be a legal
  colour of its console, even with corruption at full)
- Dead controls: `python3 tools/sweep.py` (every parameter must move pixels)
- Everything at once: `tools/verify.sh` (arm64 dev build; `--universal` for a
  fresh universal Release build plus lipo, plist and oxbow probe)

## The Amiga (v1.1.0)
- Console element 9, appended; five controls AFTER the About block (Amiga
  Mode, Screen Mode, Interlace, Flicker Fixer, Amiga Palette). Append only.
- `source/Amiga.{h,cpp}`: CPU, no GL. Built with `-ffp-contract=off` so both
  macOS slices and Windows agree to the bit.
- Checks: `netest --ham-edge|--ham-optimal|--ehb|--palette|--lace --size WxH`,
  `--negative` (each must fail on a perturbed model), `--names`, `--params`,
  `--ham-cost`. `NETEST_RENDERER=software` is CI's renderer.
- Compatibility: `python3 tools/compat.py` renders every existing configuration
  through v1.0.7's own harness (built from the tag into build/compat-*) and
  this one: byte-identical. `--negative` runs it against `netest_insert` (the
  Amiga inserted mid-list) and must fail. On the software renderer pass
  `--retries 3`: it is not repeatable at the last bit.
- The OpenFX build leaves the Amiga out.

## OpenFX build
- `source/ofx/NESolumeOFX.cpp` → `build/NESolume.ofx.bundle` (target
  `NESolumeOFX`, `-DBUILD_OFX=OFF` to skip) for Resolve/Vegas/Nuke/Natron.
  Consoles.cpp links straight from source; the four GPU stages are mirrored
  on the CPU. Change a stage's GLSL, change the matching function there.
- OFX time is the timeline frame; seconds = t / frame rate, so the glitch
  clock is deterministic against the edit.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.nesolume --size 640x360 --out /tmp/ne.bmp`
- Identity proof: add `--set mix=0` → 0 bytes differ.
- Preset proof: `--edit preset=N` must render byte-identically to `--edit`ing
  the values from Presets.h by hand.
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Notes
- Four shader stages; the effect is the GLSL, the C++ is host glue and the
  console table.
- A new console is a row in `source/Consoles.cpp` and nothing anywhere else
  (the Amiga is the exception: its machine is `source/Amiga.*`).
  The palette store offsets are hand-counted — the static_assert is the guard.
- Corruption happens **before** quantisation so a glitch can never leave the
  palette; displacement moves whole texels and wraps. Those two invariants
  are the plugin.
- The raster is height-only; width follows the composition aspect so pixels
  stay square. Deliberate — see AGENTS.md.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never
  the build log.
- `flat` and `active` are GLSL reserved words. Shader errors only surface at
  runtime, in the diagnostics log (`~/Library/Logs/nesolume/`).
- Public repo. "Commit" = commit **and** push.
