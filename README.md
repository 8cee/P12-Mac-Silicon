# P-12 FULL15 — Mac Silicon with native VST3

Apple Silicon adaptation of `P12_FULL15_WINDOWS_MAX_BUILD_FIX_2026-10-11(2).zip`.
The original FULL15 interface and P-12.html are preserved byte-for-byte, including
Rossum .sp12/HFE conversion. Includes detached Mixer 32, playlist and piano windows,
.p12 saving/recovery, original icon, and the uploaded JUCE native sampler/mixer.

## Native plugins

The Mac build compiles the native host for arm64 and bundles it inside the app.
VST3 effects and instruments use the existing pad, sub/track (including track 0),
master and standalone-slot routing, MIDI, plugin state and native editor support.
CoreAudio is used on macOS. Install Apple Silicon/Universal VST3 plugins in:

- /Library/Audio/Plug-Ins/VST3
- ~/Library/Audio/Plug-Ins/VST3

Intel-only plugins and Windows DLLs cannot run in the arm64 host. VST2 is optional
and requires a separately supplied legacy SDK; the provided build enables VST3.
Audio Units are not enabled by this build.

Mac plugin resources stay in their installed bundles. The host runs directly
from its signed app bundle; Windows DLL/ICU staging is excluded on macOS.

## Build

On an Apple Silicon Mac, install Node.js 22+, CMake 3.24+, Git and Xcode Command
Line Tools (`xcode-select --install`). Double-click BUILD_MAC.command, or run:

```bash
bash BUILD_MAC.command
```

The script builds the arm64 JUCE host, runs native tests, builds a local test
VST3 and checks real PCM processing/editor opening/bypass on pad/track-0/master
inserts, then packages a DMG and application ZIP. The fixture plugin is not
installed or included in the app. JUCE is fetched at the uploaded source's 9.0.2 tag.

Outputs: dist-mac-arm64/. Packaging verification checks arm64 binaries, signatures,
main/preload/detached-window startup, native VST3 host status and DMG integrity.
The app is ad-hoc signed, not Developer ID signed or notarized; first launch may
need approval in macOS Privacy & Security.

## Validation and remaining Mac check

Passed in this Linux workspace: JavaScript/package checks, Mac native-host launch
simulation, uploaded HTML hash, detached-window IPC, recovery, transport, saves,
MIDI, HFE layout/Rossum hash, and seven compiled native DSP test executables
(sampler, scheduling, SP character, pad controls, inserts, SSL/spectrum, live/resources).

A macOS build and actual Mac VST3 loading have not been executed here. The Mac
build script enforces those checks before delivering its DMG. Commercial plugins
such as Ozone still need validation on a Mac with that plugin installed/licensed.
This source package is not a prebuilt DMG. GitHub commits use [skip ci]; Actions
can be run manually when available.
