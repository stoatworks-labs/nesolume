#!/usr/bin/env python3
"""The Amiga must change nothing else: prove it against the previous release.

nesolume is released, and saved compositions, MIDI/OSC mappings and presets
hold its parameters (Resolume by name, FFGL's ABI and other hosts by index)
and its option VALUES. v1.1.0 appends a console and five controls. This check
says that appending really was all it did:

  1. The parameter table. Every parameter of the previous release keeps its
     index, name, type, default and group, and every option keeps its
     elements, names and values, in order; new elements may only follow them.
     The reference is `tools/compat/<tag>-params.txt`, which is `oxbow probe`
     of the RELEASED bundle, so it is the host's view of what shipped.
  2. The pixels. The previous release's own harness, built from its tag, and
     this one render the same list of configurations -- every console, the
     faults, both ends of every old control, every preset, the pipe mode --
     and every byte must be the same.

    python3 tools/compat.py [--size 320x180] [--reference path/to/old/netest]
                            [--netest build/netest] [--negative]

Without --reference the previous tag's netest is built once into
build/compat-<tag>/ from `git archive <tag>` (the FFGL SDK is the submodule
already checked out here). --negative runs the check against
build/netest_insert -- the Amiga inserted mid-list -- and exits 0 only if the
check FAILS there, as a negative control must.

Exit 1: something the previous release had has moved.
"""
import argparse, os, re, struct, subprocess, sys, tempfile, zlib
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = "v1.0.7"
MANIFEST = os.path.join(ROOT, "tools", "compat", f"{TAG}-params.txt")


def parse_params(text):
    """Both `oxbow probe` and `netest --params` print this shape."""
    params, current = {}, None
    for line in text.splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(.+?)\s+type=(\d+)\s+default=(\S+)(.*)$", line)
        if m:
            idx = int(m.group(1))
            g = re.search(r"group=(\S+)", m.group(5))
            current = params[idx] = {
                "name": m.group(2).strip(), "type": int(m.group(3)),
                "default": m.group(4), "group": g.group(1) if g else "", "elements": []}
            continue
        m = re.match(r"\s+-\s+(.+?)\s+=\s+(\S+)\s*$", line)
        if m and current is not None:
            current["elements"].append((m.group(1).strip(), m.group(2)))
    return params


def compare_tables(old, new):
    problems = []
    for idx, o in sorted(old.items()):
        n = new.get(idx)
        if n is None:
            problems.append(f"[{idx}] {o['name']}: gone")
            continue
        for key in ("name", "type", "group"):
            if o[key] != n[key]:
                problems.append(f"[{idx}] {o['name']}: {key} {o[key]!r} -> {n[key]!r}")
        # A text parameter's default is its text, which carries the version.
        if o["type"] != 100 and float(o["default"]) != float(n["default"]):
            problems.append(f"[{idx}] {o['name']}: default {o['default']} -> {n['default']}")
        if n["elements"][:len(o["elements"])] != o["elements"]:
            problems.append(f"[{idx}] {o['name']}: elements {o['elements']} -> {n['elements']}")
    return problems


def pixels(path):
    png = open(path, "rb").read()
    i, idat, w, h = 8, b"", 0, 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t, d = png[i + 4:i + 8], png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    return w, h, zlib.decompress(idat)


# The existing harness checks' configurations: verify.py's faults, sweep.py's
# baseline and both ends of every control it sweeps, every console, every
# preset, and the time-dependent paths.
FAULTS = {"Palette Glitch": 0.6, "Garbage": 0.5, "Block Glitch": 0.5, "Line Glitch": 0.5,
          "Dither": 0.6, "Attribute Clash": 0.5, "Pixel Grid": 0.0}
BASE = {"Console": 0, "Colour Depth": 0.3, "Dither": 0.5, "Attribute Clash": 0.5,
        "Pixel Grid": 0.4, "Wave": 0.3, "Shake": 0.3, "Block Glitch": 0.4,
        "Line Glitch": 0.4, "Palette Glitch": 0.4, "Garbage": 0.3,
        "Glitch Rate": 0.3, "Mix": 1.0}
OLD_CONTROLS = ["Pixel Size", "Colour Depth", "Dither", "Attribute Clash", "Pixel Grid", "Wave",
                "Shake", "Block Glitch", "Line Glitch", "Palette Glitch", "Garbage", "Glitch Rate", "Mix"]


def configurations():
    cfgs = [("defaults", {}, [])]
    for c in range(9):
        cfgs.append((f"console {c}", {"Console": c}, []))
        cfgs.append((f"console {c} faults", {**FAULTS, "Console": c}, []))
        cfgs.append((f"console {c} at quarter pixels, grid", {"Console": c, "Pixel Size": 0.0, "Pixel Grid": 1.0}, []))
    cfgs.append(("zx spectrum full clash", {**FAULTS, "Console": 3, "Attribute Clash": 1.0}, []))
    cfgs.append(("custom 1 bit", {**FAULTS, "Console": 0, "Colour Depth": 0.0}, []))
    cfgs.append(("sweep baseline", dict(BASE), []))
    for p in OLD_CONTROLS:
        for v in (0.0, 1.0):
            cfgs.append((f"sweep {p}={v}", {**BASE, p: v}, []))
    for c in (0, 8):
        cfgs.append((f"sweep Console={c}", {**BASE, "Console": c}, []))
    for n in range(9):
        cfgs.append((f"preset {n}", {"Preset": n}, []))
    cfgs.append(("glitching over 40 frames", {"Console": 2, "Block Glitch": 0.6, "Line Glitch": 0.5,
                                             "Garbage": 0.4, "Glitch Rate": 0.8, "Wave": 0.4}, ["--frames", "40"]))
    cfgs.append(("flat 0.5 NES", {"Console": 2}, ["--flat", "0.5"]))
    cfgs.append(("alpha kept, half mix", {"Console": 4, "Mix": 0.5}, ["--alpha"]))
    return cfgs


def render(netest, out, w, h, sets, extra):
    args = [netest, "--out", out, "--width", str(w), "--height", str(h)] + extra
    for k, v in sets.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{netest} failed: {r.stdout}{r.stderr}")
    return pixels(out)


def pipe(netest, w, h, script, frames):
    args = [netest, "--pipe", "--width", str(w), "--height", str(h), "--script", script, "--fps", "30"]
    r = subprocess.run(args, input=frames, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"{netest} --pipe failed: {r.stderr.decode()}")
    return r.stdout


SOFTWARE_SWITCH = """
	const char* rendererChoice = std::getenv( "NETEST_RENDERER" );
	if( rendererChoice != nullptr && std::strcmp( rendererChoice, "software" ) == 0 )
	{
		const CGLPixelFormatAttribute generic[] = {
			kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
			kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
			kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
			kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
			static_cast< CGLPixelFormatAttribute >( 0 )
		};
		CGLPixelFormatObj format = nullptr;
		GLint formatCount        = 0;
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		CGLContextObj context = nullptr;
		const CGLError error  = CGLCreateContext( format, nullptr, &context );
		CGLDestroyPixelFormat( format );
		if( error != kCGLNoError )
			return nullptr;
		CGLSetCurrentContext( context );
		return context;
	}
"""


def build_reference():
    """The previous release's harness, from its tag, built once."""
    out = os.path.join(ROOT, "build", f"compat-{TAG}-r2")
    exe = os.path.join(out, "build", "netest")
    if os.path.exists(exe):
        return exe
    src = os.path.join(out, "src")
    os.makedirs(src, exist_ok=True)
    archive = subprocess.run(["git", "-C", ROOT, "archive", TAG], capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", src], input=archive, check=True)
    # The one change to the old tree, and it is to the harness, not the plugin:
    # v1.0.7's netest has no NETEST_RENDERER switch, so under the software
    # renderer it would quietly render on the GPU and every comparison would be
    # GPU-against-software. Give it the same switch this harness has.
    main = os.path.join(src, "tools", "netest", "main.cpp")
    text = open(main).read()
    anchor = "CGLContextObj createContext()\n{\n"
    assert anchor in text, "the old harness's createContext has moved"
    text = text.replace(anchor, anchor + SOFTWARE_SWITCH, 1)
    open(main, "w").write(text.replace("#include <cstdio>\n", "#include <cstdio>\n#include <cstdlib>\n", 1))
    ffgl = os.path.join(src, "external", "ffgl")
    if os.path.isdir(ffgl) and not os.listdir(ffgl):
        os.rmdir(ffgl)
    if not os.path.exists(ffgl):
        os.symlink(os.path.join(ROOT, "external", "ffgl"), ffgl)
    subprocess.run(["cmake", "-S", src, "-B", os.path.join(out, "build"), "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DBUILD_OFX=OFF"], check=True, capture_output=True)
    subprocess.run(["cmake", "--build", os.path.join(out, "build"), "--target", "netest", "--parallel", "4"],
                   check=True, capture_output=True)
    return exe


def run(netest, reference, sizes, jobs, quiet=False, retries=0):
    say = (lambda *a: None) if quiet else print
    failures = 0

    # 1. The parameter table.
    old = parse_params(open(MANIFEST).read())
    new_text = subprocess.run([netest, "--params"], capture_output=True, text=True, check=True).stdout
    new = parse_params(new_text)
    problems = compare_tables(old, new)
    added = sorted(set(new) - set(old))
    say(f"compat    {len(old)} parameters of {TAG} (its released bundle, probed): "
        f"{'unchanged' if not problems else 'CHANGED'}; {len(added)} appended "
        f"({', '.join(new[i]['name'] for i in added)})")
    for p in problems:
        say(f"compat      {p}")
    failures += len(problems)
    if added and min(added) != max(old) + 1:
        say("compat      FAILED: the new parameters do not start straight after the old ones")
        failures += 1

    # --list's index/name/default columns, old harness vs new, for the old rows.
    lo = subprocess.run([reference, "--list"], capture_output=True, text=True, check=True).stdout.splitlines()
    ln = subprocess.run([netest, "--list"], capture_output=True, text=True, check=True).stdout.splitlines()
    same = ln[:len(lo)] == lo
    say(f"compat    --list, {len(lo)} rows of {TAG}'s harness: {'identical' if same else 'DIFFERENT'}")
    failures += not same

    # --glitch prints the tick arithmetic; it must not have moved either.
    go = subprocess.run([reference, "--glitch"], capture_output=True, text=True).stdout
    gn = subprocess.run([netest, "--glitch"], capture_output=True, text=True).stdout
    say(f"compat    --glitch output: {'identical' if go == gn else 'DIFFERENT'}")
    failures += go != gn

    # 2. The pixels.
    tmp = tempfile.mkdtemp(prefix="necompat")
    cfgs = configurations()

    def one(job):
        (name, sets, extra), (w, h), k = job
        olds = [render(reference, f"{tmp}/{k}-old.png", w, h, sets, extra)]
        news = [render(netest, f"{tmp}/{k}-new.png", w, h, sets, extra)]
        # Apple's software renderer is not always repeatable at the last bit
        # (repousse measured it; here a last-bit change in a fract(sin()) glitch
        # hash moves whole cells). With --retries, a mismatch is re-rendered on
        # BOTH sides in fresh processes; identical means some old render and
        # some new render agree byte for byte, and the old build's agreement
        # with itself is reported beside it.
        for attempt in range(retries):
            if any(o == n for o in olds for n in news):
                break
            olds.append(render(reference, f"{tmp}/{k}-old{attempt}.png", w, h, sets, extra))
            news.append(render(netest, f"{tmp}/{k}-new{attempt}.png", w, h, sets, extra))
        same = any(o == n for o in olds for n in news)
        diff = 0 if same else sum(x != y for x, y in zip(olds[0][2], news[0][2]))
        old_repeatable = all(o == olds[0] for o in olds)
        return name, (w, h), same, diff, len(olds) - 1, old_repeatable

    job_list = [(c, s, i) for i, (c, s) in enumerate((c, s) for s in sizes for c in cfgs)]
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(one, job_list))
    bad = [r for r in results if not r[2]]
    for name, (w, h), ok, diff, tries, rep_ok in bad:
        say(f"compat      {name} at {w}x{h}: {diff} bytes differ"
            + (f" after {tries} re-renders (the old build {'agreed' if rep_ok else 'DISAGREED'} with itself)" if tries else ""))
    retried = [r for r in results if r[4] and r[2]]
    for name, (w, h), ok, diff, tries, rep_ok in retried:
        say(f"compat      {name} at {w}x{h}: identical on re-render {tries}; "
            f"the old build {'agreed' if rep_ok else 'disagreed'} with itself")
    say(f"compat    {len(results)} renders ({len(cfgs)} configurations x {len(sizes)} sizes): "
        f"{len(results) - len(bad)} byte-identical to {TAG}"
        + (f" ({len(retried)} after a re-render)" if retried else ""))
    failures += len(bad)

    # The pipe mode, with automation, over a short synthetic clip.
    w, h = min(sizes)
    script = f"{tmp}/cues.txt"
    open(script, "w").write("0 Console 2\n0 Block Glitch 0\n6 Block Glitch 0.6\n0 Palette Glitch 0.3\n"
                            "3 Wave 0.0\n9 Wave 0.5\n")
    frames = bytearray()
    for f in range(10):
        for y in range(h):
            for x in range(w):
                frames += bytes(((x * 3 + f * 7) & 255, (y * 5) & 255, ((x + y) * 2 + f) & 255, 255))
    pos, pns = [pipe(reference, w, h, script, bytes(frames))], [pipe(netest, w, h, script, bytes(frames))]
    for _ in range(retries):
        if any(o == n for o in pos for n in pns):
            break
        pos.append(pipe(reference, w, h, script, bytes(frames)))
        pns.append(pipe(netest, w, h, script, bytes(frames)))
    same = any(o == n for o in pos for n in pns)
    say(f"compat    --pipe, 10 frames with a cue sheet at {w}x{h}: {'byte-identical' if same else 'DIFFERENT'}"
        + (f" ({len(pos) - 1} re-renders; the old build {'agreed' if all(o == pos[0] for o in pos) else 'disagreed'} with itself)"
           if len(pos) > 1 else ""))
    failures += not same

    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--netest", default=os.path.join(ROOT, "build", "netest"))
    ap.add_argument("--reference")
    ap.add_argument("--size", action="append", help="WxH; repeatable. Default 1280x720 and 320x180.")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--negative", action="store_true")
    ap.add_argument("--retries", type=int, default=0,
                    help="on a mismatch, re-render both sides this many times (the software renderer)")
    a = ap.parse_args()
    sizes = [tuple(map(int, s.split("x"))) for s in (a.size or ["1280x720", "320x180"])]
    reference = a.reference or build_reference()

    if a.negative:
        insert = os.path.join(ROOT, "build", "netest_insert")
        if not os.path.exists(insert):
            print(f"compat: {insert} is not built (cmake --build build --target netest_insert)")
            return 1
        failures = run(insert, reference, sizes, a.jobs, quiet=True, retries=a.retries)
        caught = failures > 0
        print(f"negative  the Amiga inserted mid-list -> --compat                 "
              f"{'caught' if caught else 'NOT CAUGHT'}  ({failures} failures)")
        return 0 if caught else 1

    failures = run(a.netest, reference, sizes, a.jobs, retries=a.retries)
    print(f"compat: {'all ok' if failures == 0 else f'{failures} FAILURES'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
