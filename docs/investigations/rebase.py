"""Re-derive the addresses the mod needs from a new CrimsonDesert.exe.

Usage: py -3 rebase.py

The game patched itself from 1.0.0.2760 to 1.0.0.2850 overnight on
11 September 2026 and every RVA in signatures.h went stale at once. The
runtime checks refused to hook anything, which is what they are for, and
this script is the other half: it finds each address again by something
that survives a rebuild.

  vtables       by RTTI class name (TypeDescriptor -> locator -> vtable)
  the ray cast  by the byte pattern of the wrapper's own prologue, then the
                facade, the world pointer and the frame offset are decoded
                from the RIP-relative operands inside it
  castRay       as the slot of hknpWorld's vtable that the wrapper's
                'call [rax+N]' names

It prints a block ready to paste into signatures.h and the exe version it
was read from.
"""
import re
import struct
import sys

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IB = 0x140000000
d = open(EXE, "rb").read()


def sections(dd):
    pe = struct.unpack_from("<I", dd, 0x3C)[0]
    nsec = struct.unpack_from("<H", dd, pe + 6)[0]
    opt = struct.unpack_from("<H", dd, pe + 20)[0]
    tbl = pe + 24 + opt
    out = []
    for i in range(nsec):
        e = tbl + i * 40
        name = dd[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", dd, e + 8)
        out.append((name, vaddr, vsize, raddr, rsize))
    return out


SECS = sections(d)


def o2r(off):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr)
    return None


def r2o(rva):
    for name, vaddr, vsize, raddr, rsize in SECS:
        if vaddr <= rva < vaddr + rsize:
            return raddr + (rva - vaddr)
    return None


def qword_at(rva):
    o = r2o(rva)
    return struct.unpack_from("<Q", d, o)[0] if o is not None else None


def find_all_qword(value):
    pat = struct.pack("<Q", value)
    return [o2r(m.start()) for m in re.finditer(re.escape(pat), d) if o2r(m.start()) is not None]


def version():
    # VS_FIXEDFILEINFO: signature 0xFEEF04BD then struct version, then file version ms/ls
    i = d.find(b"\xbd\x04\xef\xfe")
    if i < 0:
        return "?"
    ms, ls = struct.unpack_from("<II", d, i + 8)
    return "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


def vtable_by_name(decorated):
    """The primary vtable of a class, from its decorated RTTI name."""
    name = decorated.encode("ascii")
    hits = [m.start() for m in re.finditer(re.escape(name) + b"\x00", d)]
    for h in hits:
        # TypeDescriptor: vtable ptr, spare, name at +0x10
        td_off = h - 0x10
        td_rva = o2r(td_off)
        if td_rva is None:
            continue
        # COLs referencing this TD carry its RVA at +0x0C; find COLs by scanning for
        # the dword, then checking the self-RVA field at +0x14
        pat = struct.pack("<I", td_rva)
        for m in re.finditer(re.escape(pat), d):
            col_off = m.start() - 0x0C
            col_rva = o2r(col_off)
            if col_rva is None or col_off < 0:
                continue
            sig, offset, cd, tdr, chr_, self_rva = struct.unpack_from("<IIIIII", d, col_off)
            if sig != 1 or self_rva != col_rva or tdr != td_rva:
                continue
            if offset != 0:
                continue   # a secondary vtable; the primary has offset 0
            # the vtable is the qword right after the pointer to this COL
            for slot_minus1 in find_all_qword(IB + col_rva):
                return slot_minus1 + 8
    return None


def wrapper_by_pattern():
    """The ray cast wrapper: its prologue, position independent up to the first
    RIP-relative operand."""
    pat = (b"\x48\x8B\xC4"                  # mov rax, rsp
           b"\x48\x89\x58\x08"              # mov [rax+8], rbx
           b"\x48\x89\x70\x10"              # mov [rax+0x10], rsi
           b"\x48\x89\x78\x18"              # mov [rax+0x18], rdi
           b"\x4C\x89\x60\x20"              # mov [rax+0x20], r12
           b"\x55\x41\x56\x41\x57"          # push rbp; push r14; push r15
           b"\x48\x81\xEC\x00\x02\x00\x00"  # sub rsp, 0x200
           )
    hits = []
    for m in re.finditer(re.escape(pat), d):
        rva = o2r(m.start())
        if rva is None:
            continue
        body = d[m.start():m.start() + 0x400]
        # the wrapper computes end = start + dir*dist with vmulss/vaddss on [rdi]/[r9]
        # and reads its stack args at rsp+0x240/+0x248
        if b"\x8B\xBC\x24\x40\x02\x00\x00" in body and b"\xF3\x44\x0F\x10\x84\x24\x48\x02\x00\x00" in body[:0x80] or \
           b"\xC5\x7A\x10\x84\x24\x48\x02\x00\x00" in body[:0x80]:
            hits.append(rva)
    return hits


def rip_targets(func_rva, length=0x400):
    """Every RIP-relative operand in the function, as (instruction rva, target rva)."""
    import capstone
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    cs.detail = True
    out = []
    code = d[r2o(func_rva):r2o(func_rva) + length]
    for ins in cs.disasm(code, IB + func_rva):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                out.append((ins.address - IB, ins.address + ins.size + op.mem.disp - IB, ins.mnemonic, ins.op_str))
        if ins.mnemonic == "ret":
            break
    return out


def main():
    print("exe version %s, %d bytes" % (version(), len(d)))
    classes = {
        "kWorldMapVtable": ".?AVUIGamePlayControlRootWorldMap@uiCommonScript@pa@@",
        "kMiniMapVtable": ".?AVUIGamePlayControlRootMiniMap@uiCommonScript@pa@@",
        "kCameraTPSVtable": ".?AVPlayerCameraTPSMode@gameClientScript@pa@@",
        "kGimmickVtable": ".?AVClientGimmickActorComponent@pa@@",
        "kHknpWorldVtable": ".?AVhknpWorld@@",
    }
    vt = {}
    for k, name in classes.items():
        v = vtable_by_name(name)
        vt[k] = v
        print("%-20s = 0x%08X   // %s" % (k, v or 0, name))
    if vt.get("kCameraTPSVtable"):
        slot2 = qword_at(vt["kCameraTPSVtable"] + 2 * 8)
        print("kCameraTPSUpdate     = 0x%08X   // slot 2 of the TPS vtable" % (slot2 - IB))
    hits = wrapper_by_pattern()
    print("ray cast wrapper candidates: %s" % ", ".join("0x%X" % h for h in hits))
    for h in hits[:2]:
        print("  wrapper 0x%X RIP-relative operands:" % h)
        for insn, tgt, mn, ops in rip_targets(h):
            print("    %08X  %-8s %-40s -> 0x%08X" % (insn, mn, ops[:40], tgt))
    if vt.get("kHknpWorldVtable"):
        for s in (3, 4, 61):
            print("hknpWorld slot %d -> 0x%08X" % (s, (qword_at(vt["kHknpWorldVtable"] + s * 8) or IB) - IB))


if __name__ == "__main__":
    main()
