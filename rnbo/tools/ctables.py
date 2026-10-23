"""Parse constant C arrays out of the LXR firmware sources."""
import re
from pathlib import Path

CORE = Path(__file__).resolve().parents[2] / "Core"


def c_array(path, name):
    """Return the flat list of numbers in the initializer of C array `name`."""
    text = Path(path).read_text()
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    m = re.search(r"\b" + re.escape(name) + r"\s*(\[[^=]*\])\s*=\s*\{", text)
    if not m:
        raise KeyError(name)
    i = m.end()
    depth = 1
    j = i
    while depth:
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        j += 1
    body = text[i:j - 1]
    out = []
    for tok in re.findall(r"[-+]?0[xX][0-9a-fA-F]+|[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", body):
        out.append(float(int(tok, 16)) if "x" in tok.lower() else float(tok))
    return out


def c_array2d(path, name, rows, cols, signed8=False):
    """Like c_array, but keeps C's zero-fill for short rows of a 2D initializer."""
    text = Path(path).read_text()
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    m = re.search(r"\b" + re.escape(name) + r"\s*(\[[^=]*\])\s*=\s*\{", text)
    body = text[m.end():]
    out = []
    for r in range(rows):
        a = body.index("{")
        b = body.index("}", a)
        row = [float(int(t, 16)) if "x" in t.lower() else float(t)
               for t in re.findall(r"[-+]?0[xX][0-9a-fA-F]+|[-+]?\d+", body[a + 1:b])]
        if signed8:
            row = [v - 256 if v > 127 else v for v in row]
        assert len(row) <= cols, (name, r, len(row))
        out += row + [0.0] * (cols - len(row))
        body = body[b + 1:]
    return out


def dsp(rel):
    return CORE / "Src/Soundengine/DSPAudio" / rel


def tables():
    return {
        "sine": c_array(dsp("wavetable.c"), "sine_table"),
        "saw": c_array2d(dsp("wavetable.c"), "sawTable", 11, 1024),
        "tri": c_array2d(dsp("wavetable.c"), "triTable", 11, 1024),
        "rec": c_array2d(dsp("wavetable.c"), "recTable", 11, 1024),
        "transient": c_array2d(dsp("transientTables.c"), "transientData", 12, 2205, signed8=True),
        "ecc83": c_array(dsp("FX/Distortion.c"), "ecc83Table"),
        "diode": c_array(dsp("FX/Distortion.c"), "table_1n914"),
        "notefreq": c_array(CORE / "Inc/Soundengine/MIDI/MidiNoteNumbers.h", "MidiNoteFrequencies"),
    }
