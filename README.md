# Cutbench: sample slicer (VST3, JUCE) – Phase 1

Original sampler plugin for chopping long audio files into slices and playing them from MIDI.
This is **Phase 1** of the roadmap: loading, waveform, manual slicing, playback, state recall.
It was written without access to a compiler or JUCE, so **it has not been built or run yet**.
Expect a few small compile fixes on the first build.

## Build (macOS)

Requirements: Xcode command line tools, CMake >= 3.22, git (JUCE is fetched automatically).

```bash
cmake -B build -G Xcode            # or: -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

- The VST3 is copied to `~/Library/Audio/Plug-Ins/VST3/Cutbench.vst3`, the AU to `~/Library/Audio/Plug-Ins/Components/Cutbench.component`.
- No Mac toolchain? Push this folder to a GitHub repo and run the "Build macOS plugins" workflow (Actions tab). It produces a downloadable `Cutbench-macOS.zip` with both formats. Builds downloaded from the internet are unsigned: run `xattr -dr com.apple.quarantine <file>` on each plugin before use.
- A standalone app is built too (`build/Cutbench_artefacts/Release/Standalone/`), handy for quick tests.
- To use your own JUCE checkout: `-DCUTBENCH_JUCE_DIR=/path/to/JUCE`.
- Universal binary (arm64 + x86_64), deployment target macOS 11.
- Ableton Live: Preferences > Plug-ins > enable "Use VST3 System Folders" > Rescan. Cutbench is an *instrument*:
  put it on a MIDI track.
- JUCE is dual-licensed (AGPLv3 / commercial). Check which applies before distributing.

## Using it

| Action | How |
|---|---|
| Load audio | Load... button, or drop a file on the window (WAV, AIFF, FLAC, OGG, plus MP3/AAC via CoreAudio) |
| Seek / set play start | Click the waveform (sets the cursor and selects the slice under it) |
| Select a region | Drag |
| Add slice from selection | Add Slice |
| Chop evenly | Set the slice count, press Chop Equal (replaces all slices) |
| Split a slice | Double-click, or Split (splits at the cursor) |
| Adjust boundaries | Drag a slice edge (touching slices move together) |
| Audition a slice | Click its numbered strip at the top |
| Play / stop | Play (selection, or cursor to end) / Stop, Space |
| Delete slice | Delete button or Delete key |
| Zoom / scroll | Cmd/Ctrl + scroll, pinch, +/-/Fit; scroll or swipe to pan, scroll bar below |

**MIDI:** note `Base Note + n` plays slice `n+1`, numbered left to right (default base note 36 = C1, as in a Drum Rack).
Velocity scales gain. *Retrigger* = Cut fades out the previous hit of the same key; Layer stacks hits. *One-Shot* plays the whole slice; *Gate* releases on note-off. Voices overlap (32 max).
Parameters (gain, base note, play mode, fade) are host-automatable. The project saves the slice list and the
audio file *path*; if the file has moved, Cutbench keeps the slice data and shows a "not found" message.

## Architecture / real-time rules

- **Audio thread:** no locks, no allocation, no file I/O. Reads an atomic pointer to immutable `SampleData`,
  an atomic slice table, a lock-free command FIFO (UI previews) and APVTS atomics. Slice values are re-validated
  on every use because the UI can update them mid-block.
- **Loading:** decode + peak cache on a worker thread, handed to the message thread, published with one atomic store.
  Replaced samples are kept alive ~6 s so the audio thread never reads freed memory.
- **Declicking:** attack, release and the last N samples of every slice are faded (parameter "Fade").
- **Sample rates:** playback converts file rate to host rate with linear interpolation. Good enough for Phase 1;
  Phase 3 replaces this with proper resampling / pitch / time-stretch.
- **Bad input:** unreadable, empty or oversized (> ~1 GiB in memory) files produce a status message and leave the
  current sample untouched. Corrupt plugin state is ignored.

## Not in Phase 1 (planned)

2. 4x4 pad grid, per-slice mute/solo/colour, editable note mapping
3. Pitch (+/-24 st, cents), reverse, start/end, loop, attack/release, time-stretch (library such as Signalsmith Stretch or Rubber Band)
4. Automatic slicing: transient / beat / bar / equal modes
5. Keyboard mode (root note + range), key detection, BPM detection and host-tempo sync
6. Slice export (files, drag-and-drop), presets

## Manual test checklist

1. Load a long WAV: waveform appears, whole file fits the view; load a corrupt file and a 0-byte file: message, no crash.
2. Zoom in/out, pan, click to seek, drag a selection, split by double-click, drag edges, delete, reset.
3. Play from MIDI keys (C1 upward), hold overlapping notes, retrigger quickly, switch One-Shot/Gate.
4. Load a new file while notes are playing (no crash, no noise).
5. Save the Live set, close and reopen: file and slices restored; rename the file and reopen: message, no crash.
6. Automate Gain in Live; change sample rate (44.1/48/96 kHz) and confirm pitch/speed stay correct.
7. Resize the window; open several instances.
