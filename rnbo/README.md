# LXR for Ableton Move (RNBO)

A port of the LXR V2 drum machine's sound engine to [RNBO](https://rnbo.cycling74.com/),
packaged as a playable drum machine for Ableton Move in RNBO Move Takeover mode: the
seven LXR voices on the pads, a 16-step sequencer on the step buttons, and every
LXR sound parameter as an RNBO parameter (presets, parameter views).

The engine is a line-by-line port of the firmware DSP in `Core/Src/Soundengine`, and it is
tested against that C code compiled for the host: 51 scenarios (every voice,
waveform, filter, transient, LFO and FX type, plus randomized kits) must match the
firmware's output at better than 70 dB SNR. Most match to 90-120 dB or bit-exactly.

```
rnbo/
  src/lxr_engine.cb.js        engine, RNBO codebox~ (hand-written part)
  src/lxr_move_ui.cb.js       Move control surface, RNBO codebox
  tools/build.py              -> generated/lxr_engine.codebox.js, generated/params.json
  tools/patch.py              -> patchers/lxr-move.maxpat
  patchers/lxr-move.maxpat    the Max patch to export (generated)
  patchers/vendor/            Cycling '74's Move input/LED drivers (MIT)
  test/                       firmware comparison, sequencer and UI tests
```

## Putting it on the Move

You need Max 9 with RNBO and a Move set up for RNBO Move Takeover (see Cycling '74's
RNBO Move documentation and [rnbo.move.templates](https://github.com/Cycling74/rnbo.move.templates)).

1. Open `rnbo/patchers/lxr-move.maxpat` in Max and double-click the `rnbo~` object.
   The ezdac~ lets you listen on the computer first; external MIDI on channel 10
   (General MIDI drums) plays it there too.
2. Export with the Move target from the RNBO export sidebar.
3. On the Move, load the exported patcher in RNBO Move Takeover mode.

The engine's codebox~ is large (about 370 kB of code, mostly the firmware's wavetables
and transient samples), so the first compile takes a while.

## Playing it

| Control | Action |
| --- | --- |
| Bottom pad row | Drum1, Drum2, Drum3, Snare, Cymbal, closed hihat, open hihat (velocity sensitive) |
| Shift + bottom pad | select that track |
| Upper three pad rows | play the selected track chromatically, C3 to B4 (C4 is the sound's own pitch) |
| Step buttons | toggle steps of the selected track |
| Shift + step | toggle an accent (accented steps play with the accent velocity) |
| Delete + step | clear the step |
| Track buttons | select a track; the top button is the first track of the page |
| + / − | page the track buttons between tracks 1-4 and 5-7 |
| Mute + track button | mute / unmute that track; Mute alone: the selected track |
| Delete + track button | clear that track's pattern |
| ◀ / ▶ | shorten / lengthen the selected track (1-16 steps; tracks can differ) |
| Shift + ◀ / ▶ | tempo −1 / +1 BPM |
| Play | start / stop |
| Record | live recording: while the sequencer runs, pads are written to the pattern, quantized to the nearest 16th; velocity 100 or more records an accent |

The step LEDs show the selected track in its colour (dim: step on, bright: accent,
white: playhead, faint: off, dark: beyond the track length). The voice pads flash when
their voice plays. Volume, Back and the jog wheel stay with the RNBO control app.
The encoders are not used by this patch, so they are free for RNBO parameter views:
make views for the parameters you want under your fingers (for example per voice:
coarse, decay, cutoff, resonance, drive, volume, pan, FX send).

External MIDI: channels 1-7 play the tracks (the note sets the pitch), channel 10 plays
them from a General MIDI drum map (35/36 Drum1, 41/43/45 Drum2, 47/48/50 Drum3, 37-40
Snare, 49/51/52/55/57/59 Cymbal, 42/44 closed hihat, 46 open hihat).

## Parameters

All 227 LXR sound parameters are RNBO parameters with the LXR's own value ranges, so a
kit's values carry over 1:1 (names like `d1_coarse`, `sn_noise_freq`, `hh_decay_open`,
`fx_type`; the full list with ranges and defaults is in `generated/params.json`). The
defaults are the firmware's init kit. Modulation destinations (velocity and LFO
targets) are enums with the LXR's target names.

The pattern is in parameters too, so presets store kit and pattern together:
`seq_<track>_steps` and `seq_<track>_accents` (16 bits, step 1 = bit 0),
`seq_<track>_length`, `seq_swing`, `seq_velocity`, `seq_accent_velocity`, and `bpm`.
Tracks are `d1 d2 d3 sn cy ch oh`.

## How it works

The firmware renders 64-sample blocks: envelopes, LFOs, modulation and triggers run
once per block, the audio paths per sample in 16-bit integer arithmetic. The port keeps
that structure (block size, order of operations, 16-bit wrap and saturation points,
32-bit phase accumulators, float32 rounding where the firmware uses floats) so it
sounds like the hardware. The sequencer runs inside the engine, so steps land on the
same block boundaries as on the hardware. Timings follow the host sample rate.

The UI codebox talks to the engine through signals (triggers, run/mute state), the
`seq_*` parameters (pattern edits) and 18 status outputs it reads with `snapshot~ 10`.

## Differences from the hardware

- Only the sound engine is ported. The sequencer is a new, simpler one: 7 tracks of 16
  steps with accents, per-track length and swing. The LXR sequencer's sub-steps,
  per-step parameter automation, pattern chaining, Euclidean mode, kit morphing and
  MIDI out are not part of this port.
- Pitch slope 127 makes the firmware divide by zero (the pitch envelope becomes NaN);
  the port clamps the slope just below that.
- At the top wavetable octave the firmware interpolates one sample past the table end
  (reading the next table, or unrelated memory for saw); the port reads 0 there.
- Noise and random LFOs use one random stream per source instead of the shared
  hardware RNG: statistically the same, not sample-identical.
- The RAT distortion with feedback is chaotic, so the tests compare it by loudness.
- Triggers and parameter changes take effect at the next 64-sample block, as on the
  hardware.

## Tests

```
python3 rnbo/tools/build.py && python3 rnbo/tools/patch.py
node rnbo/test/run.mjs              # engine vs firmware, 51 scenarios
node rnbo/test/seq.mjs              # sequencer timing, swing, mutes, lengths
node rnbo/test/ui.mjs               # Move UI wired to the engine like the patch
```

`run.mjs` first compiles the firmware's DSP sources for the host with gcc
(`test/ref/build.py`, with small stubs for the hardware and patches that make x86 behave
like the Cortex-M7 where they differ, such as float-to-integer conversion).

`node rnbo/test/run.mjs --wav` writes the rendered scenarios to `rnbo/test/out/`.
`test/codebox.mjs` runs the codebox code in Node by translating it to JavaScript; it
covers the language subset the two codeboxes use. The patch itself can only be
compiled in Max, which these tests do not do.

## License

GPL-3.0 like the rest of this repository, except `patchers/vendor/` (MIT, Cycling '74).
