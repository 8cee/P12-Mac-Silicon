# P-12 native host

This is the JUCE host from the uploaded October 11 FULL15 package, adapted for
Apple Silicon. Run `bash scripts/build-native-mac.sh` from the repository root.
The output is `native/P-12 VST Host.app`; Electron packages and launches its
executable in place with JSON-lines stdin/stdout IPC.

The native sampler owns pad/track/master audio scheduling and processing.
VST3 inserts run in that audio graph; manufacturer editors use the JUCE message
thread. Windows resource staging is compiled out on macOS to preserve signed
host/plugin bundles. Existing Windows native code is retained under its guards.

CMake's P12_BUILD_VST3_FIXTURE option builds a private gain plugin for integration
checks. CTest runs the standalone DSP regressions with assertions enabled in Release.
See the root README for supported plugin architectures and validation limitations.

JUCE and VST SDK licensing requirements continue to apply to distribution.
