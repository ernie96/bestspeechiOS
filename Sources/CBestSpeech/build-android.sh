#!/bin/sh
# Build libunicorn plus the b32 harness for Android.
#
#   ./build-android.sh [ABI ...]        # default: arm64-v8a
#
# Produces, per ABI, c/build-android/<abi>/libb32tts.so -- the JNI library the
# TextToSpeechService loads. Run android/build-apk.sh afterwards to package it.
#
# Only the i386 guest target is compiled into unicorn, which is all this
# project needs and keeps the library roughly a quarter of its full size.
set -eu

SDK="${ANDROID_HOME:-C:/Users/Mew/scoop/apps/android-clt/current}"
NDK_VER="${NDK_VER:-27.0.12077973}"
NDK="$SDK/ndk/$NDK_VER"
CMAKE="$SDK/cmake/3.22.1/bin/cmake.exe"
NINJA="$SDK/cmake/3.22.1/bin/ninja.exe"
API="${API:-24}"

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
UC_SRC="$root/third_party/unicorn"
SHIMS="$root/third_party/shims"

[ -d "$NDK" ] || { echo "NDK not found at $NDK" >&2; exit 1; }
[ -d "$UC_SRC" ] || { echo "unicorn source missing; clone it into third_party/" >&2; exit 1; }

# qemu's configure hard-fails without a pkg-config binary and mis-detects
# endianness without `strings`; the shims satisfy both. See third_party/shims.
LLVM_BIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
[ -d "$LLVM_BIN" ] || LLVM_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
export NDK_LLVM_BIN="$LLVM_BIN"
export PATH="$SHIMS:$PATH"

CLANG="$LLVM_BIN/clang"
[ -x "$CLANG" ] || CLANG="$LLVM_BIN/clang.exe"

abi_triple() {
    case "$1" in
        arm64-v8a)   echo "aarch64-linux-android" ;;
        x86_64)      echo "x86_64-linux-android" ;;
        armeabi-v7a) echo "armv7a-linux-androideabi" ;;
        x86)         echo "i686-linux-android" ;;
        *) echo "unsupported ABI: $1" >&2; exit 1 ;;
    esac
}

for ABI in "${@:-arm64-v8a}"; do
    echo "=== $ABI ==="
    UC_BUILD="$UC_SRC/build-android-$ABI"

    if [ ! -f "$UC_BUILD/libunicorn.a" ]; then
        echo "--- configuring unicorn ---"
        "$CMAKE" -B "$UC_BUILD" -G Ninja \
            -DCMAKE_MAKE_PROGRAM="$NINJA" \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$ABI" \
            -DANDROID_PLATFORM="android-$API" \
            -DCMAKE_BUILD_TYPE=Release \
            -DUNICORN_ARCH=x86 \
            -DBUILD_SHARED_LIBS=OFF \
            -DUNICORN_BUILD_TESTS=OFF \
            -DUNICORN_INSTALL=OFF \
            -DUNICORN_LEGACY_STATIC_ARCHIVE=OFF \
            -S "$UC_SRC" >/dev/null
        echo "--- building unicorn ---"
        "$CMAKE" --build "$UC_BUILD" -j 8 >/dev/null
    else
        echo "unicorn already built"
    fi

    OUT="$here/build-android/$ABI"
    mkdir -p "$OUT"
    TARGET="$(abi_triple "$ABI")$API"

    # Link order matters: uc.c pulls the softmmu target, which pulls common.
    UCLIBS="$UC_BUILD/libunicorn.a $UC_BUILD/libx86_64-softmmu.a $UC_BUILD/libunicorn-common.a"
    CFLAGS="--target=$TARGET -O2 -fPIC -Wall -Wextra -std=c11 -I$UC_SRC/include"
    # 16 KB max page size: Android 15+ ships on devices with 16 KB pages, where a
    # library aligned only to 4 KB fails to load.
    LDFLAGS="-Wl,-z,max-page-size=16384"

    echo "--- libb32tts.so (JNI) ---"
    # shellcheck disable=SC2086
    "$CLANG" $CFLAGS $LDFLAGS -shared "$here/b32emu.c" "$here/b32jni.c" \
        "$here/sonic/sonic.c" $UCLIBS -lm -llog -o "$OUT/libb32tts.so"

    ls -la "$OUT"
done
