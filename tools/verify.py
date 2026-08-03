"""Every rendered pixel must be a colour the selected console could produce.

This is the property the whole plugin hangs on: dither, attribute clash,
palette corruption and garbage tiles are all allowed to choose the *wrong*
colour, but never an *illegal* one — nothing a broken cartridge showed was
ever outside the palette. So render the test card through each fixed-palette
console and check every pixel against that console's master palette, and
render the Custom console at 1 bit per channel and check the whole frame
lands on the 8 corners of the RGB cube.

The palettes here are the same tables as source/Consoles.cpp. That means this
does not verify the transcription of the palettes from their sources — it
verifies that the GPU pipeline (dither offsets, clash arithmetic, corruption
swizzles, the grid being off, the mix being clean) cannot push a pixel off
the palette, which is the failure a shader edit would actually introduce.

    python3 tools/verify.py

Exit code 1 means an illegal colour reached the screen.
"""
import subprocess, zlib, struct, sys, tempfile, os

SC = tempfile.mkdtemp(prefix="neverify")

GAME_BOY = [(15,56,15),(48,98,48),(139,172,15),(155,188,15)]

NES = [
    (84,84,84),(0,30,116),(8,16,144),(48,0,136),(68,0,100),(92,0,48),(84,4,0),(60,24,0),
    (32,42,0),(8,58,0),(0,64,0),(0,60,0),(0,50,60),(0,0,0),(0,0,0),(0,0,0),
    (152,150,152),(8,76,196),(48,50,236),(92,30,228),(136,20,176),(160,20,100),(152,34,32),(120,60,0),
    (84,90,0),(40,114,0),(8,124,0),(0,118,40),(0,102,120),(0,0,0),(0,0,0),(0,0,0),
    (236,238,236),(76,154,236),(120,124,236),(176,98,236),(228,84,236),(236,88,180),(236,106,100),(212,136,32),
    (160,170,0),(116,196,0),(76,208,32),(56,204,108),(56,180,204),(60,60,60),(0,0,0),(0,0,0),
    (236,238,236),(168,204,236),(188,188,236),(212,178,236),(236,174,236),(236,174,212),(236,180,176),(228,196,144),
    (204,210,120),(180,222,120),(168,226,144),(152,226,180),(160,214,228),(160,162,160),(0,0,0),(0,0,0),
]

SPECTRUM = [
    (0,0,0),(0,0,215),(215,0,0),(215,0,215),(0,215,0),(0,215,215),(215,215,0),(215,215,215),
    (0,0,255),(255,0,0),(255,0,255),(0,255,0),(0,255,255),(255,255,0),(255,255,255),
]

C64 = [
    (0,0,0),(255,255,255),(136,57,50),(103,182,189),(139,63,150),(85,160,73),(64,49,141),(191,206,114),
    (139,84,41),(87,66,0),(184,105,98),(80,80,80),(120,120,120),(148,224,122),(120,105,196),(159,159,159),
]

ONE_BIT = [(r,g,b) for r in (0,255) for g in (0,255) for b in (0,255)]

def render(path, sets):
    args = ["./build/netest", "--out", path, "--width", "1280", "--height", "720", "--frames", "5"]
    for k, v in sets.items():
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

def check(name, sets, palette, tolerance=1):
    png = render(f"{SC}/{name}.png", sets)
    data = pixels(png)
    seen = set()
    for i in range(0, len(data), 4):
        seen.add((data[i], data[i+1], data[i+2]))
    illegal = []
    for c in sorted(seen):
        if not any(max(abs(c[0]-p[0]), abs(c[1]-p[1]), abs(c[2]-p[2])) <= tolerance for p in palette):
            illegal.append(c)
    verdict = "ok" if not illegal else f"ILLEGAL: {illegal[:8]}{'...' if len(illegal) > 8 else ''}"
    print(f"{name:<28} {len(seen):3d} distinct colours, palette of {len(palette):2d}   {verdict}")
    return not illegal

# Corruption on, cosmetics that recolour (grid) off. The point is that even a
# machine failing hard cannot leave the palette.
FAULTS = {"Palette Glitch": 0.6, "Garbage": 0.5, "Block Glitch": 0.5, "Line Glitch": 0.5,
          "Dither": 0.6, "Attribute Clash": 0.5, "Pixel Grid": 0.0}

ok = True
ok &= check("game-boy", {**FAULTS, "Console": 1}, GAME_BOY)
ok &= check("nes", {**FAULTS, "Console": 2}, NES)
ok &= check("zx-spectrum", {**FAULTS, "Console": 3, "Attribute Clash": 1.0}, SPECTRUM)
ok &= check("c64", {**FAULTS, "Console": 4}, C64)
ok &= check("custom-1bit", {**FAULTS, "Console": 0, "Colour Depth": 0.0}, ONE_BIT)

print()
if not ok:
    print("ILLEGAL COLOURS REACHED THE SCREEN")
    sys.exit(1)
print("every pixel of every render is a legal colour of its console")
