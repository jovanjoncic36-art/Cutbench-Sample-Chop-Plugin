# Cutbench: cue-pad sampler (VST3 / AU, JUCE)

Load a long audio file, set cue points on 16 pads (live while it plays), and play them from MIDI.
Status: Phase 1 + the pad/live-chop part of Phase 2. Pitch, time-stretch, auto-slicing, key/BPM detection
and export are still to come (see the end).

## Build (macOS)

Requirements: Xcode command line tools, CMake >= 3.22, git (JUCE is fetched automatically).

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

- VST3 goes to `~/Library/Audio/Plug-Ins/VST3/`, AU to `~/Library/Audio/Plug-Ins/Components/`.
- No Mac toolchain? Push this folder to GitHub and run the "Build macOS plugins" workflow (Actions tab).
  It produces `Cutbench-macOS.zip`. Downloaded builds are unsigned: run
  `xattr -dr com.apple.quarantine <file>` on each plugin first.
- Ableton Live: Preferences > Plug-ins > enable Audio Units / VST3 > Rescan. Put Cutbench on a MIDI track.
- JUCE is dual-licensed (AGPLv3 / commercial). Check which applies before distributing.

## How it works

**Pads = cue points.** Pad *n* is played by MIDI note `Base Note + n - 1` (default base note 36 = C1, pad 1
at the bottom-left of the grid like a Drum Rack). A fresh load puts pad 1 at the start of the file.

**Live chop (the main workflow)**

1. Start the sample (Play button / Space, or play pad 1 from the keyboard).
2. While it runs, press an **empty** pad (MIDI key or click). The pad records the current play position as its
   cue point. Playback is NOT interrupted.
3. Press a pad that **has** a cue: playback jumps there and plays from that point. Nothing new is created.
   To move a cue, drag its marker in the waveform; to remove it, right-click the pad (or Delete).

**Pad Mode**
- *Cue* (default): a pad plays from its cue to the end of the file. Pressing another pad cuts the previous one.
- *Slice*: a pad plays from its cue to the next cue; different pads may overlap, an earlier hit of the same pad is cut.

**Play Mode**: *One-Shot* ignores key release, *Gate* stops when the key is released.

Regions: drag in the waveform to select, then **Set Pad** assigns the exact region to the selected pad (plays only
that part, in both modes). **Chop Equal** spreads N equal regions over pads 1..N.

| Action | How |
|---|---|
| Load audio | Load... button, or drop a file on the window |
| Set play start | Click the waveform (cursor) |
| Play / stop | Play (selection, or cursor to end) / Stop / Space |
| Set cue at a position | Double-click it (assigns the selected pad, then selects the next empty pad) |
| Move a cue | Drag its marker line |
| Play a pad | Click the pad or its flag in the top strip of the waveform |
| Clear a pad | Right-click it, Clear Pad, or Delete key; Clear All clears everything |
| Zoom / scroll | Cmd/Ctrl + scroll, pinch, +/-/Fit; scroll or swipe to pan |

The project saves parameters, the audio file *path* and the pad table. If the file has moved, Cutbench keeps the
pad data and shows a "not found" message.

## Real-time design

- Audio thread: no locks, no allocation, no file I/O. The pad table is a set of atomics that both the audio
  thread (live chop) and the UI write; a version counter tells the UI to refresh.
- MIDI is applied sample-accurately, so a live-chop cue lands on the exact sample of the key press.
- Every voice fades in/out (parameter "Declick Fade"), including the last samples of a region.
- File decoding and peak analysis run on a worker thread; replaced samples stay alive ~6 s before being freed.
- Bad input: unreadable, empty or oversized (> ~1 GiB) files give a status message and leave the current sample alone.
- Linear interpolation converts file rate to host rate; real pitch/time-stretch comes in Phase 3.

## Still to come

- Per-pad mute/solo/colour, editable MIDI note per pad, per-pad end handle
- Pitch (+/-24 st, cents), reverse, loop, attack/release, time-stretch (Signalsmith Stretch or Rubber Band)
- Auto-slicing: transient / beat / bar modes; snap-to-transient for live chop
- Keyboard mode, key detection, BPM detection and host-tempo sync, slice export, presets

## Manual test checklist

1. Load a long WAV: waveform appears; load a corrupt file and an empty file: message, no crash.
2. Press C1: plays from the start. While playing, press D1, E1 ...: cue flags appear at those moments, audio keeps running.
3. Press D1 again: jumps to its cue and plays from there. Switch Pad Mode to Slice and compare.
4. Drag a marker, double-click to place one, right-click a pad to clear it, Chop Equal with 8.
5. Gate vs One-Shot; hit pads quickly; load a new file while notes play (no crash, no noise).
6. Save the Live set, close, reopen: file and cues restored; rename the file and reopen: message, no crash.
7. Automate Gain; change sample rate (44.1/48/96 kHz): pitch/speed must stay correct.
