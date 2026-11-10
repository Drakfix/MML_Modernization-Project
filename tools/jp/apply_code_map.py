# Run inside the Ghidra server on the Japanese project (python tools/ghidra/gq.py run ../jp/apply_code_map.py),
# after fixups.py and realign.py: decode the code the Japanese program shares with the US one as the
# repaired US project does (native/build/us_code_map.txt, written by export_code_map.py on the US
# project). A US instruction start is taken over where the runs of tools/jp/addr_map.py put it and
# the Japanese bytes there decode to an instruction of the same length and opcode (inside a run the
# Japanese code can still differ: a short jump that became a near one). Japanese instructions that
# overlap a taken-over one without starting with it are cleared (data decoded as code, or code
# decoded a byte off), and functions are made where the US project has them. Everything else is
# left as fixups.py found it. MML_US_EXE: the US MEGAMAN.EXE.orig (default: MML_US_GAME_DIR's).
import os
import sys

sys.path.insert(0, os.path.join(HERE, "..", "jp"))
from addr_map import _SEGS   # (US start, US end, delta) runs

import capstone
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.address import AddressSet

map_path = os.path.join(HERE, "..", "..", "native", "build", "us_code_map.txt")
us_exe = os.environ.get("MML_US_EXE") or os.path.join(os.environ.get("MML_US_GAME_DIR", ""), "MEGAMAN.EXE.orig")
US = open(us_exe, "rb").read()
text = program.getMemory().getBlock("AUTO")
mem = program.getMemory()
A = lambda x: addr(hex(x))
segs = [(s, e, d) for s, e, d in _SEGS if s < text.getEnd().getOffset()]
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def us_bytes(a, n):
    return US[a - 0x401000 + 0x400:a - 0x401000 + 0x400 + n]   # AUTO: file offset 0x400


def jp_bytes(j, n):
    return bytes((mem.getByte(A(j + k)) & 0xFF) for k in range(n))


def jp_of(a, n):
    for s, e, d in segs:
        if s <= a and a + n <= e:
            return a + d
    return None


# the two programs with relocated addresses masked (and, in .m, call and jump distances), as
# tools/jp/map_exe.py compares them
from map_exe import Pe
US_PE, JP_PE = Pe(us_exe), Pe(str(program.getExecutablePath()).lstrip("/"))
CTX = 8


def same_instruction(a, n, j):
    """the US instruction and CTX bytes on each side are the same bytes here: inside a run the
    Japanese code can differ locally (a short jump made near), and a start that only decodes the
    same could sit a few bytes off the real code there"""
    return (US_PE.s(a - CTX, n + 2 * CTX) == JP_PE.s(j - CTX, n + 2 * CTX) or
            US_PE.m(a - CTX, n + 2 * CTX) == JP_PE.m(j - CTX, n + 2 * CTX))


take, funcs, skip = {}, [], []
for line in open(map_path):
    p = line.split()
    if not p or p[0].startswith("#"):
        continue
    if p[0] == "P":   # around the installer's patches: the US decoding there is the patched code's
        s, e = int(p[1], 16), int(p[2], 16)
        j = jp_of(s, e - s)
        if j is not None:
            skip.append((j, j + e - s))
    elif p[0] == "F":
        funcs.append(int(p[1], 16))
    else:
        a, n = int(p[0], 16), int(p[1])
        j = jp_of(a, n)
        if j is not None:
            take[j] = (a, n)
take = dict((j, v) for j, v in take.items() if not any(s <= j < e for s, e in skip))
ok = dict((j, n) for j, (a, n) in take.items() if same_instruction(a, n, j))
wfunc = set(j for j in (jp_of(a, 1) for a in funcs) if j is not None and j in ok)

# 1. Japanese instructions that overlap a taken-over one without starting with it: clear
claimed = AddressSet()
for j, n in ok.items():
    claimed.addRange(A(j), A(j + n - 1))
bad = []
it = listing.getInstructions(claimed, True)
while it.hasNext():
    ins = it.next()
    o = ins.getAddress().getOffset()
    if ok.get(o) != ins.getLength():
        bad.append(ins.getAddress())
removed = 0
for a in bad:
    if fm.getFunctionAt(a) is not None:
        fm.removeFunction(a)
        removed += 1
    listing.clearCodeUnits(a, a, False)

# 2. the taken-over starts without an instruction: decode (data in the way is cleared)
missing = [j for j in sorted(ok) if listing.getInstructionAt(A(j)) is None]
for j in missing:
    cu = listing.getCodeUnitContaining(A(j))
    if cu is not None and listing.getInstructionAt(cu.getAddress()) is None:
        listing.clearCodeUnits(cu.getAddress(), cu.getAddress(), False)
    listing.clearCodeUnits(A(j), A(j + ok[j] - 1), False)
todo = AddressSet()
for j in missing:
    todo.add(A(j))
DisassembleCommand(todo, None, False).applyTo(program, monitor)
still = [j for j in missing if listing.getInstructionAt(A(j)) is None]

# 3. functions where the US project has them; then every function's body again
made = 0
for j in sorted(wfunc):
    if fm.getFunctionAt(A(j)) is None and listing.getInstructionAt(A(j)) is not None:
        if CreateFunctionCmd(A(j)).applyTo(program, monitor):
            made += 1
for f in list(fm.getFunctions(True)):
    CreateFunctionCmd.fixupFunctionBody(program, f, monitor)
print("US map: %d instruction starts in the shared code, %d the same instruction here (%d differ)" % (
    len(take), len(ok), len(take) - len(ok)))
print("cleared %d overlapping instructions (%d functions), decoded %d (%d failed), made %d functions" % (
    len(bad), removed, len(missing), len(still), made))
for j in still[:20]:
    print("  not decoded: %08x" % j)
