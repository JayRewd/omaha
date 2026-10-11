#!/usr/bin/env bash
# Build universal (arm64 + x86_64) static FFmpeg + x264 for Project: Omaha on macOS.
# Each slice is built with build-ffmpeg-x264.sh, then merged with lipo.
# Output: $FFMPEG_PREFIX/{include,lib}
#
# Environment:
#   FFMPEG_PREFIX             install prefix (default: build-deps/ffmpeg)
#   FFMPEG_ARCHS              slices to build (default: "arm64 x86_64")
#   MACOSX_DEPLOYMENT_TARGET  minimum macOS version (default: 10.15)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${FFMPEG_PREFIX:-$ROOT/build-deps/ffmpeg}"
WORK="${FFMPEG_SLICES:-$PREFIX-slices}"
ARCHS="${FFMPEG_ARCHS:-arm64 x86_64}"
MIN_OS="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
LIBS=(libavformat libavcodec libswscale libavutil libx264)

if [[ "$(uname -s)" != "Darwin" ]]; then
	echo "ERROR: build-ffmpeg-x264-universal.sh must run on macOS" >&2
	exit 1
fi

first=""
for arch in $ARCHS; do
	case "$arch" in
		arm64) ff_arch=aarch64; triple=aarch64-apple-darwin ;;
		x86_64) ff_arch=x86_64; triple=x86_64-apple-darwin ;;
		*) echo "ERROR: unsupported macOS arch '$arch'" >&2; exit 1 ;;
	esac
	echo "=== macOS slice: $arch ==="
	env -u CFLAGS -u LDFLAGS \
		CC=clang CXX=clang++ \
		CFLAGS="-arch $arch -mmacosx-version-min=$MIN_OS" \
		LDFLAGS="-arch $arch -mmacosx-version-min=$MIN_OS" \
		FFMPEG_PREFIX="$WORK/$arch" \
		FFMPEG_SRC="${FFMPEG_SRC:-$ROOT/build-deps/ffmpeg-src}" \
		FFMPEG_HOST_TRIPLE="$triple" \
		FFMPEG_ARCH="$ff_arch" \
		FFMPEG_TARGET_OS=darwin \
		bash "$ROOT/misc/video/build-ffmpeg-x264.sh"
	[[ -z "$first" ]] && first="$arch"
done

rm -rf "$PREFIX/include" "$PREFIX/lib"
mkdir -p "$PREFIX/lib"
# Headers are identical across slices except avconfig.h / x264_config.h, which
# only carry endianness/bit-depth values that match for arm64 and x86_64.
cp -R "$WORK/$first/include" "$PREFIX/include"

for lib in "${LIBS[@]}"; do
	inputs=()
	for arch in $ARCHS; do
		inputs+=("$WORK/$arch/lib/$lib.a")
	done
	lipo -create "${inputs[@]}" -output "$PREFIX/lib/$lib.a"
	lipo -info "$PREFIX/lib/$lib.a"
done

echo "=== Done. Configure Omaha with: -DFFMPEG_ROOT=$PREFIX ==="
