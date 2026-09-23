#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDL2_VERSION="${SDL2_VERSION:-2.32.10}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-28}"
PREFIX_ROOT="${ANDROID_PREFIX_ROOT:-$ROOT/.android-prefixes}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

: "${ANDROID_HOME:=$HOME/Library/Android/sdk}"
: "${ANDROID_NDK_HOME:=${ANDROID_HOME}/ndk/27.1.12297006}"

SDL2_PREFIX="$PREFIX_ROOT/SDL2-${SDL2_VERSION}-android-arm64-vulkanfix1"
WORK="${RUNNER_TEMP:-/tmp}/rs64-android-deps"
CMAKE_BIN="${ANDROID_HOME}/cmake/3.22.1/bin/cmake"
[[ -x "$CMAKE_BIN" ]] || CMAKE_BIN="$(command -v cmake)"
[[ -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" ]] || {
    echo "Android NDK not found at $ANDROID_NDK_HOME" >&2
    exit 2
}
mkdir -p "$PREFIX_ROOT" "$WORK"
cd "$WORK"

fetch() { [[ -f "$2" ]] || curl -fsSL --retry 3 --retry-delay 3 -o "$2" "$1"; }

if [[ ! -f "$SDL2_PREFIX/lib/libSDL2.so" ]]; then
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz" "SDL2-${SDL2_VERSION}.tar.gz"
    rm -rf "SDL2-${SDL2_VERSION}" build-sdl2
    tar -xzf "SDL2-${SDL2_VERSION}.tar.gz"

    # SDL 2.32.10 otherwise creates an EGL surface for a Vulkan window after
    # surface recreation, stealing the ANativeWindow from Vulkan.
    SDL_ANDROID_C="SDL2-${SDL2_VERSION}/src/core/android/SDL_android.c"
    python3 - "$SDL_ANDROID_C" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
old = "        if (data->egl_surface == EGL_NO_SURFACE) {"
new = "        if ((Android_Window->flags & SDL_WINDOW_OPENGL) && data->egl_surface == EGL_NO_SURFACE) {"
if s.count(old) != 1:
    raise SystemExit(f"SDL EGL guard anchor count: {s.count(old)}")
p.write_text(s.replace(old, new, 1))
PY

    "$CMAKE_BIN" -S "SDL2-${SDL2_VERSION}" -B build-sdl2 -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-${ANDROID_PLATFORM}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL2_PREFIX" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
    "$CMAKE_BIN" --build build-sdl2 --parallel "$JOBS"
    "$CMAKE_BIN" --install build-sdl2
fi

mkdir -p "$ROOT/android/app/src/main/java/org/libsdl/app"
cp -f "SDL2-${SDL2_VERSION}/android-project/app/src/main/java/org/libsdl/app/"*.java \
    "$ROOT/android/app/src/main/java/org/libsdl/app/"

printf 'RS64_ANDROID_SDL2_PREFIX=%s\n' "$SDL2_PREFIX"
printf 'ANDROID_NDK_HOME=%s\n' "$ANDROID_NDK_HOME"
