"""Undo the places where fixups.py step 2 decoded real code at the wrong byte (run inside
ghidra_server via `python gq.py run realign.py`, after fixups.py). Safe to re-run.

Step 2 turns every relocation that points into undisassembled code into a function entry. A
relocation can point at data inside the code section too (`MOV ESI,0x4d512f` copies a 6-byte
table that sits between two functions), and a function made there decodes the data as code and
runs on into the real code after it, one or more bytes out of step. Elsewhere a stray data item
blocks the disassembler. Either way, real code that the game runs never becomes an instruction
start, so the recompiler can't translate it: the standalone runtime stops there ("nothing runs"),
the DLL falls back to the original machine code (RE-notes 7.50).

Found with three sweeps over the listing (repeated at the end, see check()): direct calls and
branches whose target is not an instruction start, fall-throughs into undefined bytes, and
relocations that point inside an instruction. What remains after this script is data decoded as
code (GUIDs, pointer and value tables), which nothing runs.
"""
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.data import ByteDataType, ArrayDataType

mem = program.getMemory()
text = mem.getBlock("AUTO")


def A(x):
    return addr(hex(x))


def clear(lo, hi):
    """Remove instructions, data and functions that start in [lo, hi]."""
    a = lo
    while a <= hi:
        f = fm.getFunctionAt(A(a))
        if f is not None:
            fm.removeFunction(A(a))
        a += 1
    listing.clearCodeUnits(A(lo), A(hi), False)


def code(at):
    DisassembleCommand(A(at), None, True).applyTo(program, monitor)
    if listing.getInstructionAt(A(at)) is None:
        raise Exception("no instruction decoded at %08x" % at)


def refit(at):
    """Recompute the body of the function holding `at` (it gained the realigned code)."""
    f = fm.getFunctionContaining(A(at))
    if f is not None:
        CreateFunctionCmd.fixupFunctionBody(program, f, monitor)


# Balloon Fantasy: called from 0x4D5365 / 0x4D5587. A 6-byte table at 0x4D512F (copied by
# the function itself) became function FUN_004d512f and swallowed the real entry 0x4D5135.
clear(0x4D512F, 0x4D5137)
listing.createData(A(0x4D512F), ArrayDataType(ByteDataType(), 6, 1))
code(0x4D5135)
if fm.getFunctionAt(A(0x4D5135)) is None:
    CreateFunctionCmd(A(0x4D5135)).applyTo(program, monitor)

# 0x497DB1 AND [0x55A66C],0x7F falls through to INC byte [ECX+5] (0x497DB8) and CALL 0x47EBCB
# (0x497DBB, also a branch target); they were decoded from 0x497DBA, one byte into the INC.
clear(0x497DB8, 0x497DD2)
code(0x497DB8)
refit(0x497DB1)

# 0x4607B6 INC byte [EAX+9] falls through to JMP (0x4607B9); the switch case right after it
# (0x4607BE, a jump table target from 0x460709) was never decoded: a data item at 0x4607BD
# (a read reference from a mis-decoded operand) blocked both.
clear(0x4607B9, 0x4607D4)
code(0x4607B9)
code(0x4607BE)
refit(0x4607B6)


def check():
    calls, falls, rels = [], [], []
    it = listing.getInstructions(True)
    while it.hasNext():
        ins = it.next()
        for t in ins.getFlows():
            if not text.contains(t):
                continue
            c = listing.getInstructionContaining(t)
            if ins.getFlowType().isCall() and fm.getFunctionAt(t) is None or c is not None and c.getAddress() != t:
                calls.append("%s -> %s" % (ins.getAddress(), t))
        ft = ins.getFallThrough()
        if ft is not None and text.contains(ft) and listing.getInstructionAt(ft) is None:
            falls.append("%s -> %s" % (ins.getAddress(), ft))
    for r in program.getRelocationTable().getRelocations():
        try:
            t = A(mem.getInt(r.getAddress()) & 0xFFFFFFFF)
        except Exception:
            continue
        c = listing.getInstructionContaining(t) if t is not None and text.contains(t) else None
        if c is not None and c.getAddress() != t:
            rels.append("%s -> %s" % (r.getAddress(), t))
    print("calls/branches not at an instruction or function start: %d  %s" % (len(calls), " ".join(calls)))
    print("fall-throughs into undefined bytes: %d  %s" % (len(falls), " ".join(falls)))
    print("relocations inside an instruction: %d  %s" % (len(rels), " ".join(rels)))


check()
