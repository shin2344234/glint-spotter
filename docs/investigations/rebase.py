"""Re-derive the addresses the mod needs from a new CrimsonDesert.exe.

Usage: py -3 rebase.py [path to CrimsonDesert.exe]

The game patches itself through Steam without notice, and every RVA in
signatures.h goes stale at once. The runtime checks refuse what no longer
matches, which is what they are for, and this script is the other half: it
finds each address again by something that survives a rebuild, compares it
with what signatures.h and savemap.cpp hold, and checks the struct offsets that
have an anchor in the game's own code.

  vtables        by RTTI class name (TypeDescriptor -> locator -> vtable)
  slot numbers   by what the function in the slot does, never by the index
                 signatures.h records, because a class that gains slots moves
                 them and leaves a ret 0 stub where they were
  functions      by the vtable slot or the call that reaches them, then held
                 up against the prologue signatures.h records
  the pin calls  from slot 2 of the four TrocTr*PinMarker* message classes:
                 each deserializer calls the function that does the work
  globals        from the one instruction that stores or tests them
  the ray cast   by the wrapper's prologue, then the facade, the frame offset
                 and the castRay slot from its own operands

Every line says same, CHANGED or FAILED. A CHANGED value is the new one to put
in the source. The exit code is 0 when everything matched, 1 when something
changed and 2 when something could not be found at all.

Two values cannot be found this way: the globals that hold the client and
server actor managers. The game fills them at runtime and nothing in the image
writes either one with a plain move (see private/re-rebase-2976.md). After a
patch they come from the first playtest log, and the script says which lines.

Written on 23 September 2026 for exe 1.0.0.2976 out of the script that did that
rebase by hand. Needs capstone and numpy.

On 29 October 2026, for exe 1.0.0.3392, the DLC build, it learned to find the
slot numbers and the struct offsets that 3392 moved. Through 2976 it read the
remover out of slot 171 and checked the gimmick at slots 124, 7 and 123; on
3392 slot 171 is the ret 0 stub and it reported the stub as the remover. Every
rule here picks exactly one candidate on both 2976 and 3392.
"""
import os
import re
import struct
import sys

import capstone
import numpy as np

EXE = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IB = 0x140000000
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "mod", "src", "game"))

d = open(EXE, "rb").read()

# ---------------------------------------------------------------- the image
#
# The section names in this exe are scrambled: code sits in one called .rsrc
# and writable data in one called .rdata. Only the characteristics are trusted.

_pe = struct.unpack_from("<I", d, 0x3C)[0]
_nsec = struct.unpack_from("<H", d, _pe + 6)[0]
_opt = _pe + 24
_optsz = struct.unpack_from("<H", d, _pe + 20)[0]
SECS = []   # (vaddr, vsize, raddr, rsize, characteristics)
for _i in range(_nsec):
    _e = _opt + _optsz + _i * 40
    _vs, _va, _rs, _ra = struct.unpack_from("<IIII", d, _e + 8)
    SECS.append((_va, _vs, _ra, _rs, struct.unpack_from("<I", d, _e + 36)[0]))
CODE = [s for s in SECS if s[4] & 0x20000000]
_exc_rva, _exc_size = struct.unpack_from("<II", d, _opt + 0x70 + 3 * 8)


def r2o(rva):
    for va, vs, ra, rs, ch in SECS:
        if va <= rva < va + rs:
            return ra + (rva - va)
    return None


def o2r(off):
    for va, vs, ra, rs, ch in SECS:
        if ra <= off < ra + rs:
            return va + (off - ra)
    return None


def q(rva):
    o = r2o(rva)
    return struct.unpack_from("<Q", d, o)[0] if o is not None else None


def raw(rva, n):
    o = r2o(rva)
    return d[o:o + n] if o is not None else b""


_pd = np.frombuffer(d, dtype="<u4", count=(_exc_size // 12) * 3, offset=r2o(_exc_rva)).reshape(-1, 3)
_fb = _pd[:, 0].astype(np.int64)
_fe = _pd[:, 1].astype(np.int64)


def func_of(rva):
    """(begin, end) from .pdata, or None for a leaf with no unwind entry."""
    i = int(np.searchsorted(_fb, rva, side="right")) - 1
    if i >= 0 and _fb[i] <= rva < _fe[i]:
        return int(_fb[i]), int(_fe[i])
    return None


def version():
    i = d.find(b"\xbd\x04\xef\xfe")
    ms, ls = struct.unpack_from("<II", d, i + 8)
    return "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


# ---------------------------------------------------------------- RTTI

def find_all_qword(value):
    pat = re.escape(struct.pack("<Q", value))
    return [r for r in (o2r(m.start()) for m in re.finditer(pat, d)) if r is not None]


def vtable_by_name(decorated):
    """The primary vtable of a class, from its decorated RTTI name."""
    name = decorated.encode("ascii")
    for m in re.finditer(re.escape(name) + b"\x00", d):
        td_rva = o2r(m.start() - 0x10)
        if td_rva is None:
            continue
        for mm in re.finditer(re.escape(struct.pack("<I", td_rva)), d):
            col_off = mm.start() - 0x0C
            col_rva = o2r(col_off)
            if col_rva is None or col_off < 0:
                continue
            sig, offset, cd, tdr, chd, self_rva = struct.unpack_from("<IIIIII", d, col_off)
            if sig != 1 or self_rva != col_rva or tdr != td_rva or offset != 0:
                continue
            for s in find_all_qword(IB + col_rva):
                return s + 8
    return None


def class_of(vt_rva):
    """The decorated name behind a vtable, through the locator before it."""
    colp = q(vt_rva - 8)
    if not colp or r2o(colp - IB) is None:
        return None
    sig, off, cd, tdr, chd, self_rva = struct.unpack_from("<IIIIII", d, r2o(colp - IB))
    if sig != 1 or self_rva != colp - IB:
        return None
    o = r2o(tdr + 0x10)
    return d[o:d.index(b"\0", o)].decode("ascii", "replace")


def slot(vt_rva, n):
    x = q(vt_rva + 8 * n)
    return x - IB if x else None


# ---------------------------------------------------------------- code

CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
CS.detail = True


def dis(rva, length=None):
    if length is None:
        f = func_of(rva)
        length = (f[1] - rva) if f else 0x40
    return list(CS.disasm(raw(rva, length), IB + rva))


def rip(ins):
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            return ins.address + ins.size + op.mem.disp - IB
    return None


def calls(rva):
    return [ins.operands[0].imm - IB for ins in dis(rva)
            if ins.mnemonic == "call" and ins.operands[0].type == capstone.x86.X86_OP_IMM]


def xrefs(target):
    """Code positions whose disp32 resolves RIP-relative to target."""
    out = []
    for va, vs, ra, rs, ch in CODE:
        for k in range(4):
            cnt = (rs - k) // 4
            a = np.frombuffer(d, dtype="<i4", count=cnt, offset=ra + k).astype(np.int64)
            pos = va + k + 4 * np.arange(cnt, dtype=np.int64)
            for tail in (0, 1, 4):
                out += [int(p) for p in pos[np.nonzero(pos + 4 + tail + a == target)[0]]]
    return sorted(set(out))


def call_sites(target):
    """Direct E8 calls to target."""
    out = []
    for va, vs, ra, rs, ch in CODE:
        for k in range(4):
            cnt = (rs - k) // 4
            a = np.frombuffer(d, dtype="<i4", count=cnt, offset=ra + k).astype(np.int64)
            pos = va + k + 4 * np.arange(cnt, dtype=np.int64)
            for p in pos[np.nonzero(pos + 4 + a == target)[0]]:
                if d[r2o(int(p)) - 1] == 0xE8:
                    out.append(int(p) - 1)
    return out


CODE_RANGES = [(va, va + vs) for va, vs, ra, rs, ch in CODE]


def slot_count(vt_rva):
    """Slots up to the first entry that does not point into code."""
    n = 0
    while n < 1000:
        x = q(vt_rva + 8 * n)
        if not x or not any(lo <= x - IB < hi for lo, hi in CODE_RANGES):
            break
        n += 1
    return n


def is_stub(rva):
    """The ret 0 the compiler fills unused slots with."""
    return raw(rva, 3) == b"\xc2\x00\x00"


def vcalls(rva):
    """The slot numbers of the virtual calls a function makes, in order."""
    out = []
    for ins in dis(rva):
        if ins.mnemonic not in ("call", "jmp") or not ins.operands:
            continue
        op = ins.operands[0]
        if op.type != capstone.x86.X86_OP_MEM:
            continue
        m = op.mem
        if m.base in (0, capstone.x86.X86_REG_RIP, capstone.x86.X86_REG_RSP) or m.index or m.disp % 8:
            continue
        out.append(m.disp // 8)
    return out


def string_at(rva, n=64):
    b = raw(rva, n)
    return b.split(b"\0")[0] if b else b""


def offsets_in(rva, pattern):
    """Every displacement in the function that an operand matching pattern uses."""
    out = set()
    for ins in dis(rva):
        m = re.search(pattern, ins.mnemonic + " " + ins.op_str)
        if m:
            out.add(int(m.group(1), 16))
    return out


# ---------------------------------------------------------------- the source

SIG = open(os.path.join(SRC, "signatures.h"), encoding="utf-8").read()
SAVEMAP = open(os.path.join(SRC, "savemap.cpp"), encoding="utf-8").read()
PICKUP = open(os.path.join(SRC, "pickup.cpp"), encoding="utf-8").read()


def rec(name, text=None):
    m = re.search(r"\b%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*[;,]" % name, SIG if text is None else text)
    return int(m.group(1), 0) if m else None


def rec_bytes(name):
    m = re.search(r"\b%s\[\d*\]\s*=\s*\{([^}]*)\}" % name, SIG, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return bytes(int(t, 16) for t in re.findall(r"0x[0-9A-Fa-f]+", body))


# ---------------------------------------------------------------- reporting

results = {"same": 0, "changed": 0, "failed": 0}


def report(name, new, old, how, width=8):
    """width 0 prints the values in decimal, for slot numbers."""
    fmt = (lambda v: "%-10d" % v) if width == 0 else (lambda v: "0x%0*X" % (width, v))
    if new is None:
        results["failed"] += 1
        print("  FAILED   %-26s %s" % (name, how))
    elif old is None or new != old:
        results["changed"] += 1
        was = ("was " + fmt(old).strip()) if old is not None else "not recorded"
        print("  CHANGED  %-26s %s   %s; %s" % (name, fmt(new), was, how))
    else:
        results["same"] += 1
        print("  same     %-26s %s   %s" % (name, fmt(new), how))


def check(label, ok):
    results["same" if ok else "failed"] += 1
    print("  %-8s %s" % ("ok" if ok else "FAILED", label))


# ---------------------------------------------------------------- slots by behaviour

# The remover homes its fourth, third and second arguments, a dword, the key and
# the icon type as a word, in that order. Its first instruction on the world map
# in 2976 and 3392 alike; the minimap's remover has it five bytes in on 3392.
REMOVER_HOMES = bytes.fromhex("44894C24204C894424186689542410")


def copies_xmm1_early(rva):
    """The update takes its float in xmm1 and saves it before anything else."""
    return any(i.mnemonic in ("vmovaps", "movaps") and i.op_str.endswith(", xmm1") for i in dis(rva, 0x80))


def map_slots(wm, mm):
    """(update, dispatcher, remover) slot numbers, found on the world map and
    checked on the minimap, or None for each one that is not certain.

    remover     the one slot whose function homes the three arguments above
                in its first 24 bytes
    dispatcher  the slot before it, which calls two virtuals: the slot before
                itself and the slot before the update ([169, 34] on 2976,
                [173, 35] on 3392)
    update      the one slot that saves xmm1 in its first 0x80 bytes and calls
                the slot before itself, and it must be the dispatcher's second
                call plus one
    """
    n = slot_count(wm)
    rem = [i for i in range(n) if not is_stub(slot(wm, i)) and REMOVER_HOMES in raw(slot(wm, i), 24)]
    upd = [i for i in range(n) if not is_stub(slot(wm, i)) and copies_xmm1_early(slot(wm, i))
           and (i - 1) in vcalls(slot(wm, i))]
    print("  info     the world map vtable has %d slots; remover candidates %s, update candidates %s" % (n, rem, upd))
    if len(rem) != 1 or len(upd) != 1:
        return None, None, None
    r, u = rem[0], upd[0]
    dsp = r - 1
    dcalls = vcalls(slot(wm, dsp))
    ok = (dsp - 1) in dcalls and (u - 1) in dcalls and (dsp - 1) in vcalls(slot(wm, r))
    print("  info     slot %d calls %s; the dispatcher calls the slot before itself and the slot before the update%s"
          % (dsp, dcalls, "" if ok else ": NO"))
    if not ok:
        return u, None, r
    if mm:
        mcalls = vcalls(slot(mm, dsp))
        mupd = [i for i in range(slot_count(mm)) if not is_stub(slot(mm, i)) and copies_xmm1_early(slot(mm, i))
                and (i - 1) in vcalls(slot(mm, i))]
        check("the minimap has the same layout: its slot %d calls %s and its update is slot %s" % (dsp, mcalls, mupd),
              (dsp - 1) in mcalls and (u - 1) in mcalls and mupd == [u])
    return u, dsp, r


def alert_slot(vt):
    """The one slot whose function names Toast, BountyHunter and TimerGauge."""
    want = {b"Toast", b"BountyHunter", b"TimerGauge"}
    hits = []
    for i in range(slot_count(vt)):
        f = slot(vt, i)
        if is_stub(f) or not func_of(f):
            continue
        got = {string_at(t) for t in (rip(x) for x in dis(f) if x.mnemonic == "lea") if t}
        if want <= got:
            hits.append(i)
    return hits[0] if len(hits) == 1 else None


def gimmick_offsets(vt):
    """The sub-object pointer and the detect byte, from the gimmick's own slots.

    The sub-object setter is the one slot that is exactly
        mov rax, [rcx + S]; mov byte ptr [rax + Y], 1; ret
    (123 with S 0x440 on 2976, 124 with S 0x450 on 3392). The detect byte is
    the one offset past +0x300 that a slot writes from r15b and a slot compares
    with 0 (124 and 7 on 2976, 125 and 7 on 3392, +0x463 and +0x473).
    """
    n = slot_count(vt)
    setters = []
    written, compared = {}, {}
    for i in range(n):
        f = slot(vt, i)
        if is_stub(f):
            continue
        head = dis(f, 0x18)
        if len(head) >= 3 and head[0].mnemonic == "mov" and head[1].mnemonic == "mov" and head[2].mnemonic == "ret":
            m0 = re.match(r"rax, qword ptr \[rcx \+ (0x[0-9a-f]+)\]$", head[0].op_str)
            m1 = re.match(r"byte ptr \[rax \+ (0x[0-9a-f]+)\], 1$", head[1].op_str)
            if m0 and m1:
                setters.append((i, int(m0.group(1), 16), int(m1.group(1), 16)))
        if not func_of(f):
            continue
        for ins in dis(f):
            m = re.match(r"byte ptr \[r\w+ \+ (0x[0-9a-f]+)\], (r15b|0)$", ins.op_str)
            if not m:
                continue
            off = int(m.group(1), 16)
            if ins.mnemonic == "mov" and m.group(2) == "r15b":
                written.setdefault(off, set()).add(i)
            elif ins.mnemonic == "cmp" and m.group(2) == "0":
                compared.setdefault(off, set()).add(i)
    both = sorted(k for k in written if k in compared and k >= 0x300)
    sub = setters[0][1] if len(setters) == 1 else None
    if len(setters) == 1:
        print("  info     gimmick slot %d sets +0x%X on the sub-object it reads at +0x%X" %
              (setters[0][0], setters[0][2], setters[0][1]))
    else:
        print("  info     %d gimmick slots have the sub-object setter's shape" % len(setters))
    det = both[0] if len(both) == 1 else None
    for k in both:
        print("  info     gimmick slots %s write +0x%X from r15b and slots %s compare it with 0" %
              (sorted(written[k]), k, sorted(compared[k])))
    return sub, det


def reflected_offset(field, near=None):
    """Where the reflection registration of a field name puts it: a lea of
    the name followed within 250 instructions by mov dword ptr [rip+X], imm.
    With near, the registration closest to that address, since common names
    are registered by many classes. Returns (offset, function) or (None, None)."""
    found = []
    pat = b"\0" + field.encode("ascii") + b"\0"
    for m in re.finditer(re.escape(pat), d):
        srva = o2r(m.start() + 1)
        if srva is None:
            continue
        for p in xrefs(srva):
            f = func_of(p)
            if not f:
                continue
            ins = dis(f[0])
            k = next((j for j, i in enumerate(ins) if i.address - IB <= p < i.address - IB + i.size), None)
            if k is None or ins[k].mnemonic != "lea":
                continue
            for n in ins[k:k + 250]:
                mm = re.match(r"dword ptr \[rip \+ 0x[0-9a-f]+\], (0x[0-9a-f]{1,4})$", n.op_str)
                if n.mnemonic == "mov" and mm:
                    found.append((int(mm.group(1), 16), f[0]))
                    break
    if not found:
        return None, None
    if near is None:
        return found[0] if len(set(found)) == 1 else (None, None)
    return min(found, key=lambda t: abs(t[1] - near))


def lgso_layout(glob):
    """The element stride the table's readers multiply by, and every float
    triple they read through a strided element: (stride, votes, triples)."""
    funcs = sorted({f[0] for f in (func_of(p) for p in xrefs(glob)) if f})
    strides = {}
    triples = set()
    for f in funcs:
        ins = dis(f)
        for k, i in enumerate(ins):
            m = re.match(r"(\w+), \w+, (0x[0-9a-f]+)$", i.op_str)
            if i.mnemonic != "imul" or not m or not 0x80 <= int(m.group(2), 16) < 0x200:
                continue
            s = int(m.group(2), 16)
            strides[s] = strides.get(s, 0) + 1
            offs = set()
            for nxt in ins[k:k + 120]:
                m2 = re.match(r"xmm\d+, dword ptr \[%s \+ (0x[0-9a-f]+)\]$" % m.group(1), nxt.op_str)
                if nxt.mnemonic in ("vmovss", "movss") and m2:
                    offs.add(int(m2.group(1), 16))
            triples |= {(s, o) for o in offs if o + 4 in offs and o + 8 in offs}
    if not strides:
        return None, 0, set()
    best = max(strides, key=strides.get)
    return best, strides[best], {o for s, o in triples if s == best}


def main():
    print("exe %s, %d bytes" % (version(), len(d)))
    old_ver = re.search(r'kExeVersion\s*=\s*"([^"]+)"', SIG).group(1)
    print("signatures.h was written against %s%s" % (old_ver, "" if old_ver == version() else
                                                        "; set kExeVersion to \"%s\"" % version()))

    print("\nvtables")
    vts = {}
    for k in ("kWorldMapVtable", "kMiniMapVtable", "kAlertRootVtable", "kCameraTPSVtable", "kGimmickVtable"):
        name = {"kWorldMapVtable": ".?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@",
                "kMiniMapVtable": ".?AVUIGamePlayControlRootMiniMap@uiCommonScript@pa@@",
                "kAlertRootVtable": ".?AVUIGamePlayControlRootAlertSystem@uiCommonScript@pa@@",
                "kCameraTPSVtable": ".?AVPlayerCameraTPSMode@gameClientScript@pa@@",
                "kGimmickVtable": ".?AVClientGimmickActorComponent@pa@@"}[k]
        vts[k] = vtable_by_name(name)
        report(k, vts[k], rec(k), "RTTI " + name)
    pick = ".?AVTrocTrProcessPickUpItemOnceTimer@pa@@"
    report("kPickUpVtableRva", vtable_by_name(pick), rec("kPickUpVtableRva", PICKUP), "RTTI %s (pickup.cpp)" % pick)

    print("\nsavemap.cpp classes")
    for rva, name in re.findall(r'\{(0x[0-9A-Fa-f]+), "(\.\?AV[^"]+)", 0\}', SAVEMAP):
        report(name[4:-5], vtable_by_name(name), int(rva, 16), "RTTI")

    print("\nslot numbers, by what each function does")
    wm, mm, cam = vts["kWorldMapVtable"], vts["kMiniMapVtable"], vts["kCameraTPSVtable"]
    u, dsp, r = map_slots(wm, mm) if wm else (None, None, None)
    report("kSlotUpdate", u, rec("kSlotUpdate"), "saves xmm1 first and calls the slot before it", 0)
    report("kSlotCreateIcon", dsp, rec("kSlotCreateIcon"), "the slot before the remover, calling the two above", 0)
    report("kSlotRemoveIcon", r, rec("kSlotRemoveIcon"), "homes r9d, r8 and dx in that order", 0)
    for k, v in (("kSlotUpdate", u), ("kSlotCreateIcon", dsp), ("kSlotRemoveIcon", r)):
        if wm and v is not None and v != rec(k) and is_stub(slot(wm, rec(k))):
            print("  info     the recorded %s, slot %d, now holds the ret 0 stub" % (k, rec(k)))
    al = vts["kAlertRootVtable"]
    report("kSlotAlertCall", alert_slot(al) if al else None, rec("kSlotAlertCall"),
           "names Toast, BountyHunter and TimerGauge", 0)

    print("\nfunctions")
    report("kRemoveIconBody", slot(wm, r) if wm and r is not None else None, rec("kRemoveIconBody"),
           "slot %s of the world map" % r)
    upd = slot(cam, rec("kSlotCameraUpdate")) if cam else None
    pro, at = rec_bytes("kCameraUpdatePrologue"), rec("kCameraUpdateFieldAt")
    got = raw(upd, len(pro)) if upd else b""
    ok = got[:at] == pro[:at] and got[at + 4:] == pro[at + 4:]
    report("kCameraTPSUpdate", upd if ok else None, rec("kCameraTPSUpdate"),
           "slot 2 of the camera, prologue matches" if ok else "slot 2's prologue no longer matches")
    if ok:
        # The recorded array stops partway into the displacement, which the
        # plugin skips when it compares, so the field is read off the exe.
        field = struct.unpack_from("<I", raw(upd, at + 4), at)[0]
        was = int.from_bytes(pro[at:at + 4], "little")
        print("  info     the update reads its fade weight at +0x%X%s" %
              (field, "" if field == was else ", where kCameraUpdatePrologue has +0x%X" % was))

    pins = [("kPinCreate", "kPinCreatePrologue", ".?AVTrocTrCreateOrChangePinMarkerReq@pa@@"),
            ("kPinServerRemove", "kPinRemovePrologue", ".?AVTrocTrRemovePinMarkerReq@pa@@"),
            ("kPinUpsert", "kPinUpsertPrologue", ".?AVTrocTrCreateOrChangePinMarkerAck@pa@@"),
            ("kPinRemove", "kPinRemovePrologue2", ".?AVTrocTrRemovePinMarkerAck@pa@@")]
    found = {}
    for k, p, cls in pins:
        v = vtable_by_name(cls)
        pb = rec_bytes(p)
        hit = [t for t in calls(slot(v, 2))] if v else []
        hit = [t for t in hit if raw(t, len(pb)) == pb]
        found[k] = hit[0] if len(hit) == 1 else None
        report(k, found[k], rec(k), "the call from %s slot 2 that starts with %s" % (cls[4:-5], p)
               if found[k] else "%d calls from %s slot 2 match %s" % (len(hit), cls[4:-5], p))
    if found["kPinCreate"]:
        acks = [dis(a) for a in [slot(vtable_by_name(".?AVTrocTrCreateOrChangePinMarkerAck@pa@@"), 2)]]
        text = " ".join(i.op_str for i in acks[0])
        for off, k in ((rec("kOff_Actor_Components"), "kOff_Actor_Components"),
                       (rec("kOff_Comp_PinSubmodule"), "kOff_Comp_PinSubmodule"),
                       (rec("kOff_Pin_Lists"), "kOff_Pin_Lists")):
            check("the acknowledgement still reads +0x%X (%s)" % (off, k), ("+ 0x%x]" % off) in text)

    print("\nglobals")
    flag = [rip(i) for i in dis(found["kPinCreate"]) if i.mnemonic == "cmp" and rip(i)] \
        if found["kPinCreate"] else []
    report("kPinOnlineFlag", flag[0] if len(flag) == 1 else None, rec("kPinOnlineFlag"),
           "the create's one cmp byte ptr [rip+...]" if len(flag) == 1 else "%d cmp [rip] in the create" % len(flag))
    lg = vtable_by_name(".?AVLevelGimmickSceneObjectInfoManager@pa@@")
    first = dis(slot(lg, 2), 8) if lg else []
    lgso = rip(first[0]) if first and first[0].mnemonic == "mov" else None
    report("kLgsoManagerGlobal", lgso, rec("kLgsoManagerGlobal"),
           "the store in slot 2 of LevelGimmickSceneObjectInfoManager")
    sm = vtable_by_name(".?AVSpecialModeInfoManager@pa@@")
    first = dis(slot(sm, 2), 8) if sm else []
    report("kSpecialModeManagerGlobal", rip(first[0]) if first and first[0].mnemonic == "mov" else None,
           rec("kSpecialModeManagerGlobal"), "the store in slot 2 of SpecialModeInfoManager")

    print("\nthe ray cast")
    pat = (b"\x48\x8B\xC4\x48\x89\x58\x08\x48\x89\x70\x10\x48\x89\x78\x18\x4C\x89\x60\x20"
           b"\x55\x41\x56\x41\x57\x48\x81\xEC\x00\x02\x00\x00")
    wrappers = []
    for va, vs, ra, rs, ch in CODE:
        for m in re.finditer(re.escape(pat), d[ra:ra + rs]):
            body = d[ra + m.start():ra + m.start() + 0x80]
            if b"\x48\x02\x00\x00" in body:
                wrappers.append(va + m.start())
    w = wrappers[0] if len(wrappers) == 1 else None
    report("kRayCastWrapper", w, rec("kRayCastWrapper"), "%d match(es) of the wrapper's prologue" % len(wrappers))
    facade = frame = None
    castray = False
    if w:
        ins = dis(w)
        for i, n in enumerate(ins):
            t = rip(n)
            if facade is None and n.mnemonic == "mov" and t and i + 1 < len(ins) and \
                    ins[i + 1].mnemonic == "lea" and rip(ins[i + 1]) == t:
                facade = t
            if frame is None and n.mnemonic == "vsubps" and t:
                frame = t
            if n.mnemonic == "call" and ("+ 0x%x]" % (rec("kSlotCastRay") * 8)) in n.op_str:
                castray = True
    report("kPhysicsFacade", facade, rec("kPhysicsFacade"), "mov then lea on one [rip+...] in the wrapper")
    report("kPhysicsFrameOff", frame, rec("kPhysicsFrameOff"), "the wrapper's vsubps [rip+...]")
    check("the wrapper still calls castRay at slot %d" % rec("kSlotCastRay"), castray)

    print("\nstruct offsets with an anchor in the code")
    g = vts["kGimmickVtable"]
    sub, det = gimmick_offsets(g) if g else (None, None)
    report("kOff_Gimmick_Sub", sub, rec("kOff_Gimmick_Sub"), "the gimmick slot that only sets a byte on it", 3)
    report("kOff_Gimmick_DetectTgt", det, rec("kOff_Gimmick_DetectTgt"),
           "the one byte past +0x300 written from r15b and compared with 0", 3)
    cm = vtable_by_name(".?AVServerContentsMiscActorComponent@pa@@")
    if cm:
        load = slot(cm, 10)
        fmap = rec("kOff_FieldMap", SAVEMAP)
        text = " ".join(i.op_str for i in dis(load))
        print("  info     ServerContentsMiscActorComponent slot 10, the save's load routine, is 0x%08X" % load)
        check("it builds the field map at +0x%X" % fmap, ("+ 0x%x]" % fmap) in text)
        strides = {}
        for i in dis(load):
            m = re.match(r"\w+, (0x[0-9a-f]+)$", i.op_str)
            if i.mnemonic in ("add", "imul") and m and 0x300 <= int(m.group(1), 16) < 0x500:
                strides[int(m.group(1), 16)] = strides.get(int(m.group(1), 16), 0) + 1
        best = max(strides, key=strides.get) if strides else None
        report("kRecordBytes", best, rec("kRecordBytes", SAVEMAP),
               "the load routine's commonest step, %s" % ", ".join("0x%X x%d" % kv for kv in sorted(strides.items())), 3)
    # The record's fields, from the reflection registrations. Each registers
    # one field name and stores its offset. The position the mod reads is
    # +0x1C into each transform on 2976, which this cannot see.
    st, near = reflected_offset("_initStateNameHash")
    report("kOff_State", st, rec("kOff_State", SAVEMAP), "reflection of _initStateNameHash", 3)
    if near:
        org, _ = reflected_offset("_originSpawnTransform", near)
        tf, _ = reflected_offset("_transform", near)
        report("kOff_OriginPos", org + 0x1C if org is not None else None, rec("kOff_OriginPos", SAVEMAP),
               "reflection of _originSpawnTransform nearest it, plus 0x1C", 3)
        report("kOff_Pos", tf + 0x1C if tf is not None else None, rec("kOff_Pos", SAVEMAP),
               "reflection of _transform nearest it, plus 0x1C", 3)
    if lgso:
        stride, votes, trips = lgso_layout(lgso)
        report("kOff_LgsoData_Stride", stride, rec("kOff_LgsoData_Stride"),
               "what %d of the table's readers multiply an element index by" % votes, 3)
        pos = rec("kOff_LgsoData_Transform") + rec("kOff_Transform_Pos")
        check("a reader of the table reads a position at +0x%X (kOff_LgsoData_Transform + 0x%X); triples read: %s" %
              (pos, rec("kOff_Transform_Pos"), ", ".join("+0x%X" % t for t in sorted(trips))), pos in trips)
    if wm:
        size = None
        for p in xrefs(wm):
            f = func_of(p)
            for c in call_sites(f[0]) if f else []:
                # From the caller's start, so the instructions decode aligned;
                # on 2976 the size is set 0x46 bytes before the call.
                cf = func_of(c)
                start = cf[0] if cf else c - 0x80
                for ins in dis(start, c - start)[-40:]:
                    m = re.match(r"ecx, (0x[0-9a-f]+)$", ins.op_str)
                    if ins.mnemonic == "mov" and m and 0x800 <= int(m.group(1), 16) < 0x4000:
                        size = int(m.group(1), 16)
        report("kRootControlSize", size, rec("kRootControlSize"), "what the root control's factory allocates", 3)

    print("\nnot derivable from the exe")
    print("  kActorManagerGlobals (signatures.h) and kServerManagerGlobal (savemap.cpp) are filled")
    print("  at runtime. Ship a build, load a save, and copy the values from these log lines:")
    print('    [actors] manager 0x... via global +0x...')
    print('    [savemap] the ServerActorManager is held at global +0x..., not the recorded ...')
    print("  The detect component's lit byte, the gimmick sub-object's active count and the camera's")
    print("  pivot, quaternion and world copies have no anchor here. A table count far from 18,000")
    print("  in the log means the table layout above is wrong after all.")
    print("  kSpecialModeRows is data, not code: read gamedata/specialmode.staticinfoheader out of")
    print("  package 0008 with Master Looter's cdtables.load_table(\"specialmode\") and compare its rows")
    print("  and their order with kModes in aim.cpp (%d rows recorded)." % rec("kSpecialModeRows"))

    print("\n%d same, %d changed, %d failed" % (results["same"], results["changed"], results["failed"]))
    return 2 if results["failed"] else 1 if results["changed"] else 0


if __name__ == "__main__":
    sys.exit(main())
