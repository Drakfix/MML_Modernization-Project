"""Long-running Ghidra query server for the mml_pc project.

Keeps the project open in one JVM and answers one JSON request per TCP
connection on 127.0.0.1:47321: {"cmd": "...", "args": [...]}. Use gq.py as
the client. Write commands (rename, comment, setsig, symbols, py) run in a
transaction and are saved immediately.

Commands:
  decomp ADDR|NAME          decompiled C of the containing function
  disasm ADDR [N]           N instructions from ADDR (default 40)
  func ADDR|NAME            name, signature, body range, caller/callee counts
  xrefs ADDR|NAME           references to the address
  callees ADDR|NAME         functions called from the function
  callers ADDR|NAME         functions calling the function
  find REGEX                symbol names matching REGEX
  rename ADDR NAME          set function/label name (creates function if needed with 'func:' prefix on ADDR)
  comment ADDR TEXT         set a plate comment
  setsig ADDR PROTOTYPE     apply a C prototype to the function at ADDR
  symbols CSV               import a symbols.csv (relative paths = this folder)
  py CODE                   exec CODE with program/flat/api in scope; prints are returned
  run SCRIPT                exec a script file the same way (relative paths = this folder)
  stats                     function count and how many are still FUN_ names
"""
import contextlib
import csv
import io
import json
import os
import re
import socket
import sys
import traceback

import pyghidra

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("MML_GAME_DIR")
if not GAME:
    raise SystemExit("Set MML_GAME_DIR to your game folder (with MEGAMAN.EXE.orig).")
PROJ_DIR = os.environ.get("MML_GHIDRA_DIR", os.path.join(GAME, "ghidra_decomp"))
PORT = 47321

os.environ.setdefault("GHIDRA_INSTALL_DIR", r"C:\tools\ghidra_12.1.2_PUBLIC")
pyghidra.start()

from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.program.flatapi import FlatProgramAPI
from ghidra.program.model.symbol import SourceType
from ghidra.program.model.data import (
    ByteDataType, WordDataType, DWordDataType, FloatDataType, DoubleDataType,
    PointerDataType, StructureDataType, CategoryPath, DataTypeConflictHandler,
    ArrayDataType)
from ghidra.program.model.listing import CodeUnit
from ghidra.util.task import ConsoleTaskMonitor

proj = GhidraProject.openProject(PROJ_DIR, "mml_pc", True)
program = proj.openProgram("/", "MEGAMAN.EXE", False)
flat = FlatProgramAPI(program)
monitor = ConsoleTaskMonitor()
fm = program.getFunctionManager()
listing = program.getListing()
st = program.getSymbolTable()

decomp = DecompInterface()
decomp.setOptions(DecompileOptions())
decomp.openProgram(program)


def addr(s):
    s = str(s)
    if re.fullmatch(r"(0x)?[0-9a-fA-F]+", s):
        return program.getAddressFactory().getAddress(hex(int(s, 16)))
    syms = list(st.getSymbols(s))
    if not syms:
        raise ValueError("no symbol named %s" % s)
    return syms[0].getAddress()


def func_at(s):
    a = addr(s)
    f = fm.getFunctionContaining(a)
    if f is None:
        raise ValueError("no function contains %s" % a)
    return f


def tx(fn):
    t = program.startTransaction("gq")
    ok = False
    try:
        r = fn()
        ok = True
        return r
    finally:
        program.endTransaction(t, ok)
        if ok:
            proj.save(program)


def c_decomp(a):
    f = func_at(a)
    res = decomp.decompileFunction(f, 60, monitor)
    if not res.decompileCompleted():
        return "decompile failed: " + str(res.getErrorMessage())
    return res.getDecompiledFunction().getC()


def c_disasm(a, n="40"):
    out = []
    ins = listing.getInstructionAt(addr(a)) or listing.getInstructionAfter(addr(a))
    for _ in range(int(n)):
        if ins is None:
            break
        f = fm.getFunctionAt(ins.getAddress())
        if f:
            out.append("%s:" % f.getName())
        out.append("  %s  %s" % (ins.getAddress(), ins))
        ins = ins.getNext()
    return "\n".join(out)


def c_func(a):
    f = func_at(a)
    return "%s @ %s\n%s\nbody %s\ncallers %d, callees %d" % (
        f.getName(), f.getEntryPoint(), f.getSignature().getPrototypeString(),
        f.getBody(), len(f.getCallingFunctions(monitor)), len(f.getCalledFunctions(monitor)))


def c_xrefs(a):
    out = []
    for r in program.getReferenceManager().getReferencesTo(addr(a)):
        f = fm.getFunctionContaining(r.getFromAddress())
        out.append("%s %s in %s" % (r.getFromAddress(), r.getReferenceType(), f.getName() if f else "?"))
    return "\n".join(out) or "(none)"


def c_callees(a):
    return "\n".join("%s %s" % (g.getEntryPoint(), g.getName())
                     for g in sorted(func_at(a).getCalledFunctions(monitor), key=lambda g: g.getEntryPoint()))


def c_callers(a):
    return "\n".join("%s %s" % (g.getEntryPoint(), g.getName())
                     for g in sorted(func_at(a).getCallingFunctions(monitor), key=lambda g: g.getEntryPoint()))


def c_find(rx):
    r = re.compile(rx, re.I)
    out = []
    for s in st.getAllSymbols(True):
        if r.search(s.getName()):
            out.append("%s %s" % (s.getAddress(), s.getName()))
        if len(out) > 300:
            out.append("...")
            break
    return "\n".join(out) or "(none)"


def set_name(a, name, make_func=False):
    f = fm.getFunctionAt(a)
    if f is None and make_func:
        f = flat.createFunction(a, name)
        if f is None:
            # Disassemble first, then retry.
            flat.disassemble(a)
            f = flat.createFunction(a, name)
    if f is not None:
        f.setName(name, SourceType.USER_DEFINED)
        return f
    st.createLabel(a, name, SourceType.USER_DEFINED).setPrimary()
    return None


def c_rename(a, name):
    make = a.startswith("func:")
    a = addr(a[5:] if make else a)
    tx(lambda: set_name(a, name, make))
    return "ok %s = %s" % (a, name)


def c_comment(a, *text):
    a = addr(a)
    tx(lambda: listing.setComment(a, CodeUnit.PLATE_COMMENT, " ".join(text)))
    return "ok"


def c_setsig(a, *proto):
    from ghidra.app.util.parser import FunctionSignatureParser
    from ghidra.app.cmd.function import ApplyFunctionSignatureCmd
    f = func_at(a)
    sig = FunctionSignatureParser(program.getDataTypeManager(), None).parse(f.getSignature(), " ".join(proto))
    def go():
        cmd = ApplyFunctionSignatureCmd(f.getEntryPoint(), sig, SourceType.USER_DEFINED)
        if not cmd.applyTo(program, monitor):
            raise RuntimeError(cmd.getStatusMsg())
    tx(go)
    return "ok " + f.getSignature().getPrototypeString()


# ---- structs we know enough about to type ----
def build_structs():
    dtm = program.getDataTypeManager()
    cat = CategoryPath("/mml")
    u8, u16, u32 = ByteDataType(), WordDataType(), DWordDataType()

    ctx = StructureDataType(cat, "gfx_ctx", 0x90)
    ctx.replaceAtOffset(0x70, ArrayDataType(PointerDataType(), 8, 4), 32, "root_ot",
                        "8 buckets; +0x78 HUD (idx2), +0x7c dialogue/minimap (idx3), +0x80 big-OT merge (idx4)")
    dtm.addDataType(ctx, DataTypeConflictHandler.REPLACE_HANDLER)

    p = StructureDataType(cat, "player_t", 0xB8)
    for off, dt, name, cmt in [
        (0x08, u32, "state_flags", "bit 0x800 = action/roll/aim busy, bit 0x01000000 = roll side (1=left)"),
        (0x1C, u32, "x", "16.16"), (0x20, u32, "y", "16.16 height"), (0x24, u32, "z", "16.16"),
        (0x54, u32, "facing", "16.16 angle, int part at +0x56 (12-bit)"),
        (0x6E, u16, "hp", None), (0xAE, u16, "special", None), (0xB2, u16, "hp_display", "per-frame mirror of hp"),
        (0xB4, u32, "turn_speed", "turn accumulator clamped per state")]:
        p.replaceAtOffset(off, dt, dt.getLength(), name, cmt)
    dtm.addDataType(p, DataTypeConflictHandler.REPLACE_HANDLER)


TYPES = {
    "byte": ByteDataType(), "word": WordDataType(), "dword": DWordDataType(),
    "float": FloatDataType(), "double": DoubleDataType(), "pointer": PointerDataType(),
}


def c_symbols(path):
    if not os.path.isabs(path):
        path = os.path.join(HERE, path)
    rows = [r for r in csv.reader(open(path, encoding="utf-8")) if r and not r[0].startswith("#")][1:]
    log = []

    def go():
        build_structs()
        dtm = program.getDataTypeManager()
        for a_s, kind, typ, name, cmt in rows:
            a = addr(a_s)
            set_name(a, name, kind == "func")
            if typ:
                dt = TYPES.get(typ) or dtm.getDataType("/mml/" + typ)
                if dt is None:
                    log.append("unknown type %s for %s" % (typ, name))
                else:
                    try:
                        flat.clearListing(a, a.add(dt.getLength() - 1))
                        flat.createData(a, dt)
                    except Exception as e:
                        log.append("type %s at %s failed: %s" % (typ, a, e))
            if cmt:
                listing.setComment(a, CodeUnit.PLATE_COMMENT, cmt)
        return None

    tx(go)
    return "applied %d symbols\n%s" % (len(rows), "\n".join(log))


def c_py(*code):
    buf = io.StringIO()
    env = {"program": program, "flat": flat, "fm": fm, "listing": listing, "st": st,
           "addr": addr, "func_at": func_at, "monitor": monitor, "decomp": decomp}
    t = program.startTransaction("gq py")
    ok = False
    try:
        with contextlib.redirect_stdout(buf):
            exec(" ".join(code), env)
        ok = True
    finally:
        program.endTransaction(t, ok)
        if ok:
            proj.save(program)
    return buf.getvalue()


def c_run(path):
    if not os.path.isabs(path):
        path = os.path.join(HERE, path)
    with open(path, encoding="utf-8") as f:
        return c_py(f.read())


def c_stats():
    n = fm.getFunctionCount()
    unnamed = sum(1 for f in fm.getFunctions(True) if f.getName().startswith("FUN_"))
    return "functions %d, still FUN_ %d, named %d" % (n, unnamed, n - unnamed)


CMDS = {"decomp": c_decomp, "disasm": c_disasm, "func": c_func, "xrefs": c_xrefs,
        "callees": c_callees, "callers": c_callers, "find": c_find, "rename": c_rename,
        "comment": c_comment, "setsig": c_setsig, "symbols": c_symbols, "py": c_py,
        "stats": c_stats, "run": c_run}


def serve():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(4)
    print("ghidra_server ready on %d" % PORT, flush=True)
    while True:
        conn, _ = srv.accept()
        with conn:
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(65536)
                if not chunk:
                    break
                data += chunk
            try:
                req = json.loads(data.decode("utf-8"))
                if req["cmd"] == "quit":
                    conn.sendall(b"bye")
                    break
                out = CMDS[req["cmd"]](*req.get("args", []))
            except Exception:
                out = "ERROR\n" + traceback.format_exc()
            conn.sendall(str(out).encode("utf-8", "replace"))
    decomp.dispose()
    proj.close()


if __name__ == "__main__":
    serve()
