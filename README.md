# Cutbench: cue-pad sampler (VST3 / AU, JUCE)

Load a long audio file, set cue points on 16 pads (live while it plays), and play them from MIDI.
Status: Phase 1, the pad/live-chop part of Phase 2, and Phase 3 (pitch, speed, reverse, loop, envelope,
region end handle). Auto-slicing, key/BPM detection and export are still to come (see the end).

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
- Every update that changes the plugin's identity (the `PLUGIN_CODE` in CMakeLists.txt, currently `Cb03`) shows up in Ableton as a *new* plugin,
  which is useful when an old copy keeps getting loaded. The top bar shows the running version ("Cutbench v0.3.0 ...").
- Ableton Live: Preferences > Plug-ins > enable Audio Units / VST3 > Rescan. Put Cutbench on a MIDI track.
- JUCE is dual-licensed (AGPLv3 / commercial). Check which applies before distributing.

- The time-stretch engine (Signalsmith Stretch + Signalsmith Linear, fetched from GitHub at configure time) is built
  with `-DCUTBENCH_STRETCH=ON`. The GitHub workflow tries (1) with Apple Accelerate FFTs, (2) without Accelerate,
  (3) without the engine, and writes which one worked into `BUILD-INFO.txt` inside the downloaded zip (plus a warning
  in the run summary). The plugin shows its version and "Keep Length: on/off" in the top bar.

## How it works

**Pads = cue points.** Pad *n* is played by MIDI note `Base Note + n - 1` (default base note 36 = C1, set in the settings menu behind the gear button; pad 1
at the bottom-left of the grid like a Drum Rack). A fresh load puts pad 1 at the start of the file.

**Live chop (the main workflow)**

1. Start the sample (Play button / Space, or play pad 1 from the keyboard).
2. While it runs, press an **empty** pad (MIDI key or click). The pad records the current play position as its
   cue point. Playback is NOT interrupted.
3. Press a pad that **has** a cue: playback jumps there and plays from that point. Nothing new is created.
   To move a cue, drag its marker in the waveform; to remove it, right-click the pad (or Delete).

**Chop length**: a pad plays from its cue to the *next cue* (the last cue runs to the end of the file).
Pads set with Set Pad / Chop Equal play exactly their region.

**Voices**
- *Mono* (default): pressing a pad cuts every other sounding pad.
- *Poly*: different pads may sound together; an earlier hit of the same pad is cut.

**Play Mode**: *One-Shot* ignores key release, *Gate* stops when the key is released.

Regions: drag in the waveform to select, then **Set Pad** assigns the exact region to the selected pad (plays only
that part, in both modes). **Chop Equal** divides the selected region (or the whole sample when nothing is selected) into N equal regions on pads 1..N.

## Pitch, speed and playback (Phase 3)

| Control | What it does |
|---|---|
| Pitch / Fine Tune | Pitch: -24..+24 semitones, shown as a plain number with [-] / [+] buttons (hold to repeat); the number can also be dragged. Fine Tune: +/-100 cents. Double-click Pitch, Fine Tune, Speed or any knob to reset it. |
| Speed | 0.25x..4x playback speed |
| Engine: Vinyl | Pitch and speed are linked, like tape or a turntable (speed 2x = one octave up). Cubic-interpolated resampling, cheap, no added latency. Best for tight live chopping. |
| Engine: Keep Length | Pitch and speed are independent (Signalsmith Stretch). Pitch +5 keeps the duration, speed 0.5x keeps the pitch. Starts right on the cue point (the engine is primed with the first frames), but uses more CPU; at most 8 voices use it, further voices fall back to Vinyl. |
| Reverse | Plays each region backwards (applies to hits started after switching) |
| Loop | Repeats the region while the pad sounds (Gate: until key release; One-Shot: until another pad / Stop) |
| Attack / Release (knobs) | Fade-in on trigger / fade-out on key release (Gate) and at the end of a region. 0 = just the declick fade. The value shows while you drag or hover. |
| Declick Fade (settings menu) | Short fade (0.5-20 ms) used for cuts, region ends and loop seams |
| End handle | Select a pad, then drag the grip at the bottom edge of its region to set where it stops. Double-click it to go back to "until the next cue" |

Pitch, speed, Attack, Release and Fade are automatable in Live. Reverse and Loop are read when a pad is triggered.

| Action | How |
|---|---|
| Load audio | Load... button, or drop a file on the window |
| Set play start | Click the waveform (cursor) |
| Play / stop | The round button in the top centre (plays the selection, or from the cursor to the end; shows a stop square while playing), or Space |
| Set cue at a position | Double-click it (assigns the selected pad, then selects the next empty pad) |
| Move a cue | Drag its marker line |
| Play a pad | Click the pad or its flag in the top strip of the waveform |
| Clear a pad | Right-click it, Clear Pad, or Delete key; Clear All clears everything |
| Zoom / scroll | Cmd/Ctrl + scroll, pinch, +/-/Fit; scroll or swipe to pan |

Every chop is tinted in its pad colour in the waveform (the selected one brighter). The Gain knob sits in the top right corner.

The project saves parameters, the audio file *path* and the pad table. If the file has moved, Cutbench keeps the
pad data and shows a "not found" message.

## Real-time design

- Audio thread: no locks, no allocation, no file I/O. The pad table is a set of atomics that both the audio
  thread (live chop) and the UI write; a version counter tells the UI to refresh.
- MIDI is applied sample-accurately, so a live-chop cue lands on the exact sample of the key press.
- Every voice fades in/out (parameter "Declick Fade"), including the last samples of a region.
- File decoding and peak analysis run on a worker thread; replaced samples stay alive ~6 s before being freed.
- Bad input: unreadable, empty or oversized (> ~1 GiB) files give a status message and leave the current sample alone.
- Vinyl engine: cubic interpolation (also converts file rate to host rate). Large pitch-ups can alias a little;
  use Keep Length for clean, big shifts.
- Keep Length: the library is configured and its memory allocated in prepareToPlay; the audio thread only calls
  outputSeek()/process(). The live-chop position and the playhead display are corrected for the engine's lag.

## Still to come

- Per-pad mute/solo/colour, editable MIDI note per pad, per-pad end handle
- Per-pad pitch / speed / reverse (Phase 3 controls are global for now)
- Auto-slicing: transient / beat / bar modes; snap-to-transient for live chop
- Keyboard mode, key detection, BPM detection and host-tempo sync, slice export, presets

## Manual test checklist

1. Load a long WAV: waveform appears; load a corrupt file and an empty file: message, no crash.
2. Press C1: plays from the start. While playing, press D1, E1 ...: cue flags appear at those moments, audio keeps running.
3. Press D1 again: jumps to its cue and plays until the next cue, then stops. Switch Voices to Poly and compare.
4. Drag a marker, double-click to place one, right-click a pad to clear it, Chop Equal with 8.
5. Gate vs One-Shot; hit pads quickly; load a new file while notes play (no crash, no noise).
6. Save the Live set, close, reopen: file and cues restored; rename the file and reopen: message, no crash.
7. Automate Gain; change sample rate (44.1/48/96 kHz): pitch/speed must stay correct.
8. Vinyl: Pitch +12 plays an octave higher and twice as fast; Speed 2x does the same. Double-click the pitch number to return to 0.
9. Keep Length (if available): Pitch +7 keeps the length, Speed 0.5x keeps the pitch. Check that nothing clicks
   when you move the sliders while a pad plays, and that a hit starts right on the cue point.
10. Reverse a pad; Loop with Gate (hold = loops, release = stops); Attack 200 ms and Release 500 ms on a pad.
11. Drag the end handle of a pad, double-click it to reset; chop with Poly voices and listen for clicks at the ends.
