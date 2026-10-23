#!/usr/bin/env python3
"""Generate the Max patch for the LXR on Ableton Move.

    python3 rnbo/tools/build.py      (first: engine code and params.json)
    python3 rnbo/tools/patch.py

writes rnbo/patchers/lxr-move.maxpat: an [rnbo~] whose patcher holds

    [midiin] -> Move input driver --------------------\\
    [notein] (external MIDI) -> [pack 100 note vel ch] -> UI codebox -> Move LED driver
                                                         |  |  |
                     triggers, run/mutes: [sig~] --------/  |  |
                     pattern edits: [param seq_*], [param bpm]  |
    [param <lxr parameter>] -> [set <name>] -> engine codebox~ <-/
                                               |       \\-> [snapshot~ 10] x 18 -> UI
                                               \\-> [out~ 1] [out~ 2]

Every LXR parameter is an RNBO [param] (so it is in presets and parameter views);
the sequencer pattern is too, which is why the UI edits it through the seq_* params.
The two Move drivers are the unmodified Cycling '74 templates in rnbo/patchers/vendor.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "generated"
VENDOR = ROOT / "patchers" / "vendor"
OUT = ROOT / "patchers" / "lxr-move.maxpat"

TITLE = "LXR"
SNAPSHOT_MS = 10           # must match TICK_MS in lxr_move_ui.cb.js
N_STATUS = 18              # engine out5..out22 -> UI in2..in19
N_UI_OUT = 31

APPVERSION = {"major": 9, "minor": 1, "revision": 3, "architecture": "x64", "modernui": 1}

VOICE_NAMES = {"d1": "Drum1", "d2": "Drum2", "d3": "Drum3", "sn": "Snare", "cy": "Cymbal",
               "hh": "Hihat", "ch": "Closed Hihat", "oh": "Open Hihat", "fx": "FX", "all": "All",
               "seq": "Seq"}


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
    params = [dict(p) for p in info["params"]]
    params.append({"name": "bpm", "min": 20, "max": 300, "default": 120})
    params += [dict(p) for p in info["seq_params"]]
    return params


def build_rnbo_patcher(info, engine_code, ui_code):
    p = Patcher("rnbo", [100.0, 100.0, 1600.0, 1000.0], TITLE)
    tracks = info["seq_tracks"]

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

    # --- control surface
    ui = p.codebox("codebox", ui_code, 20, 230, 700, 300, 1 + N_STATUS, N_UI_OUT,
                   [""] * N_UI_OUT, {})
    p.line(indrv, 0, ui, 0)
    p.line(pack, 0, ui, 0)
    leddrv_patcher = json.loads((VENDOR / "led-driver.rnbopat").read_text())["patcher"]
    leddrv = p.obj("p led-driver", 20, 560, 1, 0, patcher=leddrv_patcher)
    p.line(ui, 0, leddrv, 0)

    # --- engine
    engine = p.codebox("codebox~", engine_code, 20, 760, 1100, 200, 8, 22, ["signal"] * 22,
                       {"safemath": 0})
    # triggers and run/mute control as signals
    for k in range(7):
        s = p.obj("sig~", 160 + 60 * k, 560, 2, 1, ["signal"])
        p.line(ui, 1 + k, s, 0)
        p.line(s, 0, engine, 1 + k)
    ctl = p.obj("sig~", 160 + 60 * 7, 560, 2, 1, ["signal"])
    p.line(ui, 8, ctl, 0)
    p.line(ctl, 0, engine, 0)

    # status back to the UI
    for k in range(N_STATUS):
        snap = p.obj("snapshot~ %d" % SNAPSHOT_MS, 20 + 62 * k, 1000, 2, 1, width=58)
        p.line(engine, 4 + k, snap, 0)
        p.line(snap, 0, ui, 1 + k)

    # audio: both LXR outputs summed to Move's stereo out
    out1 = p.obj("out~ 1", 20, 1060, 1, 0)
    out2 = p.obj("out~ 2", 100, 1060, 1, 0)
    p.line(engine, 0, out1, 0)
    p.line(engine, 2, out1, 0)
    p.line(engine, 1, out2, 0)
    p.line(engine, 3, out2, 0)

    # --- parameters: [param] -> [set] -> engine
    params = all_params(info)
    names = [q["name"] for q in params]
    assert len(set(names)) == len(names)
    ui_targets = ["seq_%s_steps" % t for t in tracks] + ["seq_%s_accents" % t for t in tracks] \
        + ["seq_%s_length" % t for t in tracks] + ["bpm"]
    assert len(ui_targets) == N_UI_OUT - 9
    param_ids = {}
    cols = 6
    for i, q in enumerate(params):
        x = 1200 + 330 * (i % cols)
        y = 90 + 60 * (i // cols)
        text = param_text(q)
        pid = p.obj(text, x, y, 2, 2, width=320, varname=q["name"])
        sid = p.obj("set %s" % q["name"], x, y + 26, 1, 1, width=200)
        p.line(pid, 0, sid, 0)
        p.line(sid, 0, engine, 0)
        param_ids[q["name"]] = pid
    for k, name in enumerate(ui_targets):
        p.line(ui, 9 + k, param_ids[name], 0)
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
    ui_code = (ROOT / "src" / "lxr_move_ui.cb.js").read_text()
    inner = build_rnbo_patcher(info, engine_code, ui_code)
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
