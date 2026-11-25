#!/usr/bin/env bash
# Cross-compiles the bridge for the Raspberry Pi Zero W (ARMv6, hard float) as one static file
# that runs on any Raspberry Pi OS, 32- or 64-bit, and on the SD-card kit's Alpine Linux.
#
#   scripts/build-pi.sh [build-dir]        ->  <build-dir>/duoplay-bridge
#
# Needs: Python 3, CMake, make, perl, curl, git, protoc and Python's protobuf module (the
# same as a normal build). The compiler is Zig's clang + musl, installed with pip into the
# build folder; OpenSSL is built for the Pi first. Then make the SD-card kit with:
#   python3 scripts/make-pi-kit.py --bridge <build-dir>/duoplay-bridge --out dist
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "${1:-$ROOT/build-pi}" && cd "${1:-$ROOT/build-pi}" && pwd)
X=$OUT/xarm
ZIG_VERSION=0.17.1
OPENSSL_VERSION=3.0.15
OPENSSL_SHA256=23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533
TARGET=arm-linux-musleabihf
CPU=arm1176jzf_s # the Pi Zero's ARM11; also runs on every later Pi
mkdir -p "$X/bin"

# --- compiler -------------------------------------------------------------------------------
if [ ! -x "$X/zigenv/bin/python" ]; then
	python3 -m venv "$X/zigenv"
	"$X/zigenv/bin/pip" install --quiet "ziglang==$ZIG_VERSION"
fi
ZIG=$("$X/zigenv/bin/python" -c 'import os, ziglang; print(os.path.join(os.path.dirname(ziglang.__file__), "zig"))')
for tool in cc c++; do
	name=$([ "$tool" = cc ] && echo zcc || echo zcxx)
	cat > "$X/bin/$name" <<EOF
#!/bin/sh
# Drop linker flags zig doesn't take for this target.
for a in "\$@"; do case "\$a" in -Wl,--version-script*|-Wl,-z,defs) ;; *) set -- "\$@" "\$a";; esac; shift; done
exec "$ZIG" $tool -target $TARGET -mcpu=$CPU "\$@"
EOF
done
printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$X/bin/zar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$X/bin/zranlib"
chmod +x "$X/bin/"*

# --- OpenSSL for the Pi ---------------------------------------------------------------------
if [ ! -f "$X/sysroot/lib/libcrypto.a" ]; then
	tarball=$X/openssl-$OPENSSL_VERSION.tar.gz
	[ -f "$tarball" ] || curl -fsSL -o "$tarball" \
		"https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
	echo "$OPENSSL_SHA256  $tarball" | sha256sum -c - >/dev/null
	rm -rf "$X/openssl" && mkdir -p "$X/openssl"
	tar xzf "$tarball" -C "$X/openssl" --strip-components=1
	(cd "$X/openssl" &&
		./Configure linux-armv4 no-shared no-tests no-async no-module no-dso no-engine -static \
			--prefix="$X/sysroot" --libdir=lib --openssldir=/etc/ssl \
			CC="$X/bin/zcc" AR="$X/bin/zar" RANLIB="$X/bin/zranlib" > "$X/openssl-configure.log" &&
		make -j"$(nproc)" build_libs > "$X/openssl-build.log" 2>&1 &&
		make install_dev > /dev/null)
fi

# --- the bridge -----------------------------------------------------------------------------
cmake -S "$ROOT/bridge" -B "$OUT" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/bridge/cmake/zig-armv6.cmake" -DDUOPLAY_XARM="$X" > /dev/null
cmake --build "$OUT" -j"$(nproc)"
echo
file "$OUT/duoplay-bridge" 2>/dev/null || ls -l "$OUT/duoplay-bridge"
