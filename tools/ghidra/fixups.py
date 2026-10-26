"""One-time fix-ups for a fresh mml_pc project (run inside ghidra_server via
`python gq.py run fixups.py`). Safe to re-run.

1. Extend .bss to its real mapped size. Every section header in MEGAMAN.EXE
   has VirtualSize 0, so Windows maps SizeOfRawData (up to 0x8985FF) but
   Ghidra's loader stops .bss at 0x6C55FF. Most engine globals live past that.
2. Turn every relocation that points into undisassembled code into a
   function entry. About a quarter of the code is only reachable through
   pointer tables (state machines, task handlers), which the analyzer can't
   follow on its own.
3. Re-run auto-analysis over the result.
"""
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.app.plugin.core.analysis import AutoAnalysisManager

mem = program.getMemory()

bss = mem.getBlock(".bss")
real_end = addr("0x898fff")
if bss.getEnd().compareTo(real_end) < 0:
    start = bss.getEnd().add(1)
    hi = mem.createUninitializedBlock("bss_hi", start, real_end.subtract(start) + 1, False)
    hi.setRead(True)
    hi.setWrite(True)
    bss = mem.join(bss, hi)
print(".bss now %s-%s" % (bss.getStart(), bss.getEnd()))

text = mem.getBlock("AUTO")
targets = set()
for r in program.getRelocationTable().getRelocations():
    try:
        v = mem.getInt(r.getAddress()) & 0xFFFFFFFF
    except Exception:
        continue
    t = addr(hex(v))
    if t is not None and text.contains(t) and listing.getInstructionContaining(t) is None:
        targets.add(t)
made = 0
for t in sorted(targets):
    DisassembleCommand(t, None, True).applyTo(program, monitor)
    if fm.getFunctionAt(t) is None and CreateFunctionCmd(t).applyTo(program, monitor):
        made += 1
print("relocation targets in undisassembled code: %d, functions created: %d" % (len(targets), made))

mgr = AutoAnalysisManager.getAnalysisManager(program)
mgr.reAnalyzeAll(None)
mgr.startAnalysis(monitor)
print("functions now %d" % fm.getFunctionCount())
