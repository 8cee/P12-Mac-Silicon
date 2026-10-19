#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
trap 'result=$?; if [ "$result" -ne 0 ]; then echo "Build failed ($result). See output above."; fi; read -r -p "Press Return to close..."; exit "$result"' EXIT
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo 'Run this on an Apple Silicon Mac, using a native arm64 Terminal.' >&2
  exit 1
fi
command -v node >/dev/null || { echo 'Install Node.js 22 or newer first.'; exit 1; }
command -v cmake >/dev/null || { echo 'Install CMake 3.24 or newer first.'; exit 1; }
xcrun --find clang++ >/dev/null || { echo 'Install Xcode Command Line Tools with xcode-select --install.'; exit 1; }
npm ci
npm run dist:mac:arm64
bash scripts/verify-mac.sh
open dist-mac-arm64
