# LXR as CLAP and VST3 plugins

The LXR V2 drum machine's sound engine and sequencer as an instrument plugin for
Linux, macOS and Windows, in CLAP and VST3 format. It is the same port as the
[RNBO version for Ableton Move](../rnbo/README.md), compiled to C++: the codebox sources
in `rnbo/src` stay the one place the port is written, and a translator turns them into
the C++ in `native/generated`. The native build is checked against the firmware with the
same tests as the RNBO version.

What the plugin has:

- The seven LXR voices (Drum 1-3, Snare, Cymbal, Closed and Open Hihat) and the FX bus,
  played by MIDI notes.
- All 227 LXR sound parameters as automatable plugin parameters, grouped by voice.
- The LXR sequencer: 16 patterns of 7 tracks with up to 64 steps, per-step velocity,
  probability, note, flam, micro timing and two parameter locks, shuffle, Euclid
  generator, mutes and recording. It follows the host's transport, or runs on its own
  clock.
- Two stereo outputs, Main and Aux, as on the hardware: each voice's **Out** parameter
  sends it to Main, Aux, one side of either, or the FX bus.
- The sequencer's notes on a MIDI output, if you want them for other instruments.
- Everything (sound, patterns, settings) saved with the host's project.

There is no plugin window yet. Hosts show their generic parameter editor instead.

## Building

You need Git, CMake 3.21 or newer and a C++17 compiler (GCC, Clang, Xcode or Visual
Studio).

```
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --config Release --parallel
```

The plugins end up in `native/build/plugins/` (on Windows in its `CLAP` and `VST3`
folders): `LXR.clap` and `LXR.vst3`. On macOS they are universal (Apple Silicon and
Intel) and not signed; if macOS refuses to load a downloaded copy, clear its quarantine
flag with `xattr -dr com.apple.quarantine LXR.clap LXR.vst3`.

Configuring downloads [clap-wrapper](https://github.com/free-audio/clap-wrapper) 0.17.0,
the [CLAP](https://github.com/free-audio/clap) 1.2.10 headers and the
[VST3 SDK](https://github.com/steinbergmedia/vst3sdk) 3.8.0. To use checkouts you
already have, pass `-DCLAP_WRAPPER_ROOT=`, `-DCLAP_SDK_ROOT=` and `-DVST3_SDK_ROOT=`.
`-DLXR_PLUGIN_FORMATS="CLAP;VST3;AUV2"` adds an Audio Unit on macOS (untested).

Copy the plugins where your host looks for them:

| | CLAP | VST3 |
|---|---|---|
| Linux | `~/.clap` | `~/.vst3` |
| macOS | `~/Library/Audio/Plug-Ins/CLAP` | `~/Library/Audio/Plug-Ins/VST3` |
| Windows | `C:\Program Files\Common Files\CLAP` | `C:\Program Files\Common Files\VST3` |

The GitHub workflow `.github/workflows/native-plugins.yml` builds all three systems and
keeps the results as downloadable artifacts of each run.

## Playing it

**Notes.** By default the voices are on the General MIDI drum notes, on any channel:

| Voice | Notes |
|---|---|
| Drum 1 | 35, 36 (kick) |
| Drum 2 | 41, 43, 45 (low toms) |
| Drum 3 | 47, 48, 50 (high toms) |
| Snare | 37-40 |
| Cymbal | 49, 51, 52, 55, 57, 59 |
| Closed Hihat | 42, 44 |
| Open Hihat | 46 |

With **Note Input** set to *Channels 1-7*, MIDI channels 1-7 play the seven voices and
the note number is the pitch, as on the LXR's MIDI input; channel 10 still plays the GM
drum notes. Played notes are recorded into the pattern while **Record** is on and the
sequencer runs.

**Sequencer clock.** *Host transport* (the default): the sequencer plays while the host
plays, at the host's tempo and on its grid. Starting, or a loop or jump, starts the step
at the host's position (or the next 16th). *Own clock*: **Play** starts and stops it at
**Tempo**. *Off*: notes only.

**Patterns.** **Pattern** picks the pattern (1-16). While the sequencer runs, the change
happens at the end of the current pattern, as on the LXR. **Shuffle** is the playing
pattern's shuffle.

**Step editor.** The patterns are edited through parameters, since there is no window:
**Edit Track** and **Edit Step** pick a step, and **Step On**, **Step Velocity**,
**Step Probability**, **Step Note**, **Step Flam**, **Step Shift** (micro timing) and the
two **Step Lock** target/value pairs show and change it. **Track Length** and the
**Euclid** parameters work on the edit track; changing a Euclid parameter rewrites the
track, like the LXR's Euclid page. These parameters show the pattern rather than set the
sound, so they are not automatable. A generic editor isn't a comfortable way to
program patterns. A plugin window with a step grid is the obvious next step.

**Outputs.** *Aux also in Main* (the default) gives the whole kit on the Main output,
for hosts that only use the first output. *Main and Aux separate* keeps voices routed
to Aux off Main.

**MIDI out.** With **Sequencer MIDI Out** on, the sequencer's notes (one MIDI channel per
voice, 1-7) and pattern changes (program change on channel 8) go to the plugin's MIDI
output.

## How it works

```
native/
  tools/codebox2cpp.py      rnbo codebox -> generated/LxrEngine.hpp, LxrSeq.hpp, LxrParams.hpp
  generated/                the translated engine, sequencer and parameter table (committed)
  core/codebox_rt.hpp       the few codebox types and functions the translation uses
  core/LxrInstrument.*      parameters, notes, transport, step editor, MIDI out, state
  plugin/lxr_clap.cpp       the CLAP plugin
  plugin/lxr_clap_entry.cpp its entry point (clap-wrapper builds the VST3 from it)
  test/lxr_native.cpp       driver for the firmware comparisons
  test/clap_host_test.cpp   loads LXR.clap as a host does
```

`codebox2cpp.py` translates the codebox subset the RNBO sources use into a C++ class
with every number a `double`, as in codebox, so the C++ does the same arithmetic as the
RNBO version. The engine emulates the firmware's float32 and 16-bit arithmetic, so
the native code is built without fused multiply-add contraction and without fast math
(`-ffp-contract=off -fno-fast-math`); with them it would drift from the firmware.
After changing anything in `rnbo/src`, run `python3 rnbo/tools/build.py` and
`python3 native/tools/codebox2cpp.py` (or build the `lxr-generate` target).

As on the hardware, the engine renders 64-sample blocks and the sequencer runs the
firmware's 96 ppq clock in 0.25 ms systicks. Following a host, the sequencer's clock
advances by the host's song position instead of wall-clock time, so it stays locked to
the host's grid at any tempo, including fractional ones (the LXR's own clock works in
whole BPM). Steps take effect at the engine's next 64-sample block, as on the LXR: at
48 kHz, within 1.3 ms of the grid.

`LxrInstrument` has no plugin-format code, so other targets can use it directly.

## Differences from the RNBO version and the hardware

- Everything listed in [the RNBO version's differences](../rnbo/README.md#differences-from-the-hardware)
  that concerns the sound engine and sequencer.
- A sequencer step sends one MIDI note on. The firmware sends two for each step, one
  from the voice and one from the sequencer; the plugin keeps the sequencer's, which
  carries the MIDI note override.
- The sequencer follows the host's tempo exactly, fractional tempos included. Tempo-synced
  LFOs and the delay use it rounded to whole BPM, as the LXR does.

## Tests

```
cmake --build native/build --target lxr_clap_test && ctest --test-dir native/build
node rnbo/test/run.mjs --native       # engine vs firmware audio, 52 scenarios
node rnbo/test/seqref.mjs --native    # sequencer vs firmware events, 35 scenarios
```

The host test loads `LXR.clap`, programs steps through the step editor, plays with a
host transport (start, a loop, stop) and with the plugin's own clock, and checks the
timing of the MIDI out against the beat, live notes, and that the state survives a save
and load into a second instance. `--native` makes the firmware comparisons translate
and compile the C++ (with `g++`, or `$CXX`) and run it instead of the JavaScript
translation of the codebox.

The plugins also pass [clap-validator](https://github.com/free-audio/clap-validator)
(all tests; the skipped ones are for extensions the plugin doesn't have) and the
VST3 SDK's validator (47 of 47 tests).

## License

GPL-3.0, like the rest of this repository. The plugins are built with clap-wrapper and
CLAP (MIT) and the VST3 SDK (MIT since version 3.8). "LXR" and "Erica Synths" are
trademarks of Erica Synths; see the [repository README](../README.md#licence-trademarks-and-hardware).
