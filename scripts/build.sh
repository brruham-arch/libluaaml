#!/usr/bin/env bash
set -euo pipefail
NDK="${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
[ -d "$NDK" ] || { echo "NDK tidak ditemukan"; exit 1; }
API=24
BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$BIN/aarch64-linux-android${API}-clang"
ROOT="$PWD"; WORK="$ROOT/build"; rm -rf "$WORK"; mkdir -p "$WORK" dist

git clone --depth 1 https://github.com/RusJJ/AndroidModLoader "$WORK/aml"
AMLH="$(find "$WORK/aml" -name amlmod.h | head -1)"
[ -n "$AMLH" ] || { echo "amlmod.h tidak ketemu"; exit 1; }
AMLDIR="$(dirname "$AMLH")"

git clone --depth 1 -b v2.1 https://github.com/LuaJIT/LuaJIT "$WORK/luajit"
make -C "$WORK/luajit" -j"$(nproc)" amalg BUILDMODE=static \
  HOST_CC="gcc" TARGET_SYS=Linux \
  STATIC_CC="$CC -fPIC" DYNAMIC_CC="$CC -fPIC" TARGET_LD="$CC" \
  TARGET_AR="$BIN/llvm-ar rcus" TARGET_STRIP="$BIN/llvm-strip"

cmake -S "$ROOT" -B "$WORK/out" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DAML_INC="$AMLDIR" -DLUAJIT_DIR="$WORK/luajit/src"
cmake --build "$WORK/out" -j"$(nproc)"
cp "$WORK/out/libluaaml.so" dist/
ls -l dist
