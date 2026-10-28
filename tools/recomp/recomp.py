"""Static recompiler: x86 functions of MEGAMAN.EXE to C, from Ghidra's P-code.

Runs inside the Ghidra query server (tools/ghidra), which has the analysed
program open:

    python tools/ghidra/gq.py py "exec(open(r'tools/recomp/recomp.py').read()); main(roots=[0x42961B], out=r'native/build/gen/recomp_gen.c')"

The whole executable (after a one-time discover(), which creates the functions Ghidra's
analysis missed because they are only reached through pointer tables):

    ... discover(); main(roots=all_roots(), out=r'native/build/gen/recomp_gen.c', shards=64)

For each root it follows direct calls and tail jumps to collect a closed set of
functions, then emits one C function per x86 function. Every P-code op becomes
one C statement over the guest state (see native/src/recomp_rt.h): registers by
their Ghidra register-space offset, P-code temporaries as C locals, memory
through LD/ST. Control flow inside a function becomes labels and gotos; direct
calls become C calls (the P-code already pushed the return address on the guest
stack, and the callee's RETURN pops it); indirect calls and jumps go through the
runtime.

The output is derived from the game's code, so it is written to the build
folder and never committed.
"""
import os

from ghidra.program.model.pcode import PcodeOp
from ghidra.util.task import TaskMonitor

MON = TaskMonitor.DUMMY
INT_T = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
SINT_T = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}


class Unsupported(Exception):
    pass


def fname(a):
    return "f_%08x" % a


# The game's cooperative threads (two slots, each with its own stack) switch stacks by
# loading ESP, which C can't follow. Calls to these become runtime functions that run each
# thread on a host fiber instead (native/src/recomp.c). exec and exit never return.
INTRINSICS = {0x4EED08: ("rt_co_schedule", False), 0x4EED79: ("rt_co_create", False),
              0x4EEDA4: ("rt_co_yield", False), 0x4EEDCA: ("rt_co_exec", True),
              0x4EEDFD: ("rt_co_exit", True)}


def ctype(size):
    if size == 10:
        return "double"
    if size in INT_T:
        return INT_T[size]
    if size in (3, 5, 6, 7):
        return "uint64_t"
    raise Unsupported("varnode size %d" % size)


FLAGS = range(0x200, 0x210)   # CF PF AF ZF SF DF OF... one byte each


def flag_of(v):
    if v is None or v.getAddress().getAddressSpace().getName() != "register":
        return None
    return v.getOffset() if v.getOffset() in FLAGS and v.getSize() == 1 else None


ST0, ST1, ST2 = 0x1100, 0x1110, 0x1120


def x87_delta(pcode):
    """net x87 pushes of one instruction: Ghidra models the stack as fixed registers,
    a push is the chain ST1<-ST0, ST2<-ST1..., a pop the chain ST0<-ST1, ST1<-ST2..."""
    moves = []
    for p in pcode:
        m = None
        if p.getOpcode() == PcodeOp.COPY and p.getNumInputs() == 1:
            o, i = p.getOutput(), p.getInput(0)
            if (o.getAddress().getAddressSpace().getName() == "register" and
                    i.getAddress().getAddressSpace().getName() == "register" and o.getSize() == 10):
                m = (o.getOffset(), i.getOffset())
        moves.append(m)
    d = 0
    for k in range(len(moves) - 1):
        if moves[k] == (ST2, ST1) and moves[k + 1] == (ST1, ST0):
            d += 1
        if moves[k] == (ST0, ST1) and moves[k + 1] == (ST1, ST2):
            d -= 1
    return d


class FuncGen:
    INDEX = {}   # entry address -> table index (set by main)

    def __init__(self, func, known, entry=None):
        self.f = func
        self.entry = entry if entry is not None else func.getEntryPoint().getOffset()
        self.known = known            # entry addresses of every function we emit
        self.locals = {}              # (offset, size) -> name
        self.labels = set()           # instruction addresses that need a label
        self.plabels = set()          # (instruction address, op index) inside one instruction
        self.lines = []
        self.callees = set()          # functions this one calls or jumps into
        self.impure = False           # reaches something outside the recompiled code
        self.insns = list(program.getListing().getInstructions(func.getBody(), True))
        self.addrs = set(i.getAddress().getOffset() for i in self.insns)

    # ---- varnodes ----
    def local(self, off, size):
        k = (off, size)
        if k not in self.locals:
            self.locals[k] = "u%x_%d" % (off, size)
        return self.locals[k]

    def rd(self, v):
        sp = v.getAddress().getAddressSpace().getName()
        off, size = v.getOffset(), v.getSize()
        if sp == "const":
            if size > 8:
                raise Unsupported("wide constant")
            mask = (1 << (8 * size)) - 1
            return "((%s)0x%xull)" % (ctype(size), off & mask)
        if sp == "register":
            if size == 10:
                return "RF(c,0x%x)" % off
            if off == 0x10A2 and size == 2:
                return "FSW(c)"
            if size not in INT_T:
                raise Unsupported("register size %d" % size)
            return "R%d(c,0x%x)" % (size * 8, off)
        if sp == "unique":
            ctype(size)
            return self.local(off, size)
        if sp == "ram":
            if size == 10:
                return "rt_ld80(0x%xu)" % off
            if size not in INT_T:
                return "LDN(0x%xu,%d)" % (off, size)
            return "LD%d(0x%xu)" % (size * 8, off)
        raise Unsupported("space " + sp)

    def wr(self, v, expr):
        sp = v.getAddress().getAddressSpace().getName()
        off, size = v.getOffset(), v.getSize()
        if size in (3, 5, 6, 7):
            expr = "((%s) & 0x%xull)" % (expr, (1 << (8 * size)) - 1)
        if sp == "register":
            if size == 10:
                return "RF_SET(c,0x%x,%s);" % (off, expr)
            if size not in INT_T:
                raise Unsupported("register size %d" % size)
            return "R%d_SET(c,0x%x,%s);" % (size * 8, off, expr)
        if sp == "unique":
            ctype(size)
            return "%s = %s;" % (self.local(off, size), expr)
        if sp == "ram":
            if size == 10:
                return "rt_st80(c,0x%xu,%s);" % (off, expr)
            if size not in INT_T:
                return "STN(c,0x%xu,%s,%d);" % (off, expr, size)
            return "ST%d(c,0x%xu,%s);" % (size * 8, off, expr)
        raise Unsupported("write to space " + sp)

    def fval(self, v):
        """a float operand as a C double/float expression"""
        s = v.getSize()
        if s == 4:
            return "F32(%s)" % self.rd(v)
        if s == 8:
            return "F64(%s)" % self.rd(v)
        if s == 10:
            return self.rd(v)
        raise Unsupported("float size %d" % s)

    @staticmethod
    def fout(size, expr, arith=True):
        if size == 4:
            return "FBITS32((float)(%s))" % expr
        if size == 8:
            return "FBITS64((double)(%s))" % expr
        if size == 10:
            return "FPC(c,%s)" % expr if arith else "(double)(%s)" % expr
        raise Unsupported("float size %d" % size)

    # ---- control flow ----
    def goto_ram(self, target, ins):
        if target in INTRINSICS:
            self.impure = True
            return "{ %s(c); return; }" % INTRINSICS[target][0]
        if target in self.addrs:
            self.labels.add(target)
            return "goto L_%08x;" % target
        if target in self.known:
            self.callees.add(target)
            return "{ RC_TAIL(c,%d,0x%xu,%s); return; }" % (self.INDEX[target], target, fname(target))
        self.impure = True
        return "{ rt_jump(c,0x%xu); return; }" % target

    def branch(self, v, ins, idx):
        sp = v.getAddress().getAddressSpace().getName()
        if sp == "const":   # relative to this op, inside the instruction's P-code
            t = idx + v.getOffset() if v.getOffset() < (1 << 31) else idx - ((1 << 64) - v.getOffset())
            a = ins.getAddress().getOffset()
            self.plabels.add((a, t))
            return "goto P_%08x_%d;" % (a, t)
        return self.goto_ram(v.getOffset(), ins)

    # ---- one op ----
    def op(self, p, ins, idx):
        o = p.getOpcode()
        ins_ = [p.getInput(i) for i in range(p.getNumInputs())]
        out = p.getOutput()
        rd = self.rd
        if o == PcodeOp.COPY:
            return self.wr(out, rd(ins_[0]))
        if o == PcodeOp.LOAD:
            s = out.getSize()
            if s not in INT_T and s != 10:
                return self.wr(out, "LDN(%s,%d)" % (rd(ins_[1]), s))
            if s == 10:
                return self.wr(out, "rt_ld80(%s)" % rd(ins_[1]))
            return self.wr(out, "LD%d(%s)" % (s * 8, rd(ins_[1])))
        if o == PcodeOp.STORE:
            v = ins_[2]
            if v.getSize() not in INT_T and v.getSize() != 10:
                return "STN(c,%s,%s,%d);" % (rd(ins_[1]), rd(v), v.getSize())
            if v.getSize() == 10:
                return "rt_st80(c,%s,%s);" % (rd(ins_[1]), rd(v))
            return "ST%d(c,%s,%s);" % (v.getSize() * 8, rd(ins_[1]), rd(v))
        if o == PcodeOp.BRANCH:
            return self.branch(ins_[0], ins, idx)
        if o == PcodeOp.CBRANCH:
            return "if (%s) %s" % (rd(ins_[1]), self.branch(ins_[0], ins, idx))
        if o == PcodeOp.BRANCHIND:
            return self.branchind(rd(ins_[0]), ins)
        if o == PcodeOp.CALL:
            t = ins_[0].getOffset()
            if t in INTRINSICS:
                self.impure = True
                name, noreturn = INTRINSICS[t]
                return "%s(c);%s" % (name, " return;" if noreturn else "")
            if t in self.known:
                self.callees.add(t)
                return "RC_CALL(c,%d,0x%xu,%s);" % (self.INDEX[t], t, fname(t))
            self.impure = True
            return "rt_call(c,0x%xu);" % t
        if o == PcodeOp.CALLIND:
            self.impure = True
            return "rt_call(c,%s);" % rd(ins_[0])
        if o == PcodeOp.RETURN:
            return "c->ret = %s; return;" % rd(ins_[0])
        if o == PcodeOp.CALLOTHER:
            name = program.getLanguage().getUserDefinedOpName(int(ins_[0].getOffset()))
            args = ins_[1:]
            if name in ("LOCK", "UNLOCK"):   # the LOCK prefix: the game logic never runs in parallel with itself
                return ""
            if name == "swi":                # INT3 padding between functions
                return ""
            if name == "fpatan":             # ST1 = atan(ST1 / ST0), the pop follows in the P-code
                return self.wr(out, "FPC(c,atan2(%s,%s))" % (self.fval(args[0]), self.fval(args[1])))
            if name in ("paddusb", "psubusb", "paddusw", "psubusw", "paddsb", "psubsb", "paddsw", "psubsw"):
                return self.wr(out, "MMX_%s(%s,%s)" % (name.upper(), rd(args[0]), rd(args[1])))
            if name.startswith("cpuid"):     # returns the address of EAX, EBX, EDX, ECX results
                return self.wr(out, "rt_cpuid(%s)" % rd(args[0]))
            if name == "segment":            # flat memory: the offset
                return self.wr(out, "(%s)%s" % (ctype(out.getSize()), rd(args[1])))
            if name == "LocalDescriptorTableRegister":
                return self.wr(out, "(%s)0" % ctype(out.getSize()))
            if name == "in":                 # port I/O faults in user mode: dead code in this port
                return self.wr(out, "(%s)rt_port_in(c,%s)" % (ctype(out.getSize()), rd(args[0])))
            if name == "out":
                return "rt_port_out(c,%s);" % rd(args[0])
            self.impure = True
            return 'rt_unsupported(c,0x%xu,"%s");' % (ins.getAddress().getOffset(), name)

        s = out.getSize() if out else 0
        T = ctype(s) if out and s != 10 else None
        a = rd(ins_[0]) if ins_ else None
        b = rd(ins_[1]) if len(ins_) > 1 else None
        isz = ins_[0].getSize() if ins_ else 0
        bits = isz * 8
        binops = {PcodeOp.INT_ADD: "+", PcodeOp.INT_SUB: "-", PcodeOp.INT_MULT: "*", PcodeOp.INT_AND: "&",
                  PcodeOp.INT_OR: "|", PcodeOp.INT_XOR: "^", PcodeOp.INT_DIV: "/", PcodeOp.INT_REM: "%",
                  PcodeOp.BOOL_AND: "&", PcodeOp.BOOL_OR: "|", PcodeOp.BOOL_XOR: "^"}
        if o in binops:
            return self.wr(out, "(%s)(%s %s %s)" % (T, a, binops[o], b))
        cmps = {PcodeOp.INT_EQUAL: "==", PcodeOp.INT_NOTEQUAL: "!=", PcodeOp.INT_LESS: "<",
                PcodeOp.INT_LESSEQUAL: "<="}
        if o in cmps:
            return self.wr(out, "(uint8_t)(%s %s %s)" % (a, cmps[o], b))
        scmps = {PcodeOp.INT_SLESS: "<", PcodeOp.INT_SLESSEQUAL: "<="}
        if o in scmps:
            if isz not in SINT_T:
                raise Unsupported("signed compare size %d" % isz)
            S = SINT_T[isz]
            return self.wr(out, "(uint8_t)((%s)%s %s (%s)%s)" % (S, a, scmps[o], S, b))
        if o in (PcodeOp.INT_SDIV, PcodeOp.INT_SREM):
            S = SINT_T[isz]
            return self.wr(out, "(%s)((%s)%s %s (%s)%s)" % (T, S, a, "/" if o == PcodeOp.INT_SDIV else "%", S, b))
        if o == PcodeOp.INT_2COMP:
            return self.wr(out, "(%s)(0 - %s)" % (T, a))
        if o == PcodeOp.INT_NEGATE:
            return self.wr(out, "(%s)~%s" % (T, a))
        if o == PcodeOp.BOOL_NEGATE:
            return self.wr(out, "(uint8_t)!%s" % a)
        if o == PcodeOp.INT_ZEXT:
            return self.wr(out, "(%s)%s" % (T, a))
        if o == PcodeOp.INT_SEXT:
            if s not in SINT_T or isz not in SINT_T:
                raise Unsupported("sext %d->%d" % (isz, s))
            return self.wr(out, "(%s)(%s)(%s)%s" % (T, SINT_T[s], SINT_T[isz], a))
        if o in (PcodeOp.INT_LEFT, PcodeOp.INT_RIGHT, PcodeOp.INT_SRIGHT, PcodeOp.INT_CARRY,
                 PcodeOp.INT_SCARRY, PcodeOp.INT_SBORROW) and isz not in INT_T:
            raise Unsupported("op size %d" % isz)
        if o in (PcodeOp.INT_LEFT, PcodeOp.INT_RIGHT, PcodeOp.INT_SRIGHT):
            fn = {PcodeOp.INT_LEFT: "SHL", PcodeOp.INT_RIGHT: "SHR", PcodeOp.INT_SRIGHT: "SAR"}[o]
            return self.wr(out, "%s%d(%s,%s)" % (fn, bits, a, b))
        if o == PcodeOp.INT_CARRY:
            return self.wr(out, "CARRY%d(%s,%s)" % (bits, a, b))
        if o == PcodeOp.INT_SCARRY:
            return self.wr(out, "SCARRY%d(%s,%s)" % (bits, a, b))
        if o == PcodeOp.INT_SBORROW:
            return self.wr(out, "SBORROW%d(%s,%s)" % (bits, a, b))
        if o == PcodeOp.POPCOUNT:
            return self.wr(out, "(%s)POPCNT(%s)" % (T, a))
        if o == PcodeOp.PIECE:
            lo = ins_[1].getSize() * 8
            return self.wr(out, "(%s)(((%s)%s << %d) | %s)" % (T, T, a, lo, b))
        if o == PcodeOp.SUBPIECE:
            if isz == 10 or s == 10:
                raise Unsupported("SUBPIECE on x87 value")
            return self.wr(out, "(%s)(%s >> %d)" % (T, a, 8 * ins_[1].getOffset()) if ins_[1].getOffset()
                           else "(%s)%s" % (T, a))
        # floating point
        fops = {PcodeOp.FLOAT_ADD: "+", PcodeOp.FLOAT_SUB: "-", PcodeOp.FLOAT_MULT: "*", PcodeOp.FLOAT_DIV: "/"}
        if o in fops:
            return self.wr(out, self.fout(s, "%s %s %s" % (self.fval(ins_[0]), fops[o], self.fval(ins_[1]))))
        fcmps = {PcodeOp.FLOAT_EQUAL: "==", PcodeOp.FLOAT_NOTEQUAL: "!=", PcodeOp.FLOAT_LESS: "<",
                 PcodeOp.FLOAT_LESSEQUAL: "<="}
        if o in fcmps:
            return self.wr(out, "(uint8_t)(%s %s %s)" % (self.fval(ins_[0]), fcmps[o], self.fval(ins_[1])))
        if o == PcodeOp.FLOAT_NAN:
            return self.wr(out, "(uint8_t)isnan(%s)" % self.fval(ins_[0]))
        if o == PcodeOp.FLOAT_NEG:
            return self.wr(out, self.fout(s, "-(%s)" % self.fval(ins_[0]), arith=False))
        if o == PcodeOp.FLOAT_ABS:
            return self.wr(out, self.fout(s, "fabs(%s)" % self.fval(ins_[0]), arith=False))
        if o == PcodeOp.FLOAT_SQRT:
            return self.wr(out, self.fout(s, "sqrt(%s)" % self.fval(ins_[0])))
        if o == PcodeOp.FLOAT_FLOAT2FLOAT:
            return self.wr(out, self.fout(s, self.fval(ins_[0]), arith=False))
        if o == PcodeOp.FLOAT_INT2FLOAT:
            return self.wr(out, self.fout(s, "(double)(%s)%s" % (SINT_T[isz], a), arith=False))
        if o == PcodeOp.FLOAT_TRUNC:
            fn = {2: "FTOI16", 4: "FTOI32", 8: "FTOI64"}.get(s)
            if not fn:
                raise Unsupported("TRUNC to %d" % s)
            return self.wr(out, "%s(%s)" % (fn, self.fval(ins_[0])))
        if o == PcodeOp.FLOAT_ROUND:
            return self.wr(out, self.fout(s, "FROUND(c,%s)" % self.fval(ins_[0]), arith=False))
        if o == PcodeOp.FLOAT_CEIL:
            return self.wr(out, self.fout(s, "ceil(%s)" % self.fval(ins_[0]), arith=False))
        if o == PcodeOp.FLOAT_FLOOR:
            return self.wr(out, self.fout(s, "floor(%s)" % self.fval(ins_[0]), arith=False))
        raise Unsupported("op " + p.getMnemonic())

    def branchind(self, expr, ins):
        targets = [r.getToAddress().getOffset() for r in ins.getReferencesFrom()
                   if r.getReferenceType().isJump()]
        cases = []
        for t in sorted(set(targets)):
            if t in self.addrs:
                self.labels.add(t)
                cases.append("case 0x%xu: goto L_%08x;" % (t, t))
        if not cases:
            self.impure = True
            return "{ rt_jump(c,%s); return; }" % expr
        return "switch (%s) { %s default: rt_jump(c,%s); return; }" % (expr, " ".join(cases), expr)

    # ---- dead flag writes ----
    FLOW = (PcodeOp.BRANCH, PcodeOp.CBRANCH, PcodeOp.BRANCHIND, PcodeOp.CALL, PcodeOp.CALLIND,
            PcodeOp.RETURN, PcodeOp.CALLOTHER)

    def drop_dead_flags(self, body):
        """a flag write is dead if, going forward in straight-line code, the same flag is written
        again before anything reads it; any branch, call or return ends the straight line"""
        over = set()
        for k in range(len(body) - 1, -1, -1):
            kind, key, text, p = body[k]
            if kind == "X":
                over = set()
                continue
            if p is None:
                continue
            if p.getOpcode() in self.FLOW:
                over = set()
            f = flag_of(p.getOutput())
            if f is not None:
                if f in over:
                    body[k] = (kind, key, "", None)
                    continue
                over.add(f)
            for i in range(p.getNumInputs()):
                g = flag_of(p.getInput(i))
                if g is not None:
                    over.discard(g)

    # ---- the function ----
    def gen(self):
        body = []
        insns = self.insns
        for n, ins in enumerate(insns):
            a = ins.getAddress().getOffset()
            body.append(("I", a, "/* %08x  %s */" % (a, str(ins).replace("*/", "* /")), None))
            pc = list(ins.getPcode())
            d = x87_delta(pc)
            if d:
                body.append(("P", (a, -1), "c->fdelta += %d;" % d, None))
            for i, p in enumerate(pc):
                body.append(("P", (a, i), self.op(p, ins, i), p))
            body.append(("P", (a, len(pc)), "", None))   # target for branches past the last op
            ft = ins.getFallThrough()
            nxt = insns[n + 1].getAddress().getOffset() if n + 1 < len(insns) else None
            if ft is not None and ft.getOffset() != nxt:
                body.append(("X", None, self.goto_ram(ft.getOffset(), ins), None))
        if self.entry not in self.addrs:   # inside an instruction: data Ghidra decoded as code
            raise Unsupported("entry %08x is not an instruction" % self.entry)
        out = ["void %s(RCtx *c)" % fname(self.entry), "{"]
        for (off, size), name in sorted(self.locals.items()):
            out.append("    %s %s = 0;" % (ctype(size), name))
        if insns and insns[0].getAddress().getOffset() != self.entry:
            self.labels.add(self.entry)
            out.append("    goto L_%08x;" % self.entry)
        self.drop_dead_flags(body)
        for kind, key, text, meta in body:
            if kind == "I":
                if key in self.labels:
                    out.append("L_%08x:" % key)
                out.append("    " + text)
            elif kind == "P":
                if key in self.plabels:
                    out.append("P_%08x_%d:" % key)
                if text:
                    out.append("    " + text)
            else:
                out.append("    " + text)
        out.append("    ;")
        out.append("}")
        return "\n".join(out)


def stack_switch(g):
    """loads ESP from memory or another register (the coroutine scheduler): can't be run twice"""
    for ins in g.insns:
        m = str(ins)
        if (m.startswith("MOV ESP,") or m.startswith("XCHG ESP") or m.startswith("LSS")) and "EBP" not in m:
            return True
    return False


def reloc_code_targets():
    """every absolute address of code in the image, from the relocation table: function pointers
    in data (the game's task and object handlers live in tables) and in instructions"""
    mem, lst = program.getMemory(), program.getListing()
    text = [b for b in mem.getBlocks() if b.isExecute()]
    lo = min(b.getStart().getOffset() for b in text)
    hi = max(b.getEnd().getOffset() for b in text)
    af = program.getAddressFactory()
    out = set()
    for r in program.getRelocationTable().getRelocations():
        try:
            v = mem.getInt(r.getAddress()) & 0xFFFFFFFF
        except Exception:
            continue
        if lo <= v <= hi and lst.getInstructionAt(af.getAddress(hex(v))) is not None:
            out.add(v)
    return out


def fix_noreturn():
    """Ghidra marks some functions as never returning that do return (they end in RET), and then
    cuts every caller's body after the call: the code that runs afterwards is lost (the title
    screen's state change, for one). Clear the flag and the CALL_RETURN overrides, re-flow."""
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.program.model.listing import FlowOverride
    fm, lst = program.getFunctionManager(), program.getListing()
    fixed = 0
    for f in fm.getFunctions(True):
        if f.hasNoReturn() and any(i.getMnemonicString() == "RET" for i in lst.getInstructions(f.getBody(), True)):
            f.setNoReturn(False)
            fixed += 1
    callers, n = set(), 0
    for ins in lst.getInstructions(True):
        if ins.getFlowOverride() != FlowOverride.CALL_RETURN or not ins.getFlowType().isCall():
            continue
        tgt = [fm.getFunctionAt(x) for x in ins.getFlows()]
        if tgt and tgt[0] is not None and not tgt[0].hasNoReturn():
            ins.setFlowOverride(FlowOverride.NONE)
            n += 1
            nxt = ins.getMaxAddress().next()
            if lst.getInstructionAt(nxt) is None:
                DisassembleCommand(nxt, None, True).applyTo(program, MON)
            c = fm.getFunctionContaining(ins.getAddress())
            if c is not None:
                callers.add(c)
    for c in callers:
        CreateFunctionCmd.fixupFunctionBody(program, c, MON)
    print("fix_noreturn: %d functions return after all, %d calls fixed, %d functions re-flowed" % (fixed, n, len(callers)))


def discover():
    """make functions where code is only reached through pointers (Ghidra's analysis misses them);
    returns every pointed-to code address, for use as extra roots"""
    fix_noreturn()
    fm, af = program.getFunctionManager(), program.getAddressFactory()
    targets = reloc_code_targets()
    made = 0
    for v in sorted(targets):
        ad = af.getAddress(hex(v))
        if fm.getFunctionContaining(ad) is None:
            if flat.createFunction(ad, None) is not None:
                made += 1
    print("discover: %d code addresses in the relocation table, %d new functions" % (len(targets), made))
    return targets


def all_roots(with_pointers=True):
    roots = [f.getEntryPoint().getOffset() for f in program.getFunctionManager().getFunctions(True)
             if not f.isExternal() and not f.isThunk()]
    if with_pointers:
        roots += sorted(reloc_code_targets())
    return roots


def closure(roots):
    fm = program.getFunctionManager()
    af = program.getAddressFactory()
    seen, todo = {}, list(roots)
    while todo:
        a = todo.pop()
        if a in seen:
            continue
        t = af.getAddress(hex(a))
        f = fm.getFunctionAt(t) or fm.getFunctionContaining(t)
        if f is None or f.isThunk():
            continue
        seen[a] = f
        for ins in program.getListing().getInstructions(f.getBody(), True):
            ft = ins.getFlowType()
            if ft.isCall() or ft.isJump():
                for x in ins.getFlows():
                    if ft.isCall() or not f.getBody().contains(x):
                        g = fm.getFunctionAt(x) or fm.getFunctionContaining(x)
                        if g is not None and not g.isExternal():
                            todo.append(x.getOffset())
    return seen


def steal_info(f):
    """whole instructions at the entry covering >= 5 bytes, none of them branching"""
    n, raw = 0, []
    for ins in program.getListing().getInstructions(f.getBody(), True):
        if ins.getAddress().getOffset() < f.getEntryPoint().getOffset():
            continue
        if n >= 5:
            break
        if ins.getFlowType().isJump() or ins.getFlowType().isCall() or ins.getFlowType().isTerminal():
            return 0, []
        raw += [b & 0xFF for b in ins.getBytes()]
        n += ins.getLength()
    if not 5 <= n <= 16:
        return 0, []
    # nothing may branch into the bytes the hook overwrites (loops back to the entry's second
    # instruction are common)
    rm, af = program.getReferenceManager(), program.getAddressFactory()
    e = f.getEntryPoint().getOffset()
    for x in range(e + 1, e + n):
        for r in rm.getReferencesTo(af.getAddress(hex(x))):
            if r.getReferenceType().isFlow():
                return 0, []
    return n, raw


def main(roots, out, shards=1):
    funcs = closure(roots)
    known = set(funcs)
    parts, table, failed = [], [], []
    gens, impure = {}, set()
    FuncGen.INDEX = dict((x, i) for i, x in enumerate(sorted(funcs)))
    for a in sorted(funcs):
        f = funcs[a]
        try:
            g = FuncGen(f, known, a)
            parts.append(g.gen())
            gens[a] = g
            if g.impure or stack_switch(g):
                impure.add(a)
        except Unsupported as e:
            failed.append("%s %08x: %s" % (f.getName(), a, e))
            parts.append("void %s(RCtx *c) { rt_unsupported(c,0x%xu,\"%s\"); }" % (fname(a), a, str(e)))
            impure.add(a)
    changed = True   # impure if anything reachable is
    while changed:
        changed = False
        for a, g in gens.items():
            if a not in impure and g.callees & impure:
                impure.add(a)
                changed = True
    lst = program.getListing()
    index = dict((x, i) for i, x in enumerate(sorted(funcs)))
    callee_arrays = []
    for a in sorted(funcs):
        f = funcs[a]
        called = any(lst.getInstructionAt(r.getFromAddress()) is not None and
                     lst.getInstructionAt(r.getFromAddress()).getMnemonicString() == "CALL"
                     for r in program.getReferenceManager().getReferencesTo(f.getEntryPoint()))
        # only hookable where callers really CALL it: some "functions" are shared tails reached by JMP
        # with a local frame on the stack instead of a return address
        n, raw = steal_info(f) if f.getEntryPoint().getOffset() == a and called else (0, [])
        name = f.getName() if f.getEntryPoint().getOffset() == a else "%s%+#x" % (f.getName(), a - f.getEntryPoint().getOffset())
        body = f.getBody()
        cl = sorted(index[x] for x in gens[a].callees) if a in gens else []
        if cl:
            callee_arrays.append("static const uint16_t c_%08x[] = { %s };" % (a, ", ".join(map(str, cl))))
        table.append('    { 0x%xu, %s, "%s", %d, %d, { %s }, 0x%xu, 0x%xu, %s, %d },' % (
            a, fname(a), name, n, int(a not in impure), ", ".join("0x%02x" % b for b in raw) or "0",
            body.getMinAddress().getOffset(), body.getMaxAddress().getOffset() + 1,
            "c_%08x" % a if cl else "0", len(cl)))
    head = "/* Generated by tools/recomp/recomp.py from MEGAMAN.EXE. Do not edit or commit. */\n"
    protos = "".join("void %s(RCtx *c);\n" % fname(a) for a in sorted(funcs))
    d = os.path.dirname(out)
    os.makedirs(d, exist_ok=True)
    for fn in os.listdir(d):   # old shards
        if fn.startswith("recomp_") and fn.endswith((".c", ".h", ".obj")):
            os.remove(os.path.join(d, fn))
    if shards <= 1:
        with open(out, "w", encoding="utf-8") as fp:
            fp.write(head + '#include "recomp_rt.h"\n\n' + protos + "\n")
            fp.write("\n\n".join(parts))
            fp.write("\n\n" + "\n".join(callee_arrays))
            fp.write("\n\nconst RFuncInfo g_recomp_funcs[] = {\n%s\n};\n" % "\n".join(table))
            fp.write("const int g_recomp_nfuncs = %d;\n" % len(funcs))
    else:
        with open(os.path.join(d, "recomp_gen.h"), "w", encoding="utf-8") as fp:
            fp.write(head + '#pragma once\n#include "recomp_rt.h"\n\n' + protos)
        per = (len(parts) + shards - 1) // shards
        for k in range(shards):
            with open(os.path.join(d, "recomp_%03d.c" % k), "w", encoding="utf-8") as fp:
                fp.write(head + '#include "recomp_gen.h"\n\n')
                fp.write("\n\n".join(parts[k * per:(k + 1) * per]) + "\n")
        with open(os.path.join(d, "recomp_table.c"), "w", encoding="utf-8") as fp:
            fp.write(head + '#include "recomp_gen.h"\n\n')
            fp.write("\n".join(callee_arrays) + "\n\n")
            fp.write("const RFuncInfo g_recomp_funcs[] = {\n%s\n};\n" % "\n".join(table))
            fp.write("const int g_recomp_nfuncs = %d;\n" % len(funcs))
    print("recomp: %d functions -> %s (%d files), %d pure" % (len(funcs), d, max(1, shards),
                                                             len(funcs) - len(impure)))
    for x in failed:
        print("  unsupported: " + x)
