"""Read the LXR parameter enum (Parameters.h) and the factory init kit."""
import re
from pathlib import Path

CORE = Path(__file__).resolve().parents[2] / "Core"


def _enum_body(text, enum_name):
    m = re.search(r"enum\s+" + enum_name + r"\s*\{(.*?)\};", text, re.S)
    return m.group(1)


def par_enum():
    """Map PAR_* name -> index, following C enum rules (aliases included)."""
    text = (CORE / "Inc/Parameters.h").read_text()
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    body = _enum_body(text, "ParamEnums")
    names = {}
    nxt = 0
    for item in [x.strip() for x in body.split(",") if x.strip()]:
        if "=" in item:
            n, v = [x.strip() for x in item.split("=")]
            val = names[v] if v in names else int(v, 0)
        else:
            n, val = item, nxt
        names[n] = val
        nxt = val + 1
    return names


def init_kit():
    """activeParameters[] after preset_init(): para[i-8] = initKit[i]."""
    text = (CORE / "Src/Preset/initKit.c").read_text()
    body = text[text.index("{", text.index("initKit[")) + 1:]
    vals = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]
    end = par_enum()["END_OF_SOUND_PARAMETERS"]
    return [vals[i] for i in range(8, end)] + [0] * 8


if __name__ == "__main__":
    e = par_enum()
    kit = init_kit()
    inv = {}
    for k, v in e.items():
        inv.setdefault(v, []).append(k)
    for i in range(e["END_OF_SOUND_PARAMETERS"]):
        print(i, "/".join(inv.get(i, ["?"])), kit[i] if i < len(kit) else None)


def _strip_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def parameter_order():
    """Order in which preset_sendDrumsetParameters() sends a kit to the engine."""
    e = par_enum()
    text = _strip_comments((CORE / "Src/Parameters.c").read_text())
    body = text[text.index("{", text.index("parameterOrder")) + 1:text.index("};")]
    return [e[n.strip()] for n in body.split(",") if n.strip()]


def mod_targets():
    """modTargets[] from Cc2Text.c as a list of (TEXT_* name, PAR index)."""
    e = par_enum()
    text = _strip_comments((CORE / "Src/menu/Cc2Text.c").read_text())
    body = text[text.index("{", re.search(r"modTargets\s*\[\]", text).end()) + 1:]
    body = body[:body.index("};")]
    return [(t, e[p]) for t, p in re.findall(r"\{\s*(\w+)\s*,\s*(\w+)\s*\}", body)]
