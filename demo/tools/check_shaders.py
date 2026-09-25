"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted;
`python3 demo/tools/splice_shaders.py` copies the C++ across again.

------------------------------------------------------------------- why

`demo/plugin.js` holds the five GLSL stages and so does `source/shaders/`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. v1.1.0 is the case in point: the Amiga changed Quantize (the
HAM hand-over) and Display (interlace and the flicker fixer), and a page that
kept the old two would still have drawn something for every console.

Nothing else can. `netest` drives the real plugin class through a real FFGL
sequence and has no idea this page exists, and verify.sh's glslc step compiles
the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment updated on one side and not the other is exactly
the drift worth catching, because the comments carry the reasoning. The C++
holds no backtick, `${` or backslash (splice_shaders.py refuses to copy one),
so any backslash on the JS side is refused rather than decoded.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half: the console table, the conversions and
the Amiga in amiga.js are hand translations. amiga.js has its own check
(check_port.sh, against Amiga.cpp compiled unchanged); the console table and
the wiring in plugin.js only a reader checks.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/shaders/Vertex.cpp", "kVertex"),
    ("DOWNRES", "source/shaders/Downres.cpp", "kDownresFragment"),
    ("TILE", "source/shaders/Tile.cpp", "kTileFragment"),
    ("QUANTIZE", "source/shaders/Quantize.cpp", "kQuantizeFragment"),
    ("DISPLAY", "source/shaders/Display.cpp", "kDisplayFragment"),
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\|\$\{", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash or ${{ at line {upto.count(chr(10)) + 1}"
    return body, None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<10} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")
        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- run demo/tools/splice_shaders.py, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
