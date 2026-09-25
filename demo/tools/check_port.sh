#!/usr/bin/env bash
#
# The browser demo's CPU half (demo/amiga.js) against the plugin's own C++.
#
#   demo/tools/check_port.sh [--quick]
#
# check_port.mjs compiles refamiga.cpp against source/Amiga.cpp (unchanged),
# as built and with -ffp-contract=off, and compares HAM6 encodes, register
# choices, the fixed tables and the field count exactly. It says what it
# covers and what it cannot. Called from tools/verify.sh; exits 3 (skip)
# without node or a C++ compiler.
#
set -uo pipefail
cd "$(dirname "$0")/../.."

command -v node >/dev/null 2>&1 || { echo "skipped: node not installed"; exit 3; }
command -v "${CXX:-c++}" >/dev/null 2>&1 || { echo "skipped: no C++ compiler"; exit 3; }
node demo/tools/check_port.mjs "$@"
