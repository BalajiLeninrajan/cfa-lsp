#!/usr/bin/env bash
# Install the cs343 CFA compiler locally from the student server.
#
#   toolchain/install-cfa.sh [user@host] [prefix]
#
# The server's cfa driver has its install path compiled in and refuses to run
# from anywhere else, so this copies the toolchain (translator, cc1 wrapper,
# libcfa, headers) and rebuilds only the small driver from source with a local
# prefix. It then patches three things for a newer host:
#
#   * the driver tells cc1 to use plain `gcc` (cc1 defaults to gcc-11)
#   * glibc 2.43's variadic assert, which cfa-cpp 1.0.0 cannot resolve
#   * glibc 2.43's C23 macros (bsearch, strchr, ...) that break CFA overloads
#
# Safe to re-run: rsync only copies changes and each patch checks a marker.

set -euo pipefail

HOST=${1:-bleninra@linux.student.cs.uwaterloo.ca}
PREFIX=${2:-$HOME/.local/share/cfa-cc}
REMOTE=${CFA_REMOTE:-/u6/cs343/cfa-cc}
BIN=${CFA_BIN_LINK:-$HOME/.local/bin/cfa}
# cforall/cforall commit whose driver matches the server's CFA 1.0.0 build
CFA_COMMIT=${CFA_COMMIT:-fade1b551c966a956866b7fea0fa38b9e8ac4885}

echo "== copying toolchain from $HOST:$REMOTE"
mkdir -p "$PREFIX/bin"
rsync -az "$HOST:$REMOTE/include" "$HOST:$REMOTE/lib" "$HOST:$REMOTE/share" "$PREFIX/"

echo "== building the driver for $PREFIX"
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git -C "$src" init -q
git -C "$src" remote add origin https://github.com/cforall/cforall
git -C "$src" config core.sparseCheckout true
printf '/driver/\n/src/Common/\n/src/AST/\n/src/include/\n' > "$src/.git/info/sparse-checkout"
git -C "$src" fetch -q --depth 1 --filter=blob:none origin "$CFA_COMMIT"
git -C "$src" checkout -q FETCH_HEAD

cat > "$src/driver/config.h" <<EOF
#define CFA_VERSION_LONG "1.0.0"
#define CFA_VERSION_MAJOR 1
#define CFA_VERSION_MINOR 0
#define CFA_VERSION_PATCH 0
#define CFA_BINDIR "$PREFIX/bin/"
#define CFA_INCDIR "$PREFIX/include/cfa/"
#define CFA_LIBDIR "$PREFIX/lib/cfa/"
#define TOP_SRCDIR "/nonexistent-cfa-srcdir/"
#define TOP_BUILDDIR "/nonexistent-cfa-builddir/"
#define CFA_BACKEND_CC "gcc"
#define CFA_32_CPU "x86"
#define CFA_64_CPU "x64"
#define CFA_DEFAULT_CPU "x64"
#define HAVE_CAST_FUNCTION_TYPE 1
EOF

python3 - "$src/driver/cfa.cc" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
anchor = '\tPutenv( argv, string("-B=") + bprefix );'
assert s.count(anchor) == 1, "driver source changed; update the patch"
s = s.replace(anchor, anchor + '\n\tPutenv( argv, "-compiler=" + compiler_path );\t// cc1 must use this backend, not its built-in gcc-11')
open(p, "w").write(s)
EOF

g++ -std=c++17 -O2 -w -I "$src/driver" -I "$src/src" -I "$src/src/include" \
    -include Common/ToString.hpp -o "$PREFIX/bin/cfa" "$src/driver/cfa.cc"

echo "== patching headers for glibc"
python3 - "$PREFIX/include/cfa/stdhdr" <<'EOF'
import sys
from pathlib import Path
d = Path(sys.argv[1])
MARK = "Local patch:"

def patch(name, text):
    p = d / name
    s = p.read_text()
    if MARK in s:
        print(f"   {name}: already patched")
        return
    inc = next(l for l in s.splitlines(True) if l.startswith(f"#include_next <{name}>"))
    p.write_text(s.replace(inc, inc + text, 1))
    print(f"   {name}: patched")

patch("assert.h", """
// Local patch: glibc 2.43 made assert variadic (__assert_single_arg), which
// cfa-cpp 1.0.0 cannot resolve. Restore the classic single-argument form.
#if ! defined(NDEBUG) && defined(__ASSERT_VARIADIC) && __ASSERT_VARIADIC
	#undef assert
	#define assert(expr) ((expr) ? __ASSERT_VOID_CAST(0) : __assert_fail(#expr, __FILE__, __LINE__, __ASSERT_FUNCTION))
#endif
""")
c23 = ("\n// Local patch: glibc 2.43 defines C23 qualifier-preserving macros for these\n"
       "// under _GNU_SOURCE, which break CFA's overloads. The functions stay declared.\n")
patch("stdlib.h", c23 + "#undef bsearch\n")
patch("string.h", c23 + "".join(f"#undef {m}\n" for m in ("memchr", "strchr", "strrchr", "strpbrk", "strstr")))
patch("wchar.h", c23 + "".join(f"#undef {m}\n" for m in ("wcschr", "wcsrchr", "wcspbrk", "wcsstr", "wmemchr")))
EOF

mkdir -p "$(dirname "$BIN")"
ln -sf "$PREFIX/bin/cfa" "$BIN"

echo "== checking"
probe=$(mktemp -d)
printf '#include <fstream.hfa>\n#include <string.hfa>\nint main() { string s = "ok"; sout | s; }\n' > "$probe/p.cfa"
"$BIN" -quiet -c "$probe/p.cfa" -o "$probe/p.o"
rm -rf "$probe"
echo "cfa works: $BIN -> $PREFIX/bin/cfa"
