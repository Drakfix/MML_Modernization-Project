"""Static recompiler: x86 functions of MEGAMAN.EXE to C, from Ghidra's P-code.

Runs inside the Ghidra query server (tools/ghidra), which has the analysed
program open:

    python tools/ghidra/gq.py py "exec(open(r'tools/recomp/recomp.py').read()); main(roots=[0x42961B], out=r'native/build/gen/recomp_gen.c')"

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


def ctype(size):
    if size == 10:
        return "double"
    if size in INT_T:
        return INT_T[size]
    raise Unsupported("varnode size %d" % size)


ST0, ST1, ST2 = 0x1100, 0x1110, 0x1120


def x87_delta(pcode):
    """net x87 pushes of one instruction: Ghidra models the stack as fixed registers,
    a push is the chain ST1<-ST0, ST2<-ST1..., a pop the chain ST0<-ST1, ST1<-ST2..."""
    moves = set()
    for p in pcode:
        if p.getOpcode() == PcodeOp.COPY and p.getNumInputs() == 1:
            o, i = p.getOutput(), p.getInput(0)
            if (o.getAddress().getAddressSpace().getName() == "register" and
                    i.getAddress().getAddressSpace().getName() == "register" and o.getSize() == 10):
                moves.add((o.getOffset(), i.getOffset()))
    d = 0
    if (ST1, ST0) in moves and (ST2, ST1) in moves:
        d += 1
    if (ST0, ST1) in moves and (ST1, ST2) in moves:
        d -= 1
    return d


class FuncGen:
    def __init__(self, func, known, entry=None):
        self.f = func
        self.entry = entry if entry is not None else func.getEntryPoint().getOffset()
        self.known = known            # entry addresses of every function we emit
        self.locals = {}              # (offset, size) -> name
        self.labels = set()           # instruction addresses that need a label
        self.plabels = set()          # (instruction address, op index) inside one instruction
        self.lines = []
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
            if size not in INT_T:
                raise Unsupported("register size %d" % size)
            return "R%d(c,0x%x)" % (size * 8, off)
        if sp == "unique":
            ctype(size)
            return self.local(off, size)
        if sp == "ram":
            if size == 10:
                return "rt_ld80(0x%xu)" % off
            return "LD%d(0x%xu)" % (size * 8, off)
        raise Unsupported("space " + sp)

    def wr(self, v, expr):
        sp = v.getAddress().getAddressSpace().getName()
        off, size = v.getOffset(), v.getSize()
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
        if target in self.addrs:
            self.labels.add(target)
            return "goto L_%08x;" % target
        if target in self.known:
            return "{ %s(c); return; }" % fname(target)   # tail jump into another function
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
            if s == 10:
                return self.wr(out, "rt_ld80(%s)" % rd(ins_[1]))
            return self.wr(out, "LD%d(%s)" % (s * 8, rd(ins_[1])))
        if o == PcodeOp.STORE:
            v = ins_[2]
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
            return "%s(c);" % fname(t) if t in self.known else "rt_call(c,0x%xu);" % t
        if o == PcodeOp.CALLIND:
            return "rt_call(c,%s);" % rd(ins_[0])
        if o == PcodeOp.RETURN:
            return "c->ret = %s; return;" % rd(ins_[0])
        if o == PcodeOp.CALLOTHER:
            name = program.getLanguage().getUserDefinedOpName(int(ins_[0].getOffset()))
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
            return self.wr(out, "(%s)(%s)(%s)%s" % (T, SINT_T[s], SINT_T[isz], a))
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
            return "{ rt_jump(c,%s); return; }" % expr
        return "switch (%s) { %s default: rt_jump(c,%s); return; }" % (expr, " ".join(cases), expr)

    # ---- the function ----
    def gen(self):
        body = []
        insns = self.insns
        for n, ins in enumerate(insns):
            a = ins.getAddress().getOffset()
            body.append(("I", a, "/* %08x  %s */" % (a, str(ins).replace("*/", "* /"))))
            pc = list(ins.getPcode())
            d = x87_delta(pc)
            if d:
                body.append(("P", (a, -1), "c->fdelta += %d;" % d))
            for i, p in enumerate(pc):
                body.append(("P", (a, i), self.op(p, ins, i)))
            body.append(("P", (a, len(pc)), ""))   # target for branches past the last op
            ft = ins.getFallThrough()
            nxt = insns[n + 1].getAddress().getOffset() if n + 1 < len(insns) else None
            if ft is not None and ft.getOffset() != nxt:
                body.append(("X", None, self.goto_ram(ft.getOffset(), ins)))
        out = ["void %s(RCtx *c)" % fname(self.entry), "{"]
        for (off, size), name in sorted(self.locals.items()):
            out.append("    %s %s = 0;" % (ctype(size), name))
        if insns and insns[0].getAddress().getOffset() != self.entry:
            self.labels.add(self.entry)
            out.append("    goto L_%08x;" % self.entry)
        for kind, key, text in body:
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
    return (n, raw) if 5 <= n <= 16 else (0, [])


def main(roots, out):
    funcs = closure(roots)
    known = set(funcs)
    parts, table, failed = [], [], []
    for a in sorted(funcs):
        f = funcs[a]
        try:
            parts.append(FuncGen(f, known, a).gen())
        except Unsupported as e:
            failed.append("%s %08x: %s" % (f.getName(), a, e))
            parts.append("void %s(RCtx *c) { rt_unsupported(c,0x%xu,\"%s\"); }" % (fname(a), a, str(e)))
        lst = program.getListing()
        called = any(lst.getInstructionAt(r.getFromAddress()) is not None and
                     lst.getInstructionAt(r.getFromAddress()).getMnemonicString() == "CALL"
                     for r in program.getReferenceManager().getReferencesTo(f.getEntryPoint()))
        # only hookable where callers really CALL it: some "functions" are shared tails reached by JMP
        # with a local frame on the stack instead of a return address
        n, raw = steal_info(f) if f.getEntryPoint().getOffset() == a and called else (0, [])
        name = f.getName() if f.getEntryPoint().getOffset() == a else "%s%+#x" % (f.getName(), a - f.getEntryPoint().getOffset())
        table.append('    { 0x%xu, %s, "%s", %d, { %s } },' % (a, fname(a), name, n,
                                                              ", ".join("0x%02x" % b for b in raw) or "0"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fp:
        fp.write("/* Generated by tools/recomp/recomp.py from MEGAMAN.EXE. Do not edit or commit. */\n")
        fp.write('#include "recomp_rt.h"\n\n')
        for a in sorted(funcs):
            fp.write("void %s(RCtx *c);\n" % fname(a))
        fp.write("\n")
        fp.write("\n\n".join(parts))
        fp.write("\n\nconst RFuncInfo g_recomp_funcs[] = {\n%s\n};\n" % "\n".join(table))
        fp.write("const int g_recomp_nfuncs = %d;\n" % len(funcs))
    print("recomp: %d functions -> %s" % (len(funcs), out))
    for x in failed:
        print("  unsupported: " + x)
