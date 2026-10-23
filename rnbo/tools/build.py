#!/usr/bin/env python3
"""Generate the RNBO sources for the LXR port.

    python3 rnbo/tools/build.py

writes
    rnbo/generated/lxr_engine.codebox.js   the complete codebox~ code of the engine
    rnbo/generated/lxr_move.codebox.js     the Move control surface + sequencer codebox
    rnbo/generated/params.json             parameter list (used by the tests and the docs)

The hand-written engine is rnbo/src/lxr_engine.cb.js; this script fills in the parts
that come from the firmware sources: the @param declarations (LXR value ranges and the
factory init kit as defaults), the parameter scanner, the modulation target list and
the wavetables/transient samples (packed several values per number to keep the
codebox small).
"""
import json
import struct
from pathlib import Path

import ctables
import lxrparams
import paramdefs

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
GEN = ROOT / "generated"

CHUNK = 64  # packed numbers per fill call


def f32(x):
    """Value of x after a round trip through a C float."""
    return struct.unpack("f", struct.pack("f", x))[0]


def num(x):
    r = repr(float(x))
    return r[:-2] if r.endswith(".0") else r


# trigger slots / sequencer tracks and their parameter name prefixes
TRACKS = ["d1", "d2", "d3", "sn", "cy", "ch", "oh"]


def param_block(defs):
    # Codebox @params are attributes of the codebox~; the patch's [param] objects (with
    # names for the enums) set them, see rnbo/tools/patch.py. Enums are plain indices here.
    lines = []
    for d in defs:
        hi = len(d["enum"]) - 1 if d["enum"] else d["max"]
        lines.append("@param({min: %d, max: %d}) %s = %d;" % (d["min"], hi, d["name"], d["default"]))
    lines.append("@param({min: 20, max: 300}) bpm = 120;")
    # parameter locks from the sequencer, one per track (see autoScan())
    for t in TRACKS:
        lines.append("@param({min: 0, max: 4503599627370496}) auto_%s = 0;" % t)
    e = lxrparams.par_enum()
    lines.append("const END_OF_SOUND_PARAMETERS = %d;" % e["END_OF_SOUND_PARAMETERS"])
    return "\n".join(lines)


def scan_function(defs):
    by_par = {d["par"]: d for d in defs}
    e = lxrparams.par_enum()
    out = [
        "// Apply parameters that changed since the last block. The order is the one the",
        "// firmware uses when it loads a kit: modulation targets first, then",
        "// preset_sendDrumsetParameters()'s parameterOrder. O[] holds the values as set,",
        "// P[] the values in effect (a parameter lock can differ from O[]).",
        "function scanParams() {",
        "\tlet v = 0;",
    ]
    done = set()
    for i in range(6):
        for kind, fn in (("PAR_VEL_DEST_%d", "setVeloDest"), ("PAR_TARGET_LFO%d", "setLfoDest")):
            d = by_par[e[kind % (i + 1)]]
            out.append("\tv = pint(%s, %d);" % (d["name"], d["max"]))
            out.append("\tif (v != O[%d]) {" % d["par"])
            out.append("\t\tO[%d] = v;" % d["par"])
            out.append("\t\tP[%d] = v;" % d["par"])
            out.append("\t\t%s(%d, v);" % (fn, i))
            out.append("\t}")
            done.add(d["par"])
    for par in lxrparams.parameter_order():
        if par in done or par not in by_par:
            continue
        d = by_par[par]
        out.append("\tv = pint(%s, %d);" % (d["name"], d["max"]))
        out.append("\tif (v != O[%d]) {" % par)
        out.append("\t\tO[%d] = v;" % par)
        out.append("\t\tsetLxrParam(%d, v);" % par)
        out.append("\t}")
        done.add(par)
    assert done == set(by_par), set(by_par) - done
    out += [
        "\tv = max(1, floor(bpm + 0.5));",
        "\tif (v != curBpm) {",
        "\t\tcurBpm = v;",
        "\t\tbpmChanged();",
        "\t}",
        "}",
        "",
        "function pint(x, hi) {",
        "\tlet v = floor(x + 0.5);",
        "\tif (v < 0) return 0;",
        "\tif (v > hi) return hi;",
        "\treturn v;",
        "}",
    ]
    return "\n".join(out)


def packed(values, bits, per):
    """Pack integers (offset to be non-negative) `per` to a number, little end first."""
    off = 1 << (bits - 1)
    base = 1 << bits
    out = []
    for i in range(0, len(values), per):
        n = 0
        for k, v in enumerate(values[i:i + per]):
            v = int(v)
            assert -off <= v < off
            n += (v + off) * base ** k
        assert n < 2 ** 53
        out.append(n)
    return out


def fill_calls(fn, numbers):
    calls = []
    for i in range(0, len(numbers), CHUNK):
        chunk = numbers[i:i + CHUNK]
        calls.append("\t%s(%d, [%s]);" % (fn, i, ", ".join(str(n) for n in chunk)))
    return calls


def tables_function():
    t = ctables.tables()
    wt = []
    for name in ("tri", "saw", "rec"):
        tab = [int(x) for x in t[name]]
        # Interpolating in the top table reads one sample past the end ([10][1024]); in
        # the firmware that is the neighbouring table's first value (0) or, for saw,
        # unrelated memory. Use 0 for all three.
        wt += tab + [0]
    assert len(wt) == 3 * 11265
    sine = [int(x) for x in t["sine"]]
    trans = [int(x) for x in t["transient"]]
    targets = [par for _text, par in lxrparams.mod_targets()]
    lines = [
        "// unpack 3 x 16 bit per number",
        "function fillSine(base, c: list) {",
        "\tfor (let i = 0; i < c.length; i++) {",
        "\t\tlet n = c[i];",
        "\t\tfor (let k = 0; k < 3; k++) {",
        "\t\t\tlet idx = base * 3 + i * 3 + k;",
        "\t\t\tlet u = n - 65536 * floor(n / 65536);",
        "\t\t\tn = floor(n / 65536);",
        "\t\t\tif (idx < 4097) SINE[idx] = u - 32768;",
        "\t\t}",
        "\t}",
        "}",
        "",
        "function fillWave(base, c: list) {",
        "\tfor (let i = 0; i < c.length; i++) {",
        "\t\tlet n = c[i];",
        "\t\tfor (let k = 0; k < 3; k++) {",
        "\t\t\tlet idx = base * 3 + i * 3 + k;",
        "\t\t\tlet u = n - 65536 * floor(n / 65536);",
        "\t\t\tn = floor(n / 65536);",
        "\t\t\tif (idx < 33795) WT[idx] = u - 32768;",
        "\t\t}",
        "\t}",
        "}",
        "",
        "// unpack 6 x 8 bit per number",
        "function fillTransient(base, c: list) {",
        "\tfor (let i = 0; i < c.length; i++) {",
        "\t\tlet n = c[i];",
        "\t\tfor (let k = 0; k < 6; k++) {",
        "\t\t\tlet idx = base * 6 + i * 6 + k;",
        "\t\t\tlet u = n - 256 * floor(n / 256);",
        "\t\t\tn = floor(n / 256);",
        "\t\t\tif (idx < 26460) TR[idx] = u - 128;",
        "\t\t}",
        "\t}",
        "}",
        "",
        "function fillList(which, c: list) {",
        "\tfor (let i = 0; i < c.length; i++) {",
        "\t\tif (which == 0) ECC83[i] = c[i];",
        "\t\telse if (which == 1) DIODE[i] = c[i];",
        "\t\telse if (which == 2) NOTEF[i] = c[i];",
        "\t\telse if (which == 3) MT[i] = c[i];",
        "\t\telse HASPTR[c[i]] = 1;",
        "\t}",
        "}",
        "",
        "// firmware tables: wavetable.c, transientTables.c, FX/Distortion.c, MidiNoteNumbers.h",
        "function tablesInit() {",
    ]
    lines += fill_calls("fillSine", packed(sine, 16, 3))
    lines += fill_calls("fillWave", packed(wt, 16, 3))
    lines += fill_calls("fillTransient", packed(trans, 8, 6))
    diode = [f32(x) for x in t["diode"]]
    lines.append("\tfillList(0, [%s]);" % ", ".join(num(x) for x in t["ecc83"]))
    lines.append("\tfillList(1, [%s]);" % ", ".join(num(x) for x in diode + [diode[-1]]))
    lines.append("\tfillList(2, [%s]);" % ", ".join(num(f32(x)) for x in t["notefreq"]))
    lines.append("\tfillList(3, [%s]);" % ", ".join(str(p) for p in targets))
    lines.append("\tfillList(4, [%s]);" % ", ".join(str(p) for p in has_ptr()))
    lines.append("}")
    return "\n".join(lines)


def has_ptr():
    """PAR numbers with a parameterArray pointer (ParameterArray.c): only those can be
    modulated. Every sound parameter has one except the ones listed here."""
    e = lxrparams.par_enum()
    none = {0, 5, 97, 98, 127, 244, 246}
    none |= set(range(e["PAR_VOICE_LFO1"], e["PAR_TARGET_LFO6"] + 1))
    none |= set(range(e["PAR_UNUSED_1"], e["PAR_UNUSED_7"] + 1))
    return [i for i in range(e["END_OF_SOUND_PARAMETERS"]) if i not in none]


def build_engine():
    defs = paramdefs.definitions()
    src = (SRC / "lxr_engine.cb.js").read_text()
    head = ("// GENERATED by rnbo/tools/build.py from rnbo/src/lxr_engine.cb.js and the\n"
            "// firmware sources. Do not edit; edit those and run the script again.\n\n")
    src = src.replace("//@@PARAMS@@", param_block(defs))
    src = src.replace("//@@GENERATED@@", scan_function(defs) + "\n\n" + tables_function())
    GEN.mkdir(exist_ok=True)
    (GEN / "lxr_engine.codebox.js").write_text(head + src)
    (GEN / "params.json").write_text(json.dumps(
        {"params": defs, "has_ptr": has_ptr(),
         "mod_targets": [par for _text, par in lxrparams.mod_targets()],
         "tracks": TRACKS,
         "scan_order": [d for d in scan_order(defs)]}, indent=1))
    return src


def scan_order(defs):
    """PAR numbers in the order scanParams() applies them."""
    by_par = {d["par"]: d for d in defs}
    e = lxrparams.par_enum()
    order = []
    for i in range(6):
        order += [e["PAR_VEL_DEST_%d" % (i + 1)], e["PAR_TARGET_LFO%d" % (i + 1)]]
    order += [p for p in lxrparams.parameter_order() if p in by_par and p not in order]
    return order


def build_move():
    """The sequencer port and the Move UI share one message codebox."""
    head = ("// GENERATED by rnbo/tools/build.py from rnbo/src/lxr_seq.cb.js and\n"
            "// rnbo/src/lxr_move_ui.cb.js. Do not edit; edit those and run the script again.\n\n")
    src = head + (SRC / "lxr_seq.cb.js").read_text() + "\n" + (SRC / "lxr_move_ui.cb.js").read_text()
    (GEN / "lxr_move.codebox.js").write_text(src)
    return src


def main():
    src = build_engine()
    print("engine: %d lines, %d kB" % (src.count("\n"), len(src) // 1024))
    src = build_move()
    print("move ui + sequencer: %d lines, %d kB" % (src.count("\n"), len(src) // 1024))


if __name__ == "__main__":
    main()
