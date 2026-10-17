#!/bin/sh
# Builds qemu-system-arm (wasm32) with the SSD1306 model.
#
# This is a script of what was actually run, not a guess at what should work.
# Every workaround below exists because the obvious thing failed; the comment
# says which way.
#
# Requirements: emsdk (tested 4.0.9), a wasm32 glib, zlib, libffi and pcre2
# under $TARGET, and a checkout of ktock/qemu-wasm (QEMU 8.2 + tcg/wasm32).

set -eu

QEMU_SRC="${QEMU_SRC:-$PWD/qemu-wasm}"
TARGET="${TARGET:-$PWD/target}"
BUILD="$QEMU_SRC/build"

test -d "$QEMU_SRC/tcg/wasm32" || {
	echo "no tcg/wasm32 in $QEMU_SRC: clone https://github.com/ktock/qemu-wasm" >&2
	exit 1
}

# pkg-config must see only the wasm tree. QEMU bakes a bare "pkg-config" into its
# meson cross file, so a wrapper is the only way to redirect it.
cat > "$PWD/wasm-pkg-config" <<EOF
#!/bin/sh
PKG_CONFIG_LIBDIR=$PWD/pc exec /usr/bin/pkg-config "\$@"
EOF
chmod +x "$PWD/wasm-pkg-config"
export PKG_CONFIG="$PWD/wasm-pkg-config"

CFLAGS="-O2 -DNDEBUG -matomics -mbulk-memory -pthread -sWASM_BIGINT"
CFLAGS="$CFLAGS -Wno-error=incompatible-function-pointer-types"
CFLAGS="$CFLAGS -Wno-unused-command-line-argument"
CFLAGS="$CFLAGS -I$TARGET/include -I$TARGET/include/glib-2.0"
CFLAGS="$CFLAGS -I$TARGET/lib/glib-2.0/include -I$TARGET/include/pixman-1"

# libresolv.a is our own stub: it provides posix_spawnp, which glib's gspawn
# references but wasm cannot implement.
LDFLAGS="-L$TARGET/lib -pthread $TARGET/lib/libresolv.a"

emconfigure ./configure --static --target-list=arm-softmmu \
	--cpu=wasm32 --cross-prefix= --without-default-features --enable-system \
	--with-coroutine=fiber --disable-werror --disable-docs \
	--extra-cflags="$CFLAGS" --extra-ldflags="$LDFLAGS"

# meson's has_function() probes with __builtin_posix_memalign, which emscripten's
# clang does not provide, so the probe reports NO even though stdlib.h declares
# it and it works. Without this, util/memalign.c refuses to compile.
sed -i 's/#undef CONFIG_POSIX_MEMALIGN/#define CONFIG_POSIX_MEMALIGN 1/' \
	"$BUILD/config-host.h"

# The symbol response files are generated empty for this configuration (they only
# matter for shared modules) and emcc will not expand a nested @file. Drop the
# references rather than teaching emcc about them.
sed -i 's/ @block\.syms//; s/ @qemu\.syms//' "$BUILD/build.ninja"

# Link flags. MODULARIZE lets one .wasm take any firmware at runtime instead of
# baking one in, and the exports are what the page and the tests read the panel
# through.
python3 - "$BUILD/build.ninja" <<'PY'
import pathlib, sys

path = pathlib.Path(sys.argv[1])
lines = path.read_text().split('\n')
out, in_rule = [], False
extra = (
    ' -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB'
    ' -sASYNCIFY=1 -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM=1'
    ' -sWASM_BIGINT -sMALLOC=mimalloc'
    ' -sMODULARIZE=1 -sEXPORT_NAME=createQEMU -sINVOKE_RUN=0'
    ' -sALLOW_TABLE_GROWTH=1 -sEXPORT_KEEPALIVE=1'
    ' -sEXPORTED_FUNCTIONS=_main,_ssd1306_get_framebuffer,'
    '_ssd1306_framebuffer_size,_malloc,_free'
    ' -sEXPORTED_RUNTIME_METHODS=callMain,FS,TTY,ccall,cwrap,'
    'addFunction,removeFunction,getTempRet0,setTempRet0,HEAPU8,HEAPU32'
)
for line in lines:
    if line.startswith('build qemu-system-arm: c_LINKER_RSP'):
        in_rule = True
    elif in_rule and line.startswith(' LINK_ARGS = '):
        if 'sMODULARIZE' not in line:
            line += extra
        in_rule = False
    elif in_rule and line and not line.startswith(' '):
        in_rule = False
    out.append(line)
path.write_text('\n'.join(out))
PY

# binaryen 117 does not know two feature flags that emscripten 4.0.9 passes to
# wasm-opt, so every optimised link dies with "Unknown option". The shim drops
# just those two and execs the real binary.
mkdir -p "$PWD/binaryen-shim/bin"
cat > "$PWD/binaryen-shim/bin/wasm-opt" <<'EOF'
#!/usr/bin/env python3
import os, sys
DROP = {"--enable-bulk-memory-opt", "--enable-call-indirect-overlong"}
args = [a for a in sys.argv[1:] if a not in DROP]
real = os.environ.get("REAL_WASM_OPT", "wasm-opt")
os.execv(real, [real] + args)
EOF
chmod +x "$PWD/binaryen-shim/bin/wasm-opt"

export BINARYEN="$PWD/binaryen-shim"
ninja -C "$BUILD" qemu-system-arm

echo
echo "built $BUILD/qemu-system-arm.wasm"
ls -l "$BUILD/qemu-system-arm.wasm"
