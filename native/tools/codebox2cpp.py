#!/usr/bin/env python3
"""Translate RNBO codebox code into a C++ class.

    python3 native/tools/codebox2cpp.py      (after rnbo/tools/build.py)

writes
    native/generated/LxrEngine.hpp   from rnbo/generated/lxr_engine.codebox.js (codebox~)
    native/generated/LxrSeq.hpp      from rnbo/src/lxr_seq.cb.js (message codebox)

The RNBO sources stay the one place the LXR port is written; the plugins and other
native targets compile the same code. This covers the codebox subset those files use:

    @param({min, max}) name = value;    -> member + kParams[] table
    @state name = number;               -> member
    @state name = new FixedDoubleArray(n);  -> Arr<n>
    @state name = new liststore({...});     -> ListStore
    const NAME = expr;                  -> static constexpr double
    function f(a, b: list): list {...}  -> member function (double / List / void)
    top-level statements                -> perform() (codebox~: one sample)
    calls to functions not defined here -> pure virtual (the host provides them)

Every number is a double, as in codebox, so integer literals become double literals.
The generated code must be compiled without floating-point contraction
(-ffp-contract=off): the firmware's float32 rounding is emulated with double
arithmetic that a fused multiply-add would change.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "native" / "generated"

MATH = {
    "floor": "std::floor", "ceil": "std::ceil", "trunc": "std::trunc", "pow": "std::pow",
    "exp": "std::exp", "log": "std::log", "log2": "std::log2", "log10": "std::log10",
    "sqrt": "std::sqrt", "abs": "std::fabs", "sin": "std::sin", "cos": "std::cos",
    "tan": "std::tan", "tanh": "std::tanh", "atan": "std::atan", "atan2": "std::atan2",
    "isnan": "cbx::isnan", "min": "cbx::min", "max": "cbx::max", "clamp": "cbx::clamp",
    "round": "cbx::round",
}
KEYWORDS = {"if", "for", "while", "return", "switch", "else", "do", "case"}
CPP_RESERVED = {
    "auto", "bool", "char", "class", "default", "delete", "double", "enum", "explicit",
    "extern", "float", "friend", "goto", "inline", "int", "long", "mutable", "namespace",
    "new", "operator", "private", "protected", "public", "register", "short", "signed",
    "sizeof", "static", "struct", "template", "this", "throw", "try", "typedef", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "and", "or", "not", "xor",
}


def strip_comments(src):
    out = []
    i = 0
    while i < len(src):
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = len(src) if j < 0 else j
        elif src.startswith("/*", i):
            i = src.index("*/", i) + 2
        elif src[i] == '"':
            j = src.index('"', i + 1)
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


class Unit:
    def __init__(self):
        self.params = []      # (name, default, min, max)
        self.states = []      # (name, kind, arg)  kind: num / arr / store
        self.consts = []      # (name, expr)
        self.funcs = []       # (name, [(arg, is_list)], ret, body_lines)
        self.body = []


def split(src):
    u = Unit()
    lines = strip_comments(src).split("\n")
    i = 0
    while i < len(lines):
        t = lines[i].strip()
        i += 1
        if not t:
            continue
        m = re.match(r"^@param\((\{.*\})\)\s+(\w+)\s*=\s*([^;]+);$", t)
        if m:
            opts = dict(re.findall(r"(\w+)\s*:\s*(-?[\d.]+)", m.group(1)))
            u.params.append((m.group(2), m.group(3), opts.get("min", "0"), opts.get("max", "1")))
            continue
        m = re.match(r"^@state\s+(\w+)\s*=\s*new\s+Fixed\w*Array\((\d+)\);$", t)
        if m:
            u.states.append((m.group(1), "arr", int(m.group(2))))
            continue
        m = re.match(r"^@state\s+(\w+)\s*=\s*new\s+liststore\((\{.*\})\);$", t)
        if m:
            u.states.append((m.group(1), "store", json.loads(m.group(2))))
            continue
        m = re.match(r"^@state\s+(\w+)(?:\s*:\s*\w+)?\s*=\s*([^;]+);$", t)
        if m:
            u.states.append((m.group(1), "num", m.group(2)))
            continue
        m = re.match(r"^const\s+(\w+)\s*=\s*([^;]+);$", t)
        if m:
            u.consts.append((m.group(1), m.group(2)))
            continue
        m = re.match(r"^function\s+(\w+)\s*\(([^)]*)\)\s*(?::\s*(\w+))?\s*\{$", t)
        if m:
            args = []
            for a in [x.strip() for x in m.group(2).split(",") if x.strip()]:
                n, _, ty = a.partition(":")
                args.append((n.strip(), ty.strip() == "list"))
            depth = 1
            body = []
            while depth > 0:
                line = lines[i]
                i += 1
                depth += line.count("{") - line.count("}")
                if depth > 0:
                    body.append(line)
            u.funcs.append((m.group(1), args, m.group(3), body))
            continue
        u.body.append(lines[i - 1])
    return u


NUM = re.compile(r"(?<![\w.])(\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)(?![\w.])")


def doubles(s):
    """integer literals -> double literals"""
    return NUM.sub(lambda m: m.group(1) if ("." in m.group(1) or "e" in m.group(1).lower())
                   else m.group(1) + ".0", s)


def list_literals(s):
    """[a, b] in value position -> List{a, b}"""
    out = []
    stack = []
    for i, c in enumerate(s):
        if c == "[":
            prev = s[:i].rstrip()
            literal = not prev or not (prev[-1].isalnum() or prev[-1] in "_)]") or re.search(r"\breturn$", prev)
            stack.append(literal)
            out.append("List{" if literal else "[")
        elif c == "]":
            out.append("}" if stack.pop() else "]")
        else:
            out.append(c)
    return "".join(out)


class Translator:
    def __init__(self, unit, name, signal):
        self.u = unit
        self.name = name
        self.signal = signal
        self.defined = {f[0] for f in unit.funcs}
        self.list_funcs = {f[0] for f in unit.funcs if f[2] == "list"}
        self.stores = {s[0] for s in unit.states if s[2] and s[1] == "store"}
        self.externals = {}

    def ident(self, n):
        return n + "_" if n in CPP_RESERVED else n

    def is_list_expr(self, expr, list_vars):
        e = expr.strip()
        if e.startswith("["):
            return True
        m = re.match(r"^(\w+)\s*\(", e)
        if m and m.group(1) in self.list_funcs:
            return True
        if re.match(r"^\w+\.lookup\(", e):
            return True
        return e in list_vars

    def calls(self, code):
        for m in re.finditer(r"(?<![\w.])([A-Za-z_]\w*)\s*\(", code):
            n = m.group(1)
            if n in KEYWORDS or n in MATH or n in self.defined or n in ("samplerate", "List"):
                continue
            # arity from the call site
            depth = 0
            args = 1
            j = m.end()
            empty = True
            while j < len(code):
                c = code[j]
                if c in "([{":
                    depth += 1
                elif c in ")]}":
                    if depth == 0:
                        break
                    depth -= 1
                elif c == "," and depth == 0:
                    args += 1
                elif not c.isspace():
                    empty = False
                j += 1
            self.externals[n] = 0 if empty else args

    def statement(self, line, list_vars):
        # identifiers that are C++ keywords get a trailing underscore
        s = re.sub(r"\b(" + "|".join(sorted(CPP_RESERVED)) + r")\b", r"\1_", line)
        # declarations
        def decl(m):
            name, ty, init = m.group(1), m.group(2), m.group(3)
            if ty == "list" or (init is not None and self.is_list_expr(init, list_vars)):
                list_vars.add(name)
                return "List %s%s" % (self.ident(name), " =" if init is not None else "")
            return "double %s%s" % (self.ident(name), " =" if init is not None else " = 0")
        s = re.sub(r"\blet\s+(\w+)\s*(?::\s*(\w+))?\s*(?:=\s*([^;]*))?(?=;|$)",
                   lambda m: decl(m) + (" " + m.group(3) if m.group(3) is not None else ""), s)
        s = list_literals(s)
        s = s.replace(".length", ".length()")
        s = re.sub(r"(?<![\w.])([A-Za-z_]\w*)\s*\(", lambda m: (MATH.get(m.group(1), m.group(1))) + "(", s)
        s = doubles(s)
        return s

    def function(self, name, args, ret, body):
        code = "\n".join(body)
        self.calls(code)
        list_vars = {a for a, is_list in args if is_list}
        if ret == "list":
            rtype = "List"
        elif re.search(r"\breturn\s+[^;\s]", code):
            rtype = "double"
        else:
            rtype = "void"
        params = ", ".join(("const List& %s" if is_list else "double %s") % self.ident(a) for a, is_list in args)
        out = ["\t%s %s(%s) {" % (rtype, self.ident(name), params)]
        for line in body:
            out.append("\t" + self.statement(line, list_vars))
        if rtype == "double":
            out.append("\t\treturn 0.0;")
        elif rtype == "List":
            out.append("\t\treturn List{};")
        out.append("\t}")
        return out

    def emit(self):
        u = self.u
        lines = [
            "// GENERATED by native/tools/codebox2cpp.py from the RNBO codebox sources.",
            "// Do not edit; edit those and run rnbo/tools/build.py and this script again.",
            "#pragma once",
            '#include "codebox_rt.hpp"',
            "",
            "namespace lxr {",
            "",
            "class %s {" % self.name,
            "public:",
            "\tusing List = cbx::List;",
            "\ttemplate <int N> using Arr = cbx::Arr<N>;",
            "\tusing ListStore = cbx::ListStore;",
            "",
        ]
        # translate functions first so the externals are known
        func_lines = []
        for name, args, ret, body in u.funcs:
            func_lines += self.function(name, args, ret, body)
            func_lines.append("")
        body_code = "\n".join(u.body)
        self.calls(body_code)

        lines.append("\t// ---- parameters (@param)")
        for name, default, lo, hi in u.params:
            lines.append("\tdouble %s = %s;" % (self.ident(name), doubles(default)))
        lines.append("\tstatic constexpr int kNumParams = %d;" % len(u.params))
        lines.append("\tstatic const cbx::ParamInfo* params() {")
        lines.append("\t\tstatic const cbx::ParamInfo p[] = {")
        for name, default, lo, hi in u.params:
            lines.append('\t\t\t{"%s", %s, %s, %s},' % (name, doubles(lo), doubles(hi), doubles(default)))
        if not u.params:
            lines.append('\t\t\t{"", 0.0, 0.0, 0.0},')
        lines.append("\t\t};")
        lines.append("\t\treturn p;")
        lines.append("\t}")
        lines.append("\tdouble* param(int i) {")
        lines.append("\t\tswitch (i) {")
        for k, (name, _d, _l, _h) in enumerate(u.params):
            lines.append("\t\tcase %d: return &%s;" % (k, self.ident(name)))
        lines.append("\t\tdefault: return nullptr;")
        lines.append("\t\t}")
        lines.append("\t}")
        lines.append("")

        if self.signal:
            ins = sorted({int(n) for n in re.findall(r"\bin(\d+)\b", "\n".join(sum((f[3] for f in u.funcs), [])) + body_code)})
            outs = sorted({int(n) for n in re.findall(r"\bout(\d+)\b", body_code)})
            lines.append("\t// ---- signal inlets and outlets of the codebox~")
            lines.append("\tdouble %s;" % ", ".join("in%d = 0" % k for k in range(1, max(ins + [1]) + 1)))
            lines.append("\tdouble %s;" % ", ".join("out%d = 0" % k for k in range(1, max(outs + [1]) + 1)))
            lines.append("")

        lines.append("\t// ---- constants")
        for name, expr in u.consts:
            lines.append("\tstatic constexpr double %s = %s;" % (self.ident(name), doubles(expr)))
        lines.append("")
        lines.append("\t// ---- state (@state)")
        for name, kind, arg in u.states:
            if kind == "arr":
                lines.append("\tArr<%d> %s;" % (arg, self.ident(name)))
            elif kind == "store":
                lines.append("\tListStore %s{%d, %d};" % (self.ident(name), arg["slotcount"], arg["maxlistsize"]))
            else:
                lines.append("\tdouble %s = %s;" % (self.ident(name), doubles(arg)))
        lines.append("")
        lines.append("\tdouble sampleRate = 44100.0;")
        lines.append("\tdouble samplerate() const { return sampleRate; }")
        lines.append("")
        if self.externals:
            lines.append("\t// ---- provided by the code that uses this class")
            for n, k in sorted(self.externals.items()):
                lines.append("\tvirtual void %s(%s) = 0;" % (n, ", ".join("double" for _ in range(k))))
            lines.append("\tvirtual ~%s() = default;" % self.name)
            lines.append("")
        lines.append("\t// ---- functions")
        lines += func_lines
        if self.signal:
            lines.append("\t// one sample (the codebox~ body)")
            lines.append("\tvoid perform() {")
            list_vars = set()
            for line in u.body:
                lines.append("\t\t" + self.statement(line, list_vars).strip())
            lines.append("\t}")
        lines.append("};")
        lines.append("")
        lines.append("} // namespace lxr")
        lines.append("")
        return "\n".join(lines)


def build(src_path, name, signal):
    unit = split(Path(src_path).read_text())
    t = Translator(unit, name, signal)
    out = t.emit()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / (name + ".hpp")).write_text(out)
    return out


def cstr(s):
    return json.dumps(s)


def build_params():
    """LxrParams.hpp: the LXR sound parameters as the plugins present them (names,
    ranges, defaults, enum texts) and the modulation target list."""
    sys.path.insert(0, str(ROOT / "rnbo" / "tools"))
    from patch import display_name
    meta = json.loads((ROOT / "rnbo" / "generated" / "params.json").read_text())
    defs = meta["params"]
    targets = defs[[d["name"] for d in defs].index("d1_lfo_dest")]["enum"]
    lines = [
        "// GENERATED by native/tools/codebox2cpp.py from rnbo/generated/params.json.",
        "#pragma once",
        "",
        "namespace lxr {",
        "",
        "struct SoundParam {",
        "\tint par;              // LXR parameter number (PAR_*)",
        "\tconst char* name;     // also the engine attribute",
        "\tconst char* display;",
        "\tint min, max, def;",
        "\tint enumIndex;        // into kEnums, or -1",
        "};",
        "",
    ]
    enums = []
    for d in defs:
        if d["enum"] and d["enum"] not in enums:
            enums.append(d["enum"])
    for k, e in enumerate(enums):
        lines.append("static const char* const kEnum%d[] = {%s};" % (k, ", ".join(cstr(x) for x in e)))
    lines.append("static const char* const* const kEnums[] = {%s};" % ", ".join("kEnum%d" % k for k in range(len(enums))))
    lines.append("")
    lines.append("static constexpr int kNumSoundParams = %d;" % len(defs))
    lines.append("static const SoundParam kSoundParams[] = {")
    for d in defs:
        e = enums.index(d["enum"]) if d["enum"] else -1
        hi = len(d["enum"]) - 1 if d["enum"] else d["max"]
        lines.append("\t{%d, %s, %s, %d, %d, %d, %d}," % (d["par"], cstr(d["name"]), cstr(display_name(d["name"])),
                                                       d["min"], hi, d["default"], e))
    lines.append("};")
    lines.append("")
    lines.append("// modulation targets (velocity / LFO destinations and parameter locks): name, PAR")
    lines.append("static constexpr int kNumModTargets = %d;" % len(targets))
    lines.append("static const char* const* const kModTargetNames = kEnum%d;" % enums.index(targets))
    lines.append("static const int kModTargetPar[] = {%s};" % ", ".join(str(x) for x in meta["mod_targets"]))
    lines.append("static const char* const kTracks[] = {%s};" % ", ".join(cstr(t) for t in meta["tracks"]))
    lines.append("")
    lines.append("} // namespace lxr")
    lines.append("")
    (OUT / "LxrParams.hpp").write_text("\n".join(lines))


def main():
    a = build(ROOT / "rnbo" / "generated" / "lxr_engine.codebox.js", "LxrEngine", True)
    b = build(ROOT / "rnbo" / "src" / "lxr_seq.cb.js", "LxrSeq", False)
    build_params()
    print("LxrEngine.hpp: %d lines, LxrSeq.hpp: %d lines, LxrParams.hpp" % (a.count("\n"), b.count("\n")))


if __name__ == "__main__":
    sys.exit(main())
