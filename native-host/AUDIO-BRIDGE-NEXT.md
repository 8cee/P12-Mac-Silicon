# P-12 PCM bridge: next native stage

The current P-12 sampler and built-in Mixer-32 DSP run in Chromium's Web Audio graph, while native VST plug-ins run inside `p12-vst-host.exe`.

For VST instruments, MIDI can be sent to the host and the host can render to the default audio device. For a VST compressor/reverb/EQ to process an existing P-12 Pad, Track, or Master signal, PCM blocks must cross the process boundary in real time.

## Recommended architecture

1. Add a small N-API native Electron addon (`p12-audio-bridge.node`) that creates named shared-memory ring buffers.
2. Expose a SharedArrayBuffer-compatible control/data surface to a dedicated AudioWorklet in P-12.
3. AudioWorklet writes fixed-size interleaved/planar blocks to an input ring and reads processed blocks from an output ring without renderer-thread IPC.
4. `p12-vst-host.exe` maps the same shared memory, processes blocks through the exact Mixer-32 insert instance, and returns them through the output ring.
5. Use sequence counters + underrun flags, never JSON, Electron IPC, WebSocket, or postMessage for the audio samples themselves.
6. Keep JSON-lines IPC only for control: load/unload, MIDI, editor, parameters, state, routing, diagnostics.

## Why it is separate

Electron IPC and JSON are fine for UI and MIDI control, but they are not deterministic enough for 128/256-sample real-time audio blocks. Keeping PCM on shared memory avoids unnecessary copies and renderer scheduling jitter.

## Proposed ring header

- magic/version
- sampleRate
- blockFrames
- channels
- writeSequence
- readSequence
- underrunCount
- overrunCount
- bypass flag
- shutdown flag

Each Mixer-32 native insert should get its own pair of rings, or use a multiplexed ring with fixed route IDs if the maximum number of simultaneous native inserts is bounded.
