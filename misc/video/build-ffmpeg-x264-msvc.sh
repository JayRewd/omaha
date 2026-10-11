#!/usr/bin/env bash
# Windows MSVC variant of build-ffmpeg-x264.sh.
# Prerequisites: MSVC (cl/link) on PATH via vcvarsall, an MSYS2 shell with
# make/perl/pkgconf/diffutils, and nasm for x86/x64.
# Static libs use the dynamic CRT (-MD) to match the default CMake runtime of the client.
#
# Environment:
#   FFMPEG_PREFIX       install prefix (default: build-deps/ffmpeg)
#   FFMPEG_SRC          download/extract dir (default: build-deps/ffmpeg-src)
#   FFMPEG_MSVC_ARCH    x64 | x86 | arm64 (default: x64)
#   FFMPEG_DISABLE_ASM  1 = build without assembly
#   DEP_API_FLAGS       extra cl flags (CI: Win7 API floor or /forceInterlockedFunctions-)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Pinned sources — keep in sync with build-ffmpeg-x264.sh.
FFMPEG_VER="7.1.1"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_SHA256="733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"
X264_COMMIT="0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee"
X264_REPO="https://code.videolan.org/videolan/x264.git"

to_win() { cygpath -m "$1" 2>/dev/null || echo "$1"; }

PREFIX="$(to_win "${FFMPEG_PREFIX:-$ROOT/build-deps/ffmpeg}")"
SRC="${FFMPEG_SRC:-$ROOT/build-deps/ffmpeg-src}"
JOBS="${JOBS:-${NUMBER_OF_PROCESSORS:-4}}"
MSVC_ARCH="${FFMPEG_MSVC_ARCH:-x64}"
DISABLE_ASM="${FFMPEG_DISABLE_ASM:-0}"

case "$MSVC_ARCH" in
	x64) FF_ARCH=x86_64; X264_HOST=x86_64-w64-mingw32 ;;
	x86) FF_ARCH=x86; X264_HOST=i686-w64-mingw32 ;;
	arm64) FF_ARCH=aarch64; X264_HOST=aarch64-w64-mingw32 ;;
	*) echo "ERROR: unsupported FFMPEG_MSVC_ARCH '$MSVC_ARCH'" >&2; exit 1 ;;
esac

# MSYS rewrites "/D..." arguments into paths when calling native tools; cl
# accepts "-D..." equally, so normalise every flag to the dash form.
API_FLAGS=""
for f in ${DEP_API_FLAGS:-}; do
	API_FLAGS="$API_FLAGS -${f#/}"
done
API_FLAGS="${API_FLAGS# }"

mkdir -p "$SRC" "$PREFIX" "$ROOT/misc/video/licenses"

if ! command -v cl >/dev/null 2>&1; then
	echo "ERROR: cl.exe not on PATH. Run from a VS developer / vcvarsall shell." >&2
	exit 1
fi
if [[ "$DISABLE_ASM" != "1" && "$FF_ARCH" != "aarch64" ]] && ! command -v nasm >/dev/null 2>&1; then
	echo "WARNING: nasm not on PATH — building without x86 asm (slower encode)." >&2
	DISABLE_ASM=1
fi
if [[ "$DISABLE_ASM" != "1" && "$FF_ARCH" == "aarch64" ]] && ! command -v armasm64 >/dev/null 2>&1; then
	echo "WARNING: armasm64 not on PATH — building without aarch64 asm (slower encode)." >&2
	DISABLE_ASM=1
fi

echo "=== FFmpeg+x264 (MSVC): arch=$MSVC_ARCH asm=$([[ $DISABLE_ASM == 1 ]] && echo off || echo on) api-flags='$API_FLAGS'"

download_verify() {
	local url="$1" dest="$2" sha="$3" got
	if [[ -f "$dest" ]]; then
		got="$(sha256sum "$dest" | awk '{print $1}')"
		if [[ "$got" == "$sha" ]]; then
			return 0
		fi
		rm -f "$dest"
	fi
	echo "Downloading $url"
	curl -fsSL --retry 3 -o "$dest" "$url"
	got="$(sha256sum "$dest" | awk '{print $1}')"
	if [[ "$got" != "$sha" ]]; then
		echo "ERROR: SHA256 mismatch for $dest" >&2
		echo "  expected: $sha" >&2
		echo "  got:      $got" >&2
		exit 1
	fi
}

# x264 has no release tarballs and GitLab's on-the-fly archives are not
# byte-stable, so fetch the pinned commit with git (the commit id is the checksum).
fetch_x264() {
	local dest="$1" repo="$SRC/x264-git"
	if [[ "$(git -C "$repo" rev-parse --verify -q "${X264_COMMIT}^{commit}" 2>/dev/null)" != "$X264_COMMIT" ]]; then
		rm -rf "$repo"
		git init -q "$repo"
		git -C "$repo" fetch -q --depth 1 "$X264_REPO" "$X264_COMMIT"
	fi
	if [[ "$(git -C "$repo" rev-parse "${X264_COMMIT}^{commit}")" != "$X264_COMMIT" ]]; then
		echo "ERROR: x264 fetch did not produce commit $X264_COMMIT" >&2
		exit 1
	fi
	git -C "$repo" archive --format=tar "$X264_COMMIT" | tar -xf - -C "$dest"
}

STAMP_KEY="msvc-${MSVC_ARCH}-asm${DISABLE_ASM}"

echo "=== Building static x264 + FFmpeg (MSVC) into $PREFIX ==="

# --- x264 ---
x264_dir="$SRC/x264-${X264_COMMIT}-${STAMP_KEY}"
if [[ ! -f "$PREFIX/lib/libx264.lib" ]] || [[ ! -f "$PREFIX/.x264.stamp" ]] ||
	[[ "$(cat "$PREFIX/.x264.stamp")" != "$X264_COMMIT-$STAMP_KEY" ]]; then
	rm -rf "$x264_dir"
	mkdir -p "$x264_dir"
	fetch_x264 "$x264_dir"
	pushd "$x264_dir" >/dev/null
	x264_cfg=(
		--prefix="$PREFIX"
		--host="$X264_HOST"
		--enable-static
		--disable-cli
		--disable-opencl
	)
	[[ "$DISABLE_ASM" == "1" ]] && x264_cfg+=(--disable-asm)
	[[ -n "$API_FLAGS" ]] && x264_cfg+=(--extra-cflags="$API_FLAGS")
	# x264 picks up CFLAGS from the environment; CI exports a cl-style value on arm64.
	env -u CFLAGS CC=cl ./configure "${x264_cfg[@]}"
	make -j"$JOBS"
	make install
	popd >/dev/null
	echo "$X264_COMMIT-$STAMP_KEY" >"$PREFIX/.x264.stamp"
	cp -f "$x264_dir/COPYING" "$ROOT/misc/video/licenses/x264-COPYING" 2>/dev/null || true
fi
# FFmpeg's msvc toolchain maps -lx264 (from x264.pc) to x264.lib.
cp -f "$PREFIX/lib/libx264.lib" "$PREFIX/lib/x264.lib"

# FFmpeg's aarch64 armasm64 path passes -o twice through gas-preprocessor and
# never writes the objects. x264 keeps its NEON asm (the encode hot path); only
# the FFmpeg libs (swscale/AAC) build as plain C on Windows arm64.
FF_DISABLE_ASM="$DISABLE_ASM"
[[ "$FF_ARCH" == "aarch64" ]] && FF_DISABLE_ASM=1

# --- FFmpeg ---
tarball="$SRC/ffmpeg-${FFMPEG_VER}.tar.xz"
ff_dir="$SRC/ffmpeg-${FFMPEG_VER}-${STAMP_KEY}"
download_verify "$FFMPEG_URL" "$tarball" "$FFMPEG_SHA256"
if [[ ! -f "$PREFIX/lib/libavcodec.a" && ! -f "$PREFIX/lib/avcodec.lib" ]] || [[ ! -f "$PREFIX/.ffmpeg.stamp" ]] ||
	[[ "$(cat "$PREFIX/.ffmpeg.stamp")" != "$FFMPEG_VER-$STAMP_KEY" ]]; then
	rm -rf "$ff_dir"
	mkdir -p "$ff_dir"
	tar -xf "$tarball" -C "$ff_dir" --strip-components=1
	pushd "$ff_dir" >/dev/null
	# pkgconf splits search paths on ':', so "D:/..." must be given in POSIX form.
	pc_dir="$(cygpath -u "$PREFIX/lib/pkgconfig" 2>/dev/null || echo "$PREFIX/lib/pkgconfig")"
	export PKG_CONFIG_PATH="$pc_dir"
	export PKG_CONFIG_LIBDIR="$pc_dir"
	ff_cfg=(
		--prefix="$PREFIX"
		--toolchain=msvc
		--target-os=win32
		--arch="$FF_ARCH"
		--pkg-config=pkg-config
		--pkg-config-flags=--static
		--extra-cflags="-MD -I$PREFIX/include $API_FLAGS"
		--extra-ldflags="-LIBPATH:$PREFIX/lib"
		--enable-static
		--disable-shared
		--disable-debug
		--disable-doc
		--disable-programs
		--disable-autodetect
		--disable-network
		--disable-everything
		--enable-gpl
		--enable-libx264
		--enable-encoder=libx264,aac
		--enable-muxer=mp4
		--enable-protocol=file
		--enable-swscale
		--enable-bsf=aac_adtstoasc
	)
	[[ "$FF_ARCH" != "x86_64" ]] && ff_cfg+=(--enable-cross-compile)
	[[ "$FF_DISABLE_ASM" == "1" ]] && ff_cfg+=(--disable-asm)
	env -u CFLAGS ./configure "${ff_cfg[@]}" || {
		tail -n 60 ffbuild/config.log >&2
		exit 1
	}
	make -j"$JOBS"
	make install
	popd >/dev/null
	echo "$FFMPEG_VER-$STAMP_KEY" >"$PREFIX/.ffmpeg.stamp"
	cp -f "$ff_dir/COPYING.GPLv2" "$ROOT/misc/video/licenses/ffmpeg-COPYING.GPLv2" 2>/dev/null || true
fi

echo "=== Done (MSVC $MSVC_ARCH). -DFFMPEG_ROOT=$PREFIX ==="
ls -la "$PREFIX/lib/"
