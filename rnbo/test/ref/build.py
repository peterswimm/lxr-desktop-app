#!/usr/bin/env python3
"""Build lxr_ref (the firmware's own sound engine) and seq_ref (its sequencer),
compiled for the host.

The DSP sources are copied from Core/ unchanged except for the float->uint32
conversions listed in PATCHES. The Cortex-M7 converts those with a saturating
instruction (negative -> 0, too large -> 0xFFFFFFFF) while x86 wraps, so they go
through ARM_F2U() to keep the hardware's behaviour. Everything the engine needs
from the rest of the firmware (HAL, sequencer, menu, UART) is stubbed in stubs.c.

seq_ref is sequencer.c, EuklidGenerator.c and automationNode.c with seq_stubs.c and
seq_driver.c; see SEQ_PATCHES for the two changes.
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORE = HERE.parents[2] / "Core"
BUILD = HERE / "build"

# headers patched in the build's copy of Core/Inc: (file, old, new, expected count)
HEADER_PATCHES = [
    # Interpolation in the highest band-limited table reads sawTable[10][1024], one past
    # the end. On the hardware that is whatever the linker put next; tri/rec run into
    # rec/saw[0][0] (both 0). Give saw a zero row too so the reference is deterministic.
    ("Soundengine/DSPAudio/wavetable.h", "extern const int16_t sawTable[11][1024];",
     "extern const int16_t sawTable[12][1024];", 1),
]

SOURCES = [
    "Src/Soundengine/DSPAudio/1PoleLp.c",
    "Src/Soundengine/DSPAudio/CymbalVoice.c",
    "Src/Soundengine/DSPAudio/Decay.c",
    "Src/Soundengine/DSPAudio/DrumVoice.c",
    "Src/Soundengine/DSPAudio/HiHat.c",
    "Src/Soundengine/DSPAudio/Oscillator.c",
    "Src/Soundengine/DSPAudio/ResonantFilter.c",
    "Src/Soundengine/DSPAudio/SlopeEg2.c",
    "Src/Soundengine/DSPAudio/Snare.c",
    "Src/Soundengine/DSPAudio/distortion.c",
    "Src/Soundengine/DSPAudio/lfo.c",
    "Src/Soundengine/DSPAudio/mixer.c",
    "Src/Soundengine/DSPAudio/modulationNode.c",
    "Src/Soundengine/DSPAudio/snapEg.c",
    "Src/Soundengine/DSPAudio/transientGenerator.c",
    "Src/Soundengine/DSPAudio/transientTables.c",
    "Src/Soundengine/DSPAudio/wavetable.c",
    "Src/Soundengine/DSPAudio/FX/Compressor.c",
    "Src/Soundengine/DSPAudio/FX/Delay.c",
    "Src/Soundengine/DSPAudio/FX/Distortion.c",
    "Src/Soundengine/DSPAudio/FX/FxRamBuffer.c",
    "Src/Soundengine/DSPAudio/FX/Ringmod.c",
    "Src/Soundengine/FxBus.c",
    "Src/Soundengine/MIDI/MidiParser.c",
    "Src/Soundengine/MIDI/ParameterArray.c",
]

# (file, old, new, expected count)
PATCHES = [
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "return (((TABLESIZE*f)/REAL_FS))*1048576 ;",
     "return ARM_F2U((((TABLESIZE*f)/REAL_FS))*1048576) ;", 1),
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "return (((1024*f)/REAL_FS))*4194304 ;",
     "return ARM_F2U((((1024*f)/REAL_FS))*4194304) ;", 1),
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "return (((1024*f)/REAL_FS))*131072 ;",
     "return ARM_F2U((((1024*f)/REAL_FS))*131072) ;", 1),
    # float -> int32 of the filter output: NaN gives 0 on the Cortex-M7, INT_MIN on x86
    *[("Src/Soundengine/DSPAudio/ResonantFilter.c", f"tmp = {e};", f"tmp = ARM_F2I({e});", 2) for e in (
        "fastTanh(y1) * 0x7fff ", "h * FILTER_GAIN", "y0 * FILTER_GAIN", "ugb * FILTER_GAIN",
        "(x-ugb) * FILTER_GAIN", "(y1-h) * FILTER_GAIN", "(filter->b  * FILTER_GAIN)")],
    ("Src/Soundengine/DSPAudio/wavetable.c",
     "const int16_t sawTable[11][1024] =", "const int16_t sawTable[12][1024] =", 1),
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "return (12*fast_log2((uint32_t)(f/440.f)))+70;",
     "return (12*fast_log2(ARM_F2U(f/440.f)))+70;", 1),
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "(((uint32_t)(modBuffer[i]*osc->fmMod))<<",
     "((ARM_F2U(modBuffer[i]*osc->fmMod))<<", 5),  # 2 of them are in disabled code
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "(((uint32_t)(modOsc->output*osc->fmMod))<<",
     "((ARM_F2U(modOsc->output*osc->fmMod))<<", 3),  # 1 in disabled code
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "const uint32_t index2 = index + pwm * (UINT32_MAX/2);",
     "const uint32_t index2 = ARM_F2U((float)index + pwm * (UINT32_MAX/2));", 1),
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "const uint32_t oscPhase2 = osc->phase + pwm * (UINT32_MAX/2);",
     "const uint32_t oscPhase2 = ARM_F2U((float)osc->phase + pwm * (UINT32_MAX/2));", 1),
    ("Src/Soundengine/DSPAudio/transientGenerator.c",
     "transient->phase +=  (transient->pitch*(1<<20));",
     "transient->phase = ARM_F2U((float)transient->phase + (transient->pitch*(1<<20)));", 1),
    # one random stream per noise source (see host_rng in driver.c)
    ("Src/Soundengine/DSPAudio/Oscillator.c",
     "osc->output = GetRngValue(); //normal pitched white noise",
     "osc->output = host_rng(osc); //normal pitched white noise", 1),
    ("Src/Soundengine/DSPAudio/lfo.c",
     "lfo->rnd = GetRngValue();", "lfo->rnd = host_rng(lfo);", 1),
    ("Src/Soundengine/DSPAudio/lfo.c",
     "uint32_t incInt = inc;", "uint32_t incInt = ARM_F2U(inc);", 1),
    ("Src/Soundengine/DSPAudio/lfo.c",
     "return freq/(LFO_SR) * 0xffffffff;", "return ARM_F2U(freq/(LFO_SR) * 0xffffffff);", 2),
    ("Src/Soundengine/DSPAudio/lfo.c",
     "return (lfoSyncfreq / (LFO_SR)) * 0xffffffff;",
     "return ARM_F2U((lfoSyncfreq / (LFO_SR)) * 0xffffffff);", 1),
    ("Src/Soundengine/MIDI/MidiParser.c",
     "lfo.phaseOffset = value.flt * 0xffffffff;",
     "lfo.phaseOffset = ARM_F2U(value.flt * 0xffffffff);", 4),
]


SEQ_SOURCES = [
    "Src/Soundengine/Sequencer/sequencer.c",
    "Src/Soundengine/Sequencer/EuklidGenerator.c",
    "Src/Soundengine/DSPAudio/automationNode.c",
]

SEQ_PATCHES = [
    # The Euclid generator stores a length of 16 as 0, which the step code then uses as
    # a divisor (x % 0 is x on the Cortex-M7, a crash on x86) and as length - 1 = 255
    # when it looks up the previous step (an out-of-bounds read). The port keeps 16.
    ("Src/Soundengine/Sequencer/EuklidGenerator.c", "\tif(len == 16)\n\t\tlen=0;\n", "", 1),
    # On a pattern change the firmware restores the outgoing pattern from a backup taken
    # when it was loaded from the SD card, dropping unsaved edits. The port keeps edits
    # (the patterns are saved with the RNBO preset), so the reference does too.
    ("Src/Soundengine/Sequencer/sequencer.c",
     "\t\tseq_copyBackupBufferToPattern(seq_activePattern);\n", "", 1),
]


def build_seq():
    texts = {s: (CORE / s).read_text() for s in SEQ_SOURCES}
    for f, old, new, n in SEQ_PATCHES:
        got = texts[f].count(old)
        if got != n:
            sys.exit(f"patch mismatch in {f}: {old!r} found {got}x, expected {n}")
        texts[f] = texts[f].replace(old, new)
    objs = []
    for s, t in texts.items():
        dst = BUILD / "src" / Path(s).name
        dst.write_text(t)
        objs.append(str(dst))
    cmd = ["gcc", "-O2", "-std=gnu11", "-w", "-fno-strict-aliasing",
           "-I", str(BUILD / "inc"),
           *objs, str(HERE / "seq_stubs.c"), str(HERE / "seq_driver.c"),
           "-lm", "-o", str(BUILD / "seq_ref")]
    subprocess.run(cmd, check=True)
    print(BUILD / "seq_ref")


def main():
    if BUILD.exists():
        shutil.rmtree(BUILD)
    (BUILD / "src").mkdir(parents=True)
    texts = {s: (CORE / s).read_text() for s in SOURCES}
    for f, old, new, n in PATCHES:
        got = texts[f].count(old)
        if got != n:
            sys.exit(f"patch mismatch in {f}: {old!r} found {got}x, expected {n}")
        texts[f] = texts[f].replace(old, new)
    # Firmware headers include "main.h" etc. relative to themselves, which would bypass -I,
    # so build against a copy of Core/Inc with the stubs laid over it.
    shutil.copytree(CORE / "Inc", BUILD / "inc")
    for stub in (HERE / "stubs").glob("**/*.h"):
        dst = BUILD / "inc" / stub.relative_to(HERE / "stubs")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(stub, dst)
    for f, old, new, n in HEADER_PATCHES:
        path = BUILD / "inc" / f
        t = path.read_text()
        if t.count(old) != n:
            sys.exit(f"patch mismatch in {f}: {old!r}")
        path.write_text(t.replace(old, new))
    objs = []
    for s, t in texts.items():
        dst = BUILD / "src" / Path(s).name
        dst.write_text(t)
        objs.append(str(dst))
    cmd = ["gcc", "-O2", "-std=gnu11", "-w", "-fno-strict-aliasing",
           "-I", str(BUILD / "inc"),
           *objs, str(HERE / "stubs.c"), str(HERE / "driver.c"),
           "-lm", "-o", str(BUILD / "lxr_ref")]
    subprocess.run(cmd, check=True)
    print(BUILD / "lxr_ref")
    build_seq()


if __name__ == "__main__":
    main()
