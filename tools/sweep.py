"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage has something to do, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Things that will fool you, learned across the fleet:

  * **The baseline console must be Custom.** Colour Depth only drives the
    Custom console's DAC; sweep it against the NES and it is correctly dead.
  * **Render big enough.** The pixel grid fades itself out when a raster pixel
    is under ~2 output pixels; 1280x960 against a 240-line raster gives 4.
  * **The About block is not a control.** The text line and the link buttons
    are parameters only because FFGL has no other surface; they are skipped
    here, not swept.
"""
import subprocess, zlib, struct, sys, os, tempfile

SC = tempfile.mkdtemp(prefix="nesweep")

# A baseline where every stage has something to do: Custom console (so Colour
# Depth is live), moderate dither and clash, every fault switched partly on.
BASE = {
    "Console": 0, "Colour Depth": 0.3, "Dither": 0.5, "Attribute Clash": 0.5,
    "Pixel Grid": 0.4, "Wave": 0.3, "Shake": 0.3, "Block Glitch": 0.4,
    "Line Glitch": 0.4, "Palette Glitch": 0.4, "Garbage": 0.3,
    "Glitch Rate": 0.3, "Mix": 1.0,
}

# Not controls: the About block exists only because FFGL has no window.
SKIP = {"About", "User guide", "Project page", "Source on GitHub", "Support the work"}

def render(path, overrides):
    args = ["./build/netest", "--out", path, "--width", "1280", "--height", "960", "--frames", "5"]
    merged = dict(BASE); merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr); sys.exit(1)
    return open(path, "rb").read()

def pixels(png):
    i = 8; idat = b""; w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i+4])[0]; t = png[i+4:i+8]; d = png[i+8:i+8+ln]
        if t == b"IHDR": w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT": idat += d
        i += 12 + ln
    raw = zlib.decompress(idat); stride = w*4
    return b"".join(raw[y*(stride+1)+1:(y+1)*(stride+1)] for y in range(h))

def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa); changed = 0; total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i]-pb[i]), abs(pa[i+1]-pb[i+1]), abs(pa[i+2]-pb[i+2]))
        if d > 2: changed += 1
        total += d
    return changed / (n/4) * 100, total / (n/4)

names = subprocess.run(["./build/netest", "--list"], capture_output=True, text=True).stdout
params = [' '.join(l.split()[1:-1]) for l in names.strip().splitlines()]
params = [p for p in params if p not in SKIP]

# Options are discrete; sweep them across their real element range.
DISCRETE = {"Console": (0, 8), "Preset": (0, 8)}

print(f"{'parameter':<20} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    a = render(f"{SC}/a.png", {p: lo})
    b = render(f"{SC}/b.png", {p: hi})
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok: dead.append(p)
    print(f"{p:<20} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
