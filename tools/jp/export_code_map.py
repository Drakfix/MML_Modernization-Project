# Run inside the Ghidra server on the US project (python tools/ghidra/gq.py run ../jp/export_code_map.py):
# every instruction start and function entry of its code section, for tools/jp/apply_code_map.py,
# which makes the Japanese project decode the same code the same way. The US project has had all
# its repairs (fixups.py, realign.py, the recompiler's discover() and jump_orphans()).
# Writes native/build/us_code_map.txt (addresses only, nothing of the game's code).
import os

out_path = os.path.join(HERE, "..", "..", "native", "build", "us_code_map.txt")
text = program.getMemory().getBlock("AUTO")
lo, hi = text.getStart(), text.getEnd()
# the installer's code patches are in this project (discover()) and change instruction boundaries
# there: those ranges are left out (the Japanese project gets its own patches the same way)
ns = {}
exec(open(os.path.join(HERE, "..", "..", "src", "patch_manifest.py"), encoding="utf-8").read(), ns)
patched = [(off - 0x400 + 0x401000, off - 0x400 + 0x401000 + len(h) // 2) for off, h in ns["PATCHES"]
           if 0x400 <= off < 0x400 + text.getSize()]
starts = []
it = listing.getInstructions(lo, True)
while it.hasNext():
    ins = it.next()
    if ins.getAddress().compareTo(hi) > 0:
        break
    a, n = ins.getAddress().getOffset(), ins.getLength()
    if any(s - 16 < a + n and a < e + 16 for s, e in patched):
        continue
    starts.append((a, n))
entries = sorted(f.getEntryPoint().getOffset() for f in fm.getFunctions(True)
                 if text.contains(f.getEntryPoint()))
with open(out_path, "w") as fp:
    fp.write("# instruction starts (address length), then 'F address' for function entries\n")
    for a, n in starts:
        fp.write("%x %d\n" % (a, n))
    for a in entries:
        fp.write("F %x\n" % a)
    for s, e in patched:   # ranges left out: the Japanese project keeps its own decoding there
        fp.write("P %x %x\n" % (s - 16, e + 16))
print("%d instructions, %d functions -> %s" % (len(starts), len(entries), out_path))
