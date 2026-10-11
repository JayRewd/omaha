#!/usr/bin/env bash
# Build a minimal static FFmpeg + x264 for Project: Omaha demo export.
# Output: $FFMPEG_PREFIX/{include,lib,lib/pkgconfig}
# Developers/CI only — end users do not need this (libs link into openmohaa).
#
# Environment:
#   FFMPEG_PREFIX       install prefix (default: build-deps/ffmpeg)
#   FFMPEG_SRC          download/extract dir (default: build-deps/ffmpeg-src)
#   FFMPEG_HOST_TRIPLE  target triple when cross compiling (e.g. i686-linux-gnu)
#   FFMPEG_ARCH         FFmpeg --arch (x86_64, x86, aarch64, arm, ppc, ppc64)
#   FFMPEG_TARGET_OS    FFmpeg --target-os (linux, darwin; default from uname)
#   FFMPEG_DISABLE_ASM  1 = build without assembly (slower, always portable)
#   CC / CXX / CFLAGS / LDFLAGS  honored (CI passes clang --target=...)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${FFMPEG_PREFIX:-$ROOT/build-deps/ffmpeg}"
SRC="${FFMPEG_SRC:-$ROOT/build-deps/ffmpeg-src}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Pinned sources (FFmpeg: SHA256 of the release tarball; x264: git commit).
FFMPEG_VER="7.1.1"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_SHA256="733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"

X264_COMMIT="0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee"
X264_REPO="https://code.videolan.org/videolan/x264.git"

mkdir -p "$SRC" "$PREFIX" "$ROOT/misc/video/licenses"

have_cmd() { command -v "$1" >/dev/null 2>&1; }

sha256_of() {
	if have_cmd sha256sum; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

# Prefer a locally built nasm from a previous helper install.
if [[ -x "$ROOT/build-deps/nasm/bin/nasm" ]]; then
	export PATH="$ROOT/build-deps/nasm/bin:$PATH"
fi

# CI writes CFLAGS='--target=...' into GITHUB_ENV with literal quotes.
CFLAGS="${CFLAGS:-}"
CFLAGS="${CFLAGS//\'/}"
LDFLAGS="${LDFLAGS:-}"
LDFLAGS="${LDFLAGS//\'/}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"

BUILD_TRIPLE="$("$CC" -dumpmachine 2>/dev/null || true)"
HOST_TRIPLE="${FFMPEG_HOST_TRIPLE:-}"
TARGET_OS="${FFMPEG_TARGET_OS:-}"
if [[ -z "$TARGET_OS" ]]; then
	case "$(uname -s)" in
		Darwin) TARGET_OS=darwin ;;
		*) TARGET_OS=linux ;;
	esac
fi
DISABLE_ASM="${FFMPEG_DISABLE_ASM:-0}"

CROSS=0
if [[ -n "$HOST_TRIPLE" ]]; then
	if [[ "$TARGET_OS" == "darwin" ]]; then
		# macOS slices are always configured explicitly (-arch).
		CROSS=1
	elif [[ "$HOST_TRIPLE" != "$BUILD_TRIPLE" ]]; then
		CROSS=1
	fi
fi
if [[ "$CROSS" == "1" && -z "${FFMPEG_ARCH:-}" ]]; then
	echo "ERROR: FFMPEG_ARCH is required when FFMPEG_HOST_TRIPLE is set" >&2
	exit 1
fi

# Per-target tuning.
case "${FFMPEG_ARCH:-}" in
	arm)
		# armhf baseline is ARMv7 + VFPv3/NEON; clang's default for the triple may be older.
		CFLAGS="$CFLAGS -march=armv7-a -mfpu=neon -mfloat-abi=hard"
		;;
esac

if [[ "$DISABLE_ASM" != "1" ]]; then
	case "${FFMPEG_ARCH:-$(uname -m)}" in
		x86_64 | x86 | i686 | amd64)
			if ! have_cmd nasm; then
				echo "WARNING: nasm not found — building without x86 asm (slower encode)." >&2
				DISABLE_ASM=1
			fi
			;;
	esac
fi

echo "=== FFmpeg+x264: target-os=$TARGET_OS arch=${FFMPEG_ARCH:-native} host=${HOST_TRIPLE:-native} cross=$CROSS asm=$([[ $DISABLE_ASM == 1 ]] && echo off || echo on)"
echo "    CC=$CC CFLAGS=$CFLAGS LDFLAGS=$LDFLAGS"

download_verify() {
	local url="$1" dest="$2" sha="$3" got
	if [[ -f "$dest" ]]; then
		got="$(sha256_of "$dest")"
		if [[ "$got" == "$sha" ]]; then
			return 0
		fi
		echo "Checksum mismatch for $dest — re-downloading" >&2
		rm -f "$dest"
	fi
	echo "Downloading $url"
	curl -fsSL --retry 3 -o "$dest" "$url"
	got="$(sha256_of "$dest")"
	if [[ "$got" != "$sha" ]]; then
		echo "ERROR: SHA256 mismatch for $dest" >&2
		echo "  expected: $sha" >&2
		echo "  got:      $got" >&2
		exit 1
	fi
}

stamp_ok() {
	local file="$1" value="$2"
	[[ -f "$file" ]] && [[ "$(cat "$file")" == "$value" ]]
}

STAMP_KEY="${TARGET_OS}-${FFMPEG_ARCH:-native}-${HOST_TRIPLE:-native}-asm${DISABLE_ASM}"

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

build_x264() {
	local dir="$SRC/x264-${X264_COMMIT}-${STAMP_KEY}"
	if stamp_ok "$PREFIX/.x264.stamp" "$X264_COMMIT-$STAMP_KEY" && [[ -f "$PREFIX/lib/libx264.a" ]]; then
		echo "x264 already installed at $PREFIX"
		return 0
	fi
	rm -rf "$dir"
	mkdir -p "$dir"
	fetch_x264 "$dir"
	pushd "$dir" >/dev/null

	local cfg=(
		--prefix="$PREFIX"
		--enable-static
		--enable-pic
		--disable-cli
		--disable-opencl
	)
	if [[ "$CROSS" == "1" ]]; then
		cfg+=(--host="$HOST_TRIPLE" --cross-prefix=)
	fi
	if [[ "$DISABLE_ASM" == "1" ]]; then
		cfg+=(--disable-asm)
	fi
	if [[ -n "$LDFLAGS" ]]; then
		cfg+=(--extra-ldflags="$LDFLAGS")
	fi

	# x264 assembles non-x86 .S files with $CC but does not pass CFLAGS to
	# the assembler, so target flags (--target / -arch) must live in CC.
	local x264_cc="$CC"
	if [[ -n "$CFLAGS" ]]; then
		x264_cc="$CC $CFLAGS"
	fi

	CC="$x264_cc" AR="${AR:-ar}" RANLIB="${RANLIB:-ranlib}" STRIP="${STRIP:-strip}" ./configure "${cfg[@]}"
	make -j"$JOBS"
	make install
	popd >/dev/null
	echo "$X264_COMMIT-$STAMP_KEY" >"$PREFIX/.x264.stamp"
	cp -f "$dir/COPYING" "$ROOT/misc/video/licenses/x264-COPYING" 2>/dev/null || true
}

build_ffmpeg() {
	local tarball="$SRC/ffmpeg-${FFMPEG_VER}.tar.xz"
	local dir="$SRC/ffmpeg-${FFMPEG_VER}-${STAMP_KEY}"
	download_verify "$FFMPEG_URL" "$tarball" "$FFMPEG_SHA256"
	if stamp_ok "$PREFIX/.ffmpeg.stamp" "$FFMPEG_VER-$STAMP_KEY" && [[ -f "$PREFIX/lib/libavcodec.a" ]]; then
		echo "FFmpeg already installed at $PREFIX"
		return 0
	fi
	rm -rf "$dir"
	mkdir -p "$dir"
	tar -xf "$tarball" -C "$dir" --strip-components=1
	pushd "$dir" >/dev/null
	export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	# Cross builds: keep pkg-config from looking at the build machine's .pc files.
	if [[ "$CROSS" == "1" ]]; then
		export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
	fi

	local cfg=(
		--prefix="$PREFIX"
		--cc="$CC"
		--cxx="$CXX"
		--pkg-config=pkg-config
		--pkg-config-flags=--static
		--extra-cflags="-I$PREFIX/include $CFLAGS"
		--extra-ldflags="-L$PREFIX/lib $CFLAGS $LDFLAGS"
		--extra-libs="-lpthread -lm"
		--enable-static
		--disable-shared
		--enable-pic
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
	if [[ "$CROSS" == "1" ]]; then
		cfg+=(--enable-cross-compile --target-os="$TARGET_OS" --arch="$FFMPEG_ARCH")
		cfg+=(--ar="${AR:-ar}" --ranlib="${RANLIB:-ranlib}" --nm="${NM:-nm}" --strip="${STRIP:-strip}")
	fi
	if [[ "$DISABLE_ASM" == "1" ]]; then
		cfg+=(--disable-asm)
	fi
	# Big-endian PowerPC without VSX: swscale's fallback vec_xl() clashes with
	# clang's altivec.h builtin. x264 keeps its own AltiVec paths.
	case "${FFMPEG_ARCH:-}:${HOST_TRIPLE:-}" in
		ppc*:powerpc64le-* | ppc*:powerpcle-*) ;;
		ppc*:*) cfg+=(--disable-altivec) ;;
	esac

	./configure "${cfg[@]}" || {
		tail -n 60 ffbuild/config.log >&2
		exit 1
	}
	make -j"$JOBS"
	make install
	popd >/dev/null
	echo "$FFMPEG_VER-$STAMP_KEY" >"$PREFIX/.ffmpeg.stamp"
	cp -f "$dir/COPYING.GPLv2" "$ROOT/misc/video/licenses/ffmpeg-COPYING.GPLv2" 2>/dev/null || true
}

echo "=== Building static x264 + FFmpeg into $PREFIX ==="
build_x264
build_ffmpeg
echo "=== Done. Configure Omaha with: -DFFMPEG_ROOT=$PREFIX ==="
ls -l "$PREFIX/lib/libavcodec.a" "$PREFIX/lib/libavformat.a" "$PREFIX/lib/libswscale.a" \
	"$PREFIX/lib/libavutil.a" "$PREFIX/lib/libx264.a"
