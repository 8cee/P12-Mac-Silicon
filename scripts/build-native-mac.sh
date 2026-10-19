#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo 'Native host requires an Apple Silicon Mac.' >&2; exit 1; }
command -v cmake >/dev/null || { echo 'Install CMake 3.24 or newer first.' >&2; exit 1; }
xcrun --find clang++ >/dev/null
out=native-host/build/mac-arm64
cmake -S native-host -B "$out" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DP12_ENABLE_VST2=OFF -DP12_BUILD_VST3_FIXTURE=ON
cmake --build "$out" --config Release --parallel 3
ctest --test-dir "$out" -C Release --output-on-failure
host="$out/bin/P-12 VST Host.app"
test -f "$host/Contents/MacOS/P-12 VST Host"
mkdir -p native
rm -rf 'native/P-12 VST Host.app'
ditto "$host" 'native/P-12 VST Host.app'
codesign --force --deep --sign - 'native/P-12 VST Host.app'
lipo 'native/P-12 VST Host.app/Contents/MacOS/P-12 VST Host' -verify_arch arm64
fixture="$out/p12-test-gain_artefacts/Release/VST3/P12 Test Gain.vst3"
test -d "$fixture"
node scripts/test-vst3-host.cjs 'native/P-12 VST Host.app/Contents/MacOS/P-12 VST Host' "$fixture"
