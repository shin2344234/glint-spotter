"""Re-derive the addresses the mod needs from a new CrimsonDesert.exe.

Usage: py -3 rebase.py [path to CrimsonDesert.exe]

The game patches itself through Steam without notice, and every RVA in
signatures.h goes stale at once. The runtime checks refuse what no longer
matches, which is what they are for, and this script is the other half: it
finds each address again by something that survives a rebuild, compares it
with what signatures.h and savemap.cpp hold, and checks the struct offsets that
have an anchor in the game's own code.

  vtables        by RTTI class name (TypeDescriptor -> locator -> vtable)
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
    if new is None:
        results["failed"] += 1
        print("  FAILED   %-26s %s" % (name, how))
    elif old is None or new != old:
        results["changed"] += 1
        was = ("was 0x%0*X" % (width, old)) if old is not None else "not recorded"
        print("  CHANGED  %-26s 0x%0*X   %s; %s" % (name, width, new, was, how))
    else:
        results["same"] += 1
        print("  same     %-26s 0x%0*X   %s" % (name, width, new, how))


def check(label, ok):
    results["same" if ok else "failed"] += 1
    print("  %-8s %s" % ("ok" if ok else "FAILED", label))


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

    print("\nsavemap.cpp classes")
    for rva, name in re.findall(r'\{(0x[0-9A-Fa-f]+), "(\.\?AV[^"]+)", 0\}', SAVEMAP):
        report(name[4:-5], vtable_by_name(name), int(rva, 16), "RTTI")

    print("\nfunctions")
    wm, cam = vts["kWorldMapVtable"], vts["kCameraTPSVtable"]
    report("kRemoveIconBody", slot(wm, rec("kSlotRemoveIcon")) if wm else None, rec("kRemoveIconBody"),
           "slot %d of the world map" % rec("kSlotRemoveIcon"))
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
    report("kLgsoManagerGlobal", rip(first[0]) if first and first[0].mnemonic == "mov" else None,
           rec("kLgsoManagerGlobal"), "the store in slot 2 of LevelGimmickSceneObjectInfoManager")

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
    if g:
        tgt = rec("kOff_Gimmick_DetectTgt")
        check("gimmick slot 124 writes the detect byte at +0x%X" % tgt,
              tgt in offsets_in(slot(g, 124), r"mov byte ptr \[r\w+ \+ (0x[0-9a-f]+)\], r"))
        check("gimmick slot 7 compares it", tgt in offsets_in(slot(g, 7), r"cmp byte ptr \[r\w+ \+ (0x[0-9a-f]+)\], 0"))
        sub = rec("kOff_Gimmick_Sub")
        first = dis(slot(g, 123), 8)
        check("gimmick slot 123 reads its sub-object at +0x%X" % sub,
              bool(first) and ("+ 0x%x]" % sub) in first[0].op_str)
    cm = vtable_by_name(".?AVServerContentsMiscActorComponent@pa@@")
    if cm:
        load = slot(cm, 10)
        fmap = rec("kOff_FieldMap", SAVEMAP)
        text = " ".join(i.op_str for i in dis(load))
        print("  info     ServerContentsMiscActorComponent slot 10, the save's load routine, is 0x%08X" % load)
        check("it builds the field map at +0x%X" % fmap, ("+ 0x%x]" % fmap) in text)
        check("and steps its records by 0x%X" % rec("kRecordBytes", SAVEMAP),
              (", 0x%x" % rec("kRecordBytes", SAVEMAP)) in text)
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
    print("  The LGSO record stride, the Transform offset and the detect component's lit byte")
    print("  have no single anchor here. A table count far from 18,000 in the log means the stride moved.")

    print("\n%d same, %d changed, %d failed" % (results["same"], results["changed"], results["failed"]))
    return 2 if results["failed"] else 1 if results["changed"] else 0


if __name__ == "__main__":
    sys.exit(main())
