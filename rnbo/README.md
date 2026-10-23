# LXR for Ableton Move (RNBO)

A port of the LXR V2 drum machine (sound engine and sequencer) to
[RNBO](https://rnbo.cycling74.com/), packaged for Ableton Move in RNBO Move Takeover
mode. The seven LXR voices are on the pads and the LXR sequencer is on the step
buttons: 64-step tracks, per-step velocity, probability, note, flam, micro timing and
parameter locks, shuffle, roll, patterns and chains. Every LXR sound parameter is an RNBO
parameter (presets, parameter views).

The same sources also build as CLAP and VST3 plugins for desktop hosts: see
[native/README.md](../native/README.md).

Both halves are line-by-line ports of the firmware, tested against the firmware's own C
code compiled for the host:

- **Sound engine** (`Core/Src/Soundengine`): 52 scenarios (every voice, waveform,
  filter, transient, LFO and FX type, parameter locks, randomized kits) must match the
  firmware's audio at better than 70 dB SNR. Most match to 90-120 dB or bit-exactly.
- **Sequencer** (`Core/Src/Soundengine/Sequencer`): 35 scenarios (timing, shuffle,
  flams and shifts, probability, polymeter, pattern and song changes, roll, recording,
  automation, Euclid, editing, MIDI out, random fuzz) must produce the identical stream
  of voice triggers, parameter lock changes and MIDI messages, at the same 0.25 ms tick.

```
rnbo/
  src/lxr_engine.cb.js        sound engine, RNBO codebox~ (hand-written part)
  src/lxr_seq.cb.js           the LXR sequencer, RNBO codebox
  src/lxr_move_ui.cb.js       Move control surface (same codebox as the sequencer)
  tools/build.py              -> generated/lxr_engine.codebox.js, lxr_move.codebox.js, params.json
  tools/patch.py              -> patchers/lxr-move.maxpat
  patchers/lxr-move.maxpat    the Max patch to export (generated)
  patchers/vendor/            Cycling '74's Move input/LED drivers (MIT)
  test/                       firmware comparisons and UI tests
```

## Putting it on the Move

You need Max 9 with RNBO 1.4 or later (pattern data is saved in presets through
`liststore`, which needs 1.4) and a Move set up for RNBO Move Takeover (see Cycling
'74's RNBO Move documentation and
[rnbo.move.templates](https://github.com/Cycling74/rnbo.move.templates)).

1. Open `rnbo/patchers/lxr-move.maxpat` in Max and double-click the `rnbo~` object.
   The ezdac~ lets you listen on the computer first; external MIDI on channel 10
   (General MIDI drums) plays it there too.
2. Export with the Move target from the RNBO export sidebar.
3. On the Move, load the exported patcher in RNBO Move Takeover mode.

The engine's codebox~ is large (about 370 kB of code, mostly the firmware's wavetables
and transient samples), so the first compile takes a while. Pattern 1 starts with a
small demo beat; press Play.

## Playing it

**Step view** (default): the 16 step buttons show one bar of the selected track.

| Control | Action |
| --- | --- |
| Step | tap: on / off. Hold: edit the step (below) |
| Hold step + encoders | 1 velocity, 2 probability, 3 note, 4 flam (off, x2, x3, x4, x6, x8), 5 micro shift (−7 … +7 24ths of a step), 6 lock lane (1 / 2), 7 lane 1 value, 8 lane 2 value |
| Hold step + move a parameter | parameter lock: open a parameter view, hold steps and turn; the steps play with that value. The parameter goes back when you let go of the steps |
| Shift + step | the track ends at this step (1-64) |
| Delete + step | clear the step |
| ◀ / ▶ | previous / next bar (bars 1-4; the view follows the playhead until you do this, Play turns following back on) |
| Shift + ◀ / ▶ | tempo −1 / +1 BPM |
| Capture + encoders 1-3 | Euclid generator for the selected track: length, steps, rotation |

**Tracks and pads**

| Control | Action |
| --- | --- |
| Track buttons | select a track; the top button is the first track of the page |
| + / − | page the track buttons between tracks 1-4 and 5-7 |
| Mute + track | mute / unmute (Mute alone: the selected track) |
| Delete + track | clear the track |
| Shift + Delete + track | clear the track's locks in the selected lane |
| Duplicate + track | copy the selected track there |
| Bottom pad row | Drum1, Drum2, Drum3, Snare, Cymbal, closed hihat, open hihat (velocity sensitive) |
| Shift + bottom pad | select that track |
| Upper three pad rows | the selected track chromatically, C3 to B4 (C4 is the sound's own pitch) |
| Repeat + bottom pads | roll the held voices |
| Repeat + pads 9-14 | roll rate: one shot, 1/1, 1/2, 1/4, 1/8, 1/16 |

**Patterns and transport**

| Control | Action |
| --- | --- |
| Session | pattern view: the step buttons are the 16 patterns (green: playing, blinking: next, blue: in the chain, grey: has steps) |
| Pattern | play it next (at the end of the playing pattern, or now when stopped) |
| Hold a pattern + tap others | chain them (the LXR's song mode); a single tap ends the chain |
| Delete + pattern / Duplicate + pattern | clear it / copy the playing pattern there |
| Play | start / stop (Shift + Play: restart) |
| Record | live recording: pads and external notes go into the pattern, quantized like the LXR (to the playing step, or the next one past half a step); parameter moves go into the playing step's lock lane |
| Shift + Delete | while recording: erase the selected track's steps as they play |

The step LEDs show the selected track in its colour, brighter for higher velocity, paler
for steps with locks, dimmer for steps with probability below 100%. The playhead is white
and a held step bright white. The encoder LEDs show the bar position, or the held step's
values while you edit it. The voice pads flash when their voice plays. Volume, Back and
the jog wheel stay with the RNBO control app. The encoders are free for RNBO parameter
views whenever no step is held and Capture is not down.

External MIDI: channels 1-7 play the tracks (the note sets the pitch), channel 10 plays
them from a General MIDI drum map (35/36 Drum1, 41/43/45 Drum2, 47/48/50 Drum3, 37-40
Snare, 49/51/52/55/57/59 Cymbal, 42/44 closed hihat, 46 open hihat).

## Parameters

All 227 LXR sound parameters are RNBO parameters with the LXR's own value ranges, so a
kit's values carry over 1:1 (names like `d1_coarse`, `sn_noise_freq`, `hh_decay_open`,
`fx_type`; the full list with ranges and defaults is in `generated/params.json`). The
defaults are the firmware's init kit. Modulation destinations (velocity and LFO
targets) are enums with the LXR's target names.

Sequencer settings: `bpm`, `shuffle` (0-127, stored per pattern like on the LXR),
`midi_out` (off by default), `midi_chan_<track>` and `midi_chan_global` (program changes),
`midi_note_<track>` (0: send the step's note, otherwise always this note). Tracks are
`d1 d2 d3 sn cy ch oh`.

The patterns themselves are not parameters: they live in the sequencer codebox's
`liststore`s, which RNBO saves and restores with the presets, so a preset holds kit and
patterns together.

## How it works

The firmware renders 64-sample blocks: envelopes, LFOs, modulation and triggers run
once per block, the audio paths per sample in 16-bit integer arithmetic. The engine
keeps that structure (block size, order of operations, 16-bit wrap and saturation
points, 32-bit phase accumulators, float32 rounding where the firmware uses floats) so it
sounds like the hardware. Timings follow the host sample rate.

The sequencer runs the firmware's 96 ppq clock (24 ticks per step) in the firmware's
own time unit, the 0.25 ms systick, with its single-precision arithmetic and the way the
audio interrupt calls it, so shuffle, flams and shifts land where the LXR puts them. A
`[metro 1]` drives it four systicks per call. Its triggers reach the engine through
`[sig~]` and take effect at the next 64-sample block, as on the hardware. Parameter
locks go to the engine's `auto_<track>` attributes, where they work like the firmware's
automation nodes: each trigger first puts the track's previous lock targets back to
their parameter values, then sets the new ones.

Every `[param]` also reports its value to the sequencer codebox, which is how holding a
step and moving a parameter records a lock, and how recording catches parameter moves.
The codebox can set parameters back through a `[route]` (tempo, shuffle, releasing locks).

## Differences from the hardware

Sequencer:
- 16 patterns per preset instead of 64 (presets hold the pattern data; the number is
  `NUM_PATTERNS` in `src/lxr_seq.cb.js`).
- Edits to the playing pattern are kept when patterns change; the LXR discards unsaved
  edits and keeps the version last loaded from the SD card.
- While a step is held, a moved parameter also changes the sound until the step is let
  go (on the LXR it only records the lock).
- Fixed firmware bugs: with shuffle, the internal clock can land on the external-sync
  sentinel (`deltaT == -1`) and stall for 8 seconds; the Euclid generator stores a length
  of 16 as 0, which the step code then divides by; a roll, note or parameter move between
  pressing Play and the first step reads or writes step −1, outside the pattern data.
- Not ported: external clock sync, trigger outputs, loading a kit with each pattern, and
  (outside the sequencer) kit morphing.
- MIDI out goes to the patch's MIDI output, which in Takeover mode also carries the LED
  messages for the Move surface; it is off by default.

Sound engine:
- Pitch slope 127 makes the firmware divide by zero (the pitch envelope becomes NaN);
  the port clamps the slope just below that.
- At the top wavetable octave the firmware interpolates one sample past the table end
  (reading the next table, or unrelated memory for saw); the port reads 0 there.
- Noise, random LFOs and step probability use one random stream per source instead of
  the shared hardware RNG: statistically the same, not sample-identical.
- The RAT distortion with feedback is chaotic, so the tests compare it by loudness.

## Tests

```
python3 rnbo/tools/build.py && python3 rnbo/tools/patch.py
node rnbo/test/run.mjs              # engine vs firmware audio, 52 scenarios
node rnbo/test/seqref.mjs           # sequencer vs firmware events, 35 scenarios
node rnbo/test/ui.mjs               # Move UI + sequencer + engine wired like the patch
```

With `--native`, `run.mjs` and `seqref.mjs` test the C++ translation used by the
plugins (`native/`) instead.

Both firmware comparisons first compile the firmware's sources for the host with gcc
(`test/ref/build.py`: `lxr_ref` for the sound engine, `seq_ref` for the sequencer, with
small stubs for the hardware and patches that make x86 behave like the Cortex-M7 where
they differ, such as float-to-integer conversion). `node rnbo/test/run.mjs --wav`
writes the rendered audio scenarios to `rnbo/test/out/`.

`test/codebox.mjs` runs the codebox code in Node by translating it to JavaScript; it
covers the language subset the codeboxes use. The patch itself can only be compiled
in Max, which these tests do not do.

## License

GPL-3.0 like the rest of this repository, except `patchers/vendor/` (MIT, Cycling '74).
