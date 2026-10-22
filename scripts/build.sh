#!/usr/bin/env bash
set -euo pipefail
NDK="${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
[ -d "$NDK" ] || { echo "NDK tidak ditemukan"; exit 1; }
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -d "$SDK" ] || { echo "Android SDK tidak ditemukan"; exit 1; }
API=24
BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$BIN/aarch64-linux-android${API}-clang"
ROOT="$PWD"; WORK="$ROOT/build"; rm -rf "$WORK"
mkdir -p "$WORK/classes" "$WORK/dex" "$WORK/gen" dist

# --- Java -> dex -> header ---
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
PLAT="$(ls -d "$SDK"/platforms/android-[0-9]* | sort -V | tail -1)"
[ -x "$BT/d8" ] || { echo "d8 tidak ada di $BT"; exit 1; }
javac --release 8 -encoding UTF-8 -Xlint:-options -cp "$PLAT/android.jar" \
  -d "$WORK/classes" $(find java -name '*.java')
"$BT/d8" --release --min-api 24 --lib "$PLAT/android.jar" \
  --output "$WORK/dex" $(find "$WORK/classes" -name '*.class')
python3 - "$WORK/dex/classes.dex" "$WORK/gen/dex_data.h" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('static const unsigned char kDex[] __attribute__((aligned(8))) = {')
    f.write(','.join(str(b) for b in d))
    f.write('};\nstatic const unsigned int kDexLen = %d;\n' % len(d))
PY

# --- AML headers ---
git clone --depth 1 https://github.com/RusJJ/AndroidModLoader "$WORK/aml"
AMLH="$(find "$WORK/aml" -name amlmod.h | head -1)"
[ -n "$AMLH" ] || { echo "amlmod.h tidak ketemu"; exit 1; }
AMLDIR="$(dirname "$AMLH")"

# --- LuaJIT ---
git clone --depth 1 -b v2.1 https://github.com/LuaJIT/LuaJIT "$WORK/luajit"
make -C "$WORK/luajit" -j"$(nproc)" amalg BUILDMODE=static \
  HOST_CC="gcc" TARGET_SYS=Linux \
  STATIC_CC="$CC -fPIC" DYNAMIC_CC="$CC -fPIC" TARGET_LD="$CC" \
  TARGET_AR="$BIN/llvm-ar rcus" TARGET_STRIP="$BIN/llvm-strip"

# --- Mod ---
cmake -S "$ROOT" -B "$WORK/out" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DAML_INC="$AMLDIR" -DLUAJIT_DIR="$WORK/luajit/src" -DGEN_DIR="$WORK/gen"
cmake --build "$WORK/out" -j"$(nproc)"
cp "$WORK/out/libluaaml.so" dist/
ls -l dist
