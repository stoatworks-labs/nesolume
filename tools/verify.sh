#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh              arm64 dev build in build/, then every check
#   tools/verify.sh --universal  a fresh universal Release build in build-verify/
#                                first, plus lipo, the plist and oxbow probe
#
# The shaders through glslc; the harness's own checks (--glitch, --names, and
# the Amiga's --ham-edge, --ham-optimal, --ehb, --palette, --lace) at two
# rasters on the GPU and at CI's 320x180 on Apple's software renderer; the
# negative controls; --compat against the previous release; verify.py's palette
# legality; the sweep; the browser demo's shader copies and its Amiga port
# (demo/tools/); and the pipe mode's broken-pipe exit.
#
# The general lesson the file exists for: a check that only ever runs in CI,
# after a tag, is a check that will catch you after the tag. Anything that can
# be done locally is done here, where it costs a second.
#
set -uo pipefail

cd "$(dirname "$0")/.."

failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/shaders/Display.cpp",
	"source/shaders/Downres.cpp",
	"source/shaders/Quantize.cpp",
	"source/shaders/Tile.cpp",
	"source/shaders/Vertex.cpp",
]

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures=$(( failures + 1 ))
fi

#---------------------------------------------------------------------------
# GLSL 4.10's reserved words, which Apple's compiler and glslc (4.5) both let
# through and Mesa -- the Arena gate's llvmpipe -- refuses. `packed` cost a
# sibling a gate run. Comments stripped first; the shader text only.
#---------------------------------------------------------------------------
step "shaders: no GLSL 4.10 reserved word used as a name"
if python3 - <<'RESERVED_PY'
import re, sys, pathlib
words = set("""attribute const uniform varying layout centroid flat smooth noperspective patch sample
subroutine in out inout invariant precise discard struct common partition active asm class union enum
typedef template this packed resource goto inline noinline public static extern external interface
long short half fixed unsigned superp input output hvec2 hvec3 hvec4 fvec2 fvec3 fvec4 sampler3DRect
filter image1D image2D image3D imageCube iimage1D iimage2D uimage1D uimage2D sizeof cast namespace using
row_major""".split())
# Keywords the shaders legitimately use as keywords.
allowed = {"const", "uniform", "in", "out", "inout", "flat", "smooth", "layout", "discard", "struct"}
bad = []
for f in sorted(pathlib.Path("source/shaders").glob("*.cpp")):
    for body in re.findall(r'R"\((.*?)\)"', f.read_text(), re.S):
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", body, flags=re.S)
        for w in re.findall(r"[A-Za-z_]\w*", text):
            if w in words and w not in allowed:
                bad.append(f"{f.name}: {w}")
print("   " + ("\n   ".join(sorted(set(bad))) if bad else "none"))
sys.exit(1 if bad else 0)
RESERVED_PY
then :; else failures=$(( failures + 1 )); fi

#---------------------------------------------------------------------------
# The MSVC traps a Mac build cannot see: no M_PI without _USE_MATH_DEFINES,
# far/near are windef.h macros, no __builtin_*.
#---------------------------------------------------------------------------
step "C++: nothing MSVC will refuse"
if grep -nE '\bM_PI\b|__builtin_|__attribute__|\bfar\b|\bnear\b' source/*.cpp source/*.h 2>/dev/null \
	| grep -v '^\s*//' | grep -vE ':[0-9]+:\s*(//|\*)'; then
	printf '   found (above)\n'
	failures=$(( failures + 1 ))
else
	printf '   none\n'
fi

#---------------------------------------------------------------------------
# The build.
#---------------------------------------------------------------------------
BUILD=build
if [ "${1:-}" = "--universal" ]; then
	BUILD=build-verify
	step "build: fresh universal Release ($BUILD)"
	rm -rf "$BUILD"
	cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DNESOLUME_BUILD_TOOLS=ON >/dev/null \
		&& cmake --build "$BUILD" --config Release --parallel 4 >/dev/null \
		|| { printf '   build FAILED\n'; exit 1; }
else
	step "build: arm64 dev ($BUILD)"
	cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null \
		&& cmake --build "$BUILD" --parallel 4 >/dev/null \
		|| { printf '   build FAILED\n'; exit 1; }
fi
cmake --build "$BUILD" --target netest_insert --parallel 4 >/dev/null || { printf '   netest_insert FAILED\n'; exit 1; }
printf '   ok\n'
NETEST="./$BUILD/netest"

run() {
	# A check's own lines are printed; its exit status decides.
	local label="$1"; shift
	local out
	if out="$( "$@" 2>&1 )"; then
		printf '%s\n' "$out" | grep -v '^GL \|^netest: NETEST_RENDERER' | sed 's/^/   /'
	else
		printf '%s\n' "$out" | sed 's/^/   /'
		printf '   \033[31m%s FAILED\033[0m\n' "$label"
		failures=$(( failures + 1 ))
	fi
}

step "harness: parameters and the glitch clock (no GL)"
run names "$NETEST" --names
run glitch "$NETEST" --glitch
run ham-optimal "$NETEST" --ham-optimal

for size in 1280x1024 320x180; do
	step "the Amiga, GPU, $size"
	for check in --ham-edge --ehb --palette --lace --negative; do
		run "$check $size" "$NETEST" "$check" --size "$size"
	done
done

step "the Amiga, Apple's software renderer (CI's), 320x180"
for check in --ham-edge --ehb --palette --lace --negative; do
	run "$check software" env NETEST_RENDERER=software "$NETEST" "$check" --size 320x180
done

step "compat: every existing console bit-identical to the previous release"
run compat python3 tools/compat.py --netest "$NETEST" --jobs 4
run "compat negative" python3 tools/compat.py --negative --size 320x180 --jobs 4
run "compat software" env NETEST_RENDERER=software python3 tools/compat.py --netest "$NETEST" --size 320x180 --jobs 2 --retries 3

step "palette legality of the fixed-palette consoles (verify.py)"
run verify.py python3 tools/verify.py

step "sweep: every control moves pixels"
run sweep python3 tools/sweep.py --jobs 4

#---------------------------------------------------------------------------
# The browser demo: its shaders are the plugin's, character for character, and
# its port of the Amiga (demo/amiga.js) agrees with source/Amiga.cpp exactly on
# check_port's cases. No browser needed; check_port.sh skips (exit 3) without
# node or a compiler.
#---------------------------------------------------------------------------
step "demo: the page's shaders and its Amiga port"
run check_shaders python3 demo/tools/check_shaders.py
out=$( demo/tools/check_port.sh 2>&1 )
status=$?
if [ "$status" -eq 0 ]; then
	printf '%s\n' "$out" | sed 's/^/   /'
elif [ "$status" -eq 3 ]; then
	printf '   %s\n' "$out"
else
	printf '%s\n' "$out" | sed 's/^/   /'
	printf '   \033[31mcheck_port FAILED\033[0m -- demo/amiga.js no longer agrees with source/Amiga.cpp\n'
	failures=$(( failures + 1 ))
fi

#---------------------------------------------------------------------------
# --pipe with the reader gone: the documented exit 1 and its message, not
# SIGPIPE's silent 141 (rebate's bug, which a sibling copying its pipe code
# would share).
#---------------------------------------------------------------------------
step "pipe: a reader that goes away gets exit 1, not SIGPIPE"
head -c $(( 64 * 36 * 4 * 8 )) /dev/zero | "$NETEST" --pipe --width 64 --height 36 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[1]}
if [ "$status" -eq 1 ]; then
	printf '   exit %d\n' "$status"
else
	printf '   exit %d, want 1\n' "$status"
	failures=$(( failures + 1 ))
fi

#---------------------------------------------------------------------------
# The shipped artefact, when it was built here: both slices, the plist, and
# what a host sees.
#---------------------------------------------------------------------------
if [ "$BUILD" = "build-verify" ]; then
	step "bundle: lipo, plist, ad-hoc signature, oxbow probe"
	archs="$( lipo -archs "$BUILD/NESolume.bundle/Contents/MacOS/NESolume" )"
	printf '   lipo: %s\n' "$archs"
	case "$archs" in *arm64*x86_64*|*x86_64*arm64*) ;; *) failures=$(( failures + 1 ));; esac
	# cmake/Info.plist.in writes CFBundleVersion only (it always has).
	ver="$( /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$BUILD/NESolume.bundle/Contents/Info.plist" 2>/dev/null )"
	want="$( sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"
	printf '   plist version %s (CMakeLists %s)\n' "$ver" "$want"
	[ "$ver" = "$want" ] || failures=$(( failures + 1 ))
	codesign --force --sign - "$BUILD/NESolume.bundle" >/dev/null 2>&1 \
		&& codesign --verify "$BUILD/NESolume.bundle" && printf '   ad-hoc signature verifies\n' \
		|| failures=$(( failures + 1 ))
	OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
	if [ -x "$OXBOW" ]; then
		probe="$( "$OXBOW" probe "$BUILD/NESolume.bundle" 2>&1 )"
		printf '%s\n' "$probe" | grep -E '^(name|id|type|params):' | sed 's/^/   /'
		printf '%s\n' "$probe" | grep -q '^name: *SW NESolume$' && printf '%s\n' "$probe" | grep -q '^id: *NE01$' \
			&& printf '%s\n' "$probe" | grep -q '^type: *effect$' || failures=$(( failures + 1 ))
		# The host's view of the table against the previous release's.
		if printf '%s\n' "$probe" | python3 -c '
import sys; sys.path.insert(0, "tools"); import compat
old = compat.parse_params(open(compat.MANIFEST).read()); new = compat.parse_params(sys.stdin.read())
p = compat.compare_tables(old, new); print("   host view vs v1.0.7:", "unchanged" if not p else p); sys.exit(1 if p else 0)'; then :
		else failures=$(( failures + 1 )); fi
	else
		printf '   oxbow not found (%s): probe skipped\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
	exit 0
fi
printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
exit 1
