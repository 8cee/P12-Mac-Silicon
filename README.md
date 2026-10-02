# P-12 Mac Silicon

Apple Silicon (arm64) macOS build of P-12 v1.0.

This build intentionally omits the native VST host. The P-12 sampler/sequencer, Tune BF, SP12/HFE export, swing, Time Correct, sample handling and other HTML application features remain in the same P-12.html source.

## Build

GitHub Actions builds a macOS arm64 DMG automatically when files are pushed to main.

Required application source at repository root:

`P-12.html`

The finished artifact is uploaded as **P-12-Mac-Silicon-DMG**.
