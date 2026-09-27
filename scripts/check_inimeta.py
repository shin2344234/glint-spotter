"""Hold the INI Master metadata up against the code that reads the ini.

mod/data/GlintSpotter.inimeta is compiled into the plugin as the INIMETA
resource (mod/src/resources.rc), and INI Master shows people what it says: the
type of each key, its default, its range and its help. None of that is read
from the code, so a new key, a changed default or a new clamp has to be
copied across by hand, and a copy that is wrong does not fail anywhere. This
is the check that it was copied.

gs::Settings::Load in mod/src/core/settings.cpp is the authority. It is one
long if / else _stricmp(key, "...") chain; each branch either sets a member of
gs::Settings::Values or logs and keeps the old value. For every key with a
branch that sets a member:

  - it must be described in the metadata, with the type the branch's
    conversion implies: atoi/_stricmp-as-bool is bool, atof is float, atol or
    strtoul is int, sscanf_s of two numbers is string (there is no dedicated
    pair type), KeyFromName is key, and a strncpy_s of the raw value is
    string (or enum, which the reader cannot tell apart from string).
  - a float or int branch's clamp, read out of the `if (r >= lo && r <= hi)`
    guard (allowing an `r == 0.0f ||` alternative before it, which the
    metadata still expresses as min 0), must match the metadata's min and
    max.
  - the default is the member's initialiser in settings.h, formatted the way
    the ini would print it: 1/0 for bool, the plain number for int/float, the
    literal text for string. Key=F9 is checked by name.

Two keys are read but set nothing: "Cone" only logs that it is gone. It is
deliberately left out of the metadata and this script does not expect it.
Any other key the metadata describes must be one the code reads.

Exits non-zero on any mismatch, so it can gate a build.

    py -3 scripts/check_inimeta.py
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
META = os.path.join(ROOT, "mod", "data", "GlintSpotter.inimeta")
SETTINGS_CPP = os.path.join(ROOT, "mod", "src", "core", "settings.cpp")
SETTINGS_H = os.path.join(ROOT, "mod", "src", "core", "settings.h")

# Keys the code reads but that set nothing (a retired key kept only so an old
# ini does not spam "unknown key").
NO_OP_KEYS = {"Cone"}

# Keys with a member default that cannot be read out of settings.h textually
# (a computed or non-trivial literal) go here as key -> ini-form default.
KEY_NAME_DEFAULTS = {
    "Key": "F9",       # g_values.key = 0x78, which is VK_F9
    "Chord": "RB+LB+A",  # g_values.chord = 0x1300 (RB | LB | A)
    "PinStyle": "1,4",   # g_values.pinStyle1 = 1, g_values.pinStyle2 = 4
}


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def body(src, head):
    """The brace-balanced body of the first function whose signature starts with head."""
    i = src.index(head)
    i = src.index("{", i)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise ValueError(head)


def parse_branches(cpp):
    """key -> (member, kind, lo, hi) for every branch that sets a g_values member."""
    src = body(cpp, "const Values& Load(void* selfModule)")
    out = {}
    for m in re.finditer(
        r'_stricmp\(key,\s*"(\w+)"\)\s*==\s*0\)\s*\{(.*?)\n\s{12}\}',
        src, re.S,
    ):
        key, rest = m.group(1), m.group(2)
        if key in NO_OP_KEYS:
            continue

        sm = re.search(r"strncpy_s\(g_values\.(\w+),", rest)
        if sm:
            out[key] = (sm.group(1), "string", None, None)
            continue

        mm = re.search(r"g_values\.(\w+)\s*=\s*(.*?);", rest)
        if not mm:
            if "GS_LOG" in rest and key not in NO_OP_KEYS:
                # Cone-shaped: recognised, but sets nothing.
                continue
            raise ValueError("no assignment found for key %s" % key)
        member, expr = mm.group(1), mm.group(2)

        def guard_numbers(pattern):
            """Numbers in the `if (...)` guard directly in front of this
            assignment. Dot (not [^)]) so a nested paren, as in the
            `r == 0.0f || (r >= lo && r <= hi)` shape, does not truncate the
            match early: the search only stops at the ')' immediately
            followed by this member's assignment."""
            g = re.search(
                r"if\s*\((.*?)\)\s*g_values\." + member + r"\s*=",
                rest, re.S,
            )
            cond = g.group(1) if g else ""
            return cond, [float(x) for x in re.findall(pattern, cond)]

        lo = hi = None
        if key == "PinStyle":
            kind = "string"
        elif key == "Key":
            kind = "key"
        elif "ChordBits" in rest:
            kind = "string"
        elif "atof(val)" in rest:
            kind = "float"
            cond, nums = guard_numbers(r"-?\d+\.?\d*")
            # A guard of the shape `r == 0.0f || (r >= LO && r <= HI)` still
            # accepts 0, so the metadata's min is 0 either way.
            if "==" in cond and "0.0f" in cond:
                lo = 0.0
                hi = nums[-1] if nums else None
            elif nums:
                lo, hi = nums[0], nums[-1]
        elif expr == "atoi(val) != 0":
            kind = "bool"
        elif "atoi(val)" in rest:
            kind = "int"
        elif "atol(val)" in rest:
            kind = "int"
            _, nums = guard_numbers(r"-?\d+")
            if len(nums) >= 2:
                lo, hi = int(nums[0]), int(nums[1])
        elif "strncpy_s" in rest:
            kind = "string"
        else:
            raise ValueError("unrecognised reader for %s: %s" % (key, expr))
        out[key] = (member, kind, lo, hi)
    return out


def parse_defaults(h):
    """member -> default as the ini would write it, from the Values struct."""
    src = body(h, "struct Values")
    out = {}
    for m in re.finditer(
        r"^\s*(uint32_t|uint16_t|uint8_t|bool|int|float|char)\s+(\w+)(?:\[\d+\])?\s*=\s*([^;]+);",
        src, re.M,
    ):
        typ, name, val = m.group(1), m.group(2), m.group(3).strip()
        if typ == "bool":
            out[name] = "1" if val == "true" else "0"
        elif typ == "float":
            out[name] = float(val.rstrip("f"))
        elif typ in ("uint32_t", "uint16_t", "uint8_t", "int"):
            if val.startswith("0x"):
                out[name] = str(int(val, 16))
            else:
                out[name] = str(int(re.match(r"-?\d+", val).group(0)))
        elif typ == "char":
            # Adjacent string literals split across lines, C++ style:
            # "abc,"\n "def" is one string. Join the quoted pieces.
            out[name] = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', val))
    return out


def load_meta():
    text = read(META)
    text = re.sub(r"^\s*//.*\n", "", text, flags=re.M)
    return json.loads(text)


def main():
    cpp, h = read(SETTINGS_CPP), read(SETTINGS_H)
    reader = parse_branches(cpp)
    defaults = parse_defaults(h)
    meta = load_meta()
    keys = meta["sections"]["GlintSpotter"]["keys"]
    errors = []

    for k in sorted(set(reader) - set(keys)):
        errors.append("%s: the code reads it and the metadata does not describe it" % k)
    for k in sorted(set(keys) - set(reader)):
        errors.append("%s: in the metadata but Load() never assigns from it" % k)

    for k, spec in keys.items():
        if k not in reader:
            continue
        member, kind, lo, hi = reader[k]
        t = spec.get("type")
        want_types = {"string": ("string", "enum"), "int": ("int", "enum")}.get(kind, (kind,))
        if t not in want_types:
            errors.append("%s: type %s, but the code reads it as %s" % (k, t, kind))

        if k in KEY_NAME_DEFAULTS:
            want = KEY_NAME_DEFAULTS[k]
        else:
            want = defaults.get(member)
        got = spec.get("default")
        if want is None:
            errors.append("%s: no default found for g_values.%s in settings.h" % (k, member))
        elif kind == "float":
            if got is None or abs(float(got) - float(want)) > 1e-6:
                errors.append("%s: default %s, settings.h says %g" % (k, got, want))
        elif str(got) != str(want):
            errors.append("%s: default %s, settings.h says %s" % (k, got, want))

        if lo is not None:
            got_min = spec.get("min")
            if got_min is None or float(got_min) != float(lo):
                errors.append("%s: min %s, the code clamps to %s" % (k, got_min, lo))
        if hi is not None:
            got_max = spec.get("max")
            if got_max is None or float(got_max) != float(hi):
                errors.append("%s: max %s, the code clamps to %s" % (k, got_max, hi))

    for line in errors:
        print("error  " + line)
    print("%d keys in the metadata, %d read by the code, %d errors"
          % (len(keys), len(reader), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
