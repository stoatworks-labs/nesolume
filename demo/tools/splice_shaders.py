"""Copy the plugin's shaders into demo/plugin.js, unedited.

    python3 demo/tools/splice_shaders.py

Replaces each existing `const NAME = `...`;` literal in demo/plugin.js with the
body of the matching `R"( ... )"` in source/shaders/*.cpp, exactly as it is
there, tabs and comments included (each carries its own `#version 410 core`
line, which the kit's port() swaps for ES 3.00's). Then run check_shaders.py,
which is what verify.sh runs and what actually guards the copy. A backtick, a
`${` or a backslash in a shader would change meaning inside a template
literal, so any one of them stops the splice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True  # no __pycache__ in demo/tools/
sys.path.insert(0, HERE)
from check_shaders import SHADERS, from_cpp  # noqa: E402


def main():
    path = os.path.join(REPO, "demo", "plugin.js")
    js = open(path).read()

    for name, cpp_path, symbol in SHADERS:
        body = from_cpp(cpp_path, symbol)
        if body is None:
            print(f"{symbol} not found in {cpp_path}")
            return 1
        if "`" in body or "${" in body or "\\" in body:
            print(f"{symbol} holds a backtick, ${{ or a backslash; splice it by hand and teach check_shaders.py")
            return 1
        block = f"const {name} = `{body}`;"
        js, n = re.subn(r"^const " + name + r" = `.*?`;$", lambda _m: block, js, flags=re.S | re.M)
        if n != 1:
            print(f"{name} not found once in demo/plugin.js")
            return 1

    open(path, "w").write(js)
    print(f"spliced {len(SHADERS)} shaders into demo/plugin.js")
    return 0


if __name__ == "__main__":
    sys.exit(main())
