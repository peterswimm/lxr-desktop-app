#!/usr/bin/env python3
r"""Generate the Max patch for the LXR on Ableton Move.

    python3 rnbo/tools/build.py      (first: the codebox code and params.json)
    python3 rnbo/tools/patch.py

writes rnbo/patchers/lxr-move.maxpat: an [rnbo~] whose patcher holds

    [midiin] -> Move input driver ------------------------------\
    [notein] (external MIDI) -> [pack 100 note velocity channel] --> Move codebox --> Move LED driver
    [loadmess 1] -> [metro 1] -> [t 4] (clock) ---------------------/  |  |  |  \--> [midiout]
    [param <x>] -> [t 101 f] -> [pack 101 <index> 0.] (value events) -/   |  |  |
         ^-- [route <index> ...] <-- tempo, shuffle, lock release -------/   |  |
    triggers: [sig~] x 7 --------------------------------------------------/   |
    parameter locks: [set auto_<track>] x 7 ------------------------------------/
    [param <x>] -> [set <x>] -------------------------------> engine codebox~ -> [out~ 1] [out~ 2]

The Move codebox is the ported LXR sequencer plus the Move UI (build.py joins
src/lxr_seq.cb.js and src/lxr_move_ui.cb.js). Every LXR sound parameter is an RNBO
[param] (presets, parameter views); the sequencer's patterns live in the codebox's
liststores, which RNBO saves with the presets too. The two Move drivers are the
unmodified Cycling '74 templates in rnbo/patchers/vendor.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "generated"
VENDOR = ROOT / "patchers" / "vendor"
OUT = ROOT / "patchers" / "lxr-move.maxpat"

TITLE = "LXR"
N_UI_OUT = 17
# parameter indices the UI knows besides the LXR parameter numbers (IDX_* in
# src/lxr_move_ui.cb.js)
IDX_BPM = 300
IDX_SHUFFLE = 301
IDX_MIDI_OUT = 302
IDX_MIDI_CHAN = 303
IDX_MIDI_NOTE = 311

APPVERSION = {"major": 9, "minor": 1, "revision": 3, "architecture": "x64", "modernui": 1}

VOICE_NAMES = {"d1": "Drum1", "d2": "Drum2", "d3": "Drum3", "sn": "Snare", "cy": "Cymbal",
               "hh": "Hihat", "ch": "Closed Hihat", "oh": "Open Hihat", "fx": "FX", "all": "All",
               "midi": "MIDI", "chan": "Channel", "global": "Global"}


def display_name(name):
    if name == "bpm":
        return "Tempo"
    words = []
    for w in name.split("_"):
        if w in VOICE_NAMES:
            words.append(VOICE_NAMES[w])
        elif w in ("fm", "lfo", "pwm"):
            words.append(w.upper())
        else:
            words.append(w.capitalize())
    return " ".join(words)


def quote(s):
    assert not any(c in s for c in '",;$\\{}'), s
    return '"%s"' % s if " " in s or not s else s


class Patcher:
    def __init__(self, namespace, rect, title=None):
        self.boxes = []
        self.lines = []
        self.attrs = {
            "fileversion": 1,
            "appversion": APPVERSION,
            "classnamespace": namespace,
            "rect": rect,
            "default_fontname": "Lato",
            "gridsize": [15.0, 15.0],
        }
        if title:
            self.attrs["title"] = title

    def add(self, box):
        box["id"] = "obj-%d" % (len(self.boxes) + 1)
        self.boxes.append({"box": box})
        return box["id"]

    def obj(self, text, x, y, inlets, outlets, outlettype=None, width=None, **extra):
        box = {
            "maxclass": "newobj",
            "text": text,
            "numinlets": inlets,
            "numoutlets": outlets,
            "patching_rect": [float(x), float(y), float(width or max(40, 7 * len(text) + 16)), 23.0],
        }
        if outlets:
            box["outlettype"] = outlettype or [""] * outlets
        box.update(extra)
        return self.add(box)

    def comment(self, text, x, y, width=400.0, height=None):
        lines = text.count("\n") + 1
        return self.add({
            "maxclass": "comment",
            "text": text,
            "numinlets": 1,
            "numoutlets": 0,
            "patching_rect": [float(x), float(y), float(width), float(height or 20 * lines + 6)],
        })

    def codebox(self, maxclass, code, x, y, w, h, inlets, outlets, outlettype, extra_attrs):
        attrs = {"code": code, "hot": 0}
        attrs.update(extra_attrs)
        return self.add({
            "maxclass": maxclass,
            "code": code,
            "fontface": 0,
            "fontname": "<Monospaced>",
            "fontsize": 12.0,
            "numinlets": inlets,
            "numoutlets": outlets,
            "outlettype": outlettype,
            "patching_rect": [float(x), float(y), float(w), float(h)],
            "rnbo_extra_attributes": attrs,
        })

    def line(self, src, outlet, dst, inlet):
        self.lines.append({"patchline": {"source": [src, outlet], "destination": [dst, inlet]}})

    def to_dict(self):
        d = dict(self.attrs)
        d["boxes"] = self.boxes
        d["lines"] = self.lines
        return d


def param_text(p):
    """[param] text for one parameter dict (name, min, max, default, enum?)."""
    words = ["param", p["name"]]
    if p.get("enum"):
        words += ["@enum"] + [quote(e) for e in p["enum"]]
        words += ["@value", str(p["default"])]
    else:
        words += [str(p["default"]), "@min", str(p["min"]), "@max", str(p["max"]),
                  "@steps", str(p["max"] - p["min"] + 1)]
    words += ["@displayname", quote(display_name(p["name"]))]
    return " ".join(words)


def all_params(info):
    """Every [param]: (dict, UI index or None, engine attribute or None)."""
    out = [(dict(p), p["par"], p["name"]) for p in info["params"]]
    out.append(({"name": "bpm", "min": 20, "max": 300, "default": 120}, IDX_BPM, "bpm"))
    out.append(({"name": "shuffle", "min": 0, "max": 127, "default": 0}, IDX_SHUFFLE, None))
    out.append(({"name": "midi_out", "enum": ["off", "on"], "default": 0}, IDX_MIDI_OUT, None))
    for t, name in enumerate(info["tracks"]):
        out.append(({"name": "midi_chan_%s" % name, "min": 1, "max": 16, "default": t + 1},
                    IDX_MIDI_CHAN + t, None))
    out.append(({"name": "midi_chan_global", "min": 1, "max": 16, "default": 8}, IDX_MIDI_CHAN + 7, None))
    for t, name in enumerate(info["tracks"]):
        out.append(({"name": "midi_note_%s" % name, "min": 0, "max": 127, "default": 0},
                    IDX_MIDI_NOTE + t, None))
    return out


def build_rnbo_patcher(info, engine_code, move_code):
    p = Patcher("rnbo", [100.0, 100.0, 1600.0, 1000.0], TITLE)
    tracks = info["tracks"]

    p.comment("LXR drum machine for Ableton Move (RNBO Move Takeover).\n"
              "Generated by rnbo/tools/patch.py - edit the sources in rnbo/src and run "
              "rnbo/tools/build.py and rnbo/tools/patch.py again.\n"
              "Move input/LED drivers: Cycling '74 rnbo.move.templates, MIT license "
              "(rnbo/patchers/vendor).", 20, 10, 900)

    # --- Move controls in
    midiin = p.obj("midiin", 20, 90, 1, 1)
    drv_in = json.loads((VENDOR / "input-driver.rnbopat").read_text())["patcher"]
    indrv = p.obj("p input-driver", 20, 130, 1, 2, patcher=drv_in)
    p.line(midiin, 0, indrv, 0)

    # --- external MIDI notes -> [100, note, velocity, channel]
    notein = p.obj("notein", 200, 90, 2, 4)
    trig = p.obj("t 100 i", 200, 130, 1, 2)
    pack = p.obj("pack 100 i i i", 200, 170, 4, 1)
    p.line(notein, 0, trig, 0)
    p.line(trig, 1, pack, 1)
    p.line(trig, 0, pack, 0)
    p.line(notein, 1, pack, 2)
    p.line(notein, 2, pack, 3)

    # --- clock: 4 systicks (1 ms) per call
    load = p.obj("loadmess 1", 380, 90, 1, 1)
    metro = p.obj("metro 1", 380, 130, 2, 1)
    tick = p.obj("t 4", 380, 170, 1, 1)
    p.line(load, 0, metro, 0)
    p.line(metro, 0, tick, 0)

    # --- sequencer + control surface
    ui = p.codebox("codebox", move_code, 20, 230, 700, 300, 2, N_UI_OUT, [""] * N_UI_OUT, {})
    p.line(indrv, 0, ui, 0)
    p.line(pack, 0, ui, 0)
    p.line(tick, 0, ui, 1)
    leddrv_patcher = json.loads((VENDOR / "led-driver.rnbopat").read_text())["patcher"]
    leddrv = p.obj("p led-driver", 20, 560, 1, 0, patcher=leddrv_patcher)
    p.line(ui, 0, leddrv, 0)
    midiout = p.obj("midiout", 740, 560, 1, 0)
    p.line(ui, 16, midiout, 0)

    # --- engine
    engine = p.codebox("codebox~", engine_code, 20, 760, 1100, 200, 8, 4, ["signal"] * 4,
                       {"safemath": 0})
    for k in range(7):
        s = p.obj("sig~", 160 + 60 * k, 560, 2, 1, ["signal"])
        p.line(ui, 1 + k, s, 0)
        p.line(s, 0, engine, 1 + k)
        a = p.obj("set auto_%s" % tracks[k], 160 + 90 * k, 600, 1, 1, width=88)
        p.line(ui, 8 + k, a, 0)
        p.line(a, 0, engine, 0)

    # audio: both LXR outputs summed to Move's stereo out
    out1 = p.obj("out~ 1", 20, 1000, 1, 0)
    out2 = p.obj("out~ 2", 100, 1000, 1, 0)
    p.line(engine, 0, out1, 0)
    p.line(engine, 2, out1, 0)
    p.line(engine, 1, out2, 0)
    p.line(engine, 3, out2, 0)

    # --- parameters: [param] -> [set] -> engine, and -> [index, value] -> Move codebox;
    # the codebox sets them through [route]
    params = all_params(info)
    names = [q["name"] for q, _i, _a in params]
    assert len(set(names)) == len(names)
    indices = [i for _q, i, _a in params]
    assert len(set(indices)) == len(indices)
    route = p.obj("route " + " ".join(str(i) for i in indices), 740, 600, 1, len(indices) + 1,
                  width=400)
    p.line(ui, 15, route, 0)
    cols = 6
    for k, (q, idx, attr) in enumerate(params):
        x = 1200 + 330 * (k % cols)
        y = 90 + 110 * (k // cols)
        pid = p.obj(param_text(q), x, y, 2, 2, width=320, varname=q["name"])
        p.line(route, k, pid, 0)
        if attr:
            sid = p.obj("set %s" % attr, x, y + 26, 1, 1, width=200)
            p.line(pid, 0, sid, 0)
            p.line(sid, 0, engine, 0)
        t = p.obj("t 101 f", x, y + 52, 1, 2, width=60)
        pk = p.obj("pack 101 %d 0." % idx, x + 70, y + 78, 3, 1, width=120)
        p.line(pid, 0, t, 0)
        p.line(t, 1, pk, 2)
        p.line(t, 0, pk, 0)
        p.line(pk, 0, ui, 0)
    return p


def check(patcher, engine_code):
    """Connections stay within each object's inlets/outlets and every [set] names an
    engine @param."""
    import re
    params = set(re.findall(r"^@param\(\{[^}]*\}\) (\w+) =", engine_code, re.M))
    boxes = {b["box"]["id"]: b["box"] for b in patcher["boxes"]}
    for line in patcher["lines"]:
        src, outlet = line["patchline"]["source"]
        dst, inlet = line["patchline"]["destination"]
        assert outlet < boxes[src]["numoutlets"], (boxes[src].get("text"), outlet)
        assert inlet < boxes[dst]["numinlets"], (boxes[dst].get("text"), inlet)
    sets = [b["text"].split()[1] for b in boxes.values() if b.get("text", "").startswith("set ")]
    assert set(sets) == params, set(sets) ^ params


def build():
    info = json.loads((GEN / "params.json").read_text())
    engine_code = (GEN / "lxr_engine.codebox.js").read_text()
    move_code = (GEN / "lxr_move.codebox.js").read_text()
    inner = build_rnbo_patcher(info, engine_code, move_code)
    check(inner.to_dict(), engine_code)

    top = Patcher("box", [100.0, 100.0, 640.0, 480.0])
    top.comment("LXR drum machine for Ableton Move. Open the rnbo~ object, then export with "
                "the Move target (see rnbo/README.md). The ezdac~ is for listening on the "
                "computer.", 20, 10, 560)
    rnbo = top.obj("rnbo~ @title %s" % TITLE, 20, 80, 2, 3, ["signal", "signal", "list"],
                   width=160,
                   inletInfo={"IOInfo": [{"type": "midi", "index": -1, "tag": "", "comment": ""}]},
                   outletInfo={"IOInfo": [
                       {"type": "signal", "index": 1, "tag": "out1", "comment": ""},
                       {"type": "signal", "index": 2, "tag": "out2", "comment": ""}]},
                   saved_object_attributes={"optimization": "O1", "parameter_enable": 1},
                   varname="rnbo~",
                   patcher=inner.to_dict())
    dac = top.obj("ezdac~", 20, 160, 2, 0, width=45)
    top.line(rnbo, 0, dac, 0)
    top.line(rnbo, 1, dac, 1)
    OUT.write_text(json.dumps({"patcher": top.to_dict()}, indent=1) + "\n")
    print("%s: %d boxes in the rnbo~ patcher" % (OUT.relative_to(ROOT.parent), len(inner.boxes)))


if __name__ == "__main__":
    build()
