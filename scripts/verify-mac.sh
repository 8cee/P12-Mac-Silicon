#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
app='dist-mac-arm64/mac-arm64/P-12 v1.0.app'
exe="$app/Contents/MacOS/P-12 v1.0"
test -f "$exe"
lipo "$exe" -verify_arch arm64
lipo "$app/Contents/Resources/native/P-12 VST Host.app/Contents/MacOS/P-12 VST Host" -verify_arch arm64
codesign --verify --deep --strict --verbose=2 "$app"
"$exe" --p12-smoke-test
for dmg in dist-mac-arm64/*.dmg; do hdiutil verify "$dmg"; done
(cd dist-mac-arm64 && shasum -a 256 ./*.dmg ./*.zip > SHA256SUMS.txt)
