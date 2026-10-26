"""Recover __watcall signatures for every function (run inside ghidra_server
via `python gq.py run recover_watcom_params.py`).

Why: the Watcom spec lists EDX/EBX/ECX as preserved (true for this
compiler), and Ghidra's decompiler then refuses to treat them as parameters,
so it shows unaff_EDX etc. A committed signature overrides that, so we infer
one per function and commit it with SourceType.ANALYSIS (user-defined
signatures are never touched).

Inference:
  * register params: forward dataflow over the function's blocks; a read of
    EAX/EDX/EBX/ECX (any sub-register) that may happen before any write marks
    it live-in. Prologue pushes (saving callee-saved regs) are not reads.
    Calls read the callee's already-inferred params (iterated to a fixpoint),
    and define EAX. Watcom fills EAX, EDX, EBX, ECX in order, so the count is
    the highest live-in slot + 1.
  * stack params: 'ret N' => N/4 extra stack params (callee pops). A function
    with no register live-ins that reads stack args but ends in plain 'ret'
    is treated as __cdecl (varargs/clib style).
  * return: some call site reads EAX (or AX/AL) right after the call before
    writing it => int return; reads ST0 before any FLD => float10 return.
"""
from ghidra.program.model.block import SimpleBlockModel
from ghidra.program.model.lang import Register
from ghidra.program.model.data import (FunctionDefinitionDataType, CategoryPath, ParameterDefinitionImpl,
                                       Undefined4DataType, VoidDataType, Float10DataType)
from ghidra.app.cmd.function import ApplyFunctionSignatureCmd
from ghidra.program.model.symbol import SourceType

ARGS = ["EAX", "EDX", "EBX", "ECX"]
lang = program.getLanguage()
BASE = {}
for name in ARGS:
    r = lang.getRegister(name)
    BASE[name] = r
ARGSET = set(ARGS)


def base_name(obj):
    if not isinstance(obj, Register):
        return None
    b = obj.getBaseRegister()
    n = b.getName()
    return n if n in ARGSET else None


def regs(objs):
    out = set()
    for o in objs:
        n = base_name(o)
        if n:
            out.add(n)
    return out


bm = SimpleBlockModel(program)
funcs = [f for f in fm.getFunctions(True) if not f.isThunk() and not f.isExternal()]
info = {}  # entry -> dict(ins=[...], blocks=..., ret_pop=int|None)


def decode(f):
    body = f.getBody()
    blocks = {}
    it = bm.getCodeBlocksContaining(body, monitor)
    while it.hasNext():
        b = it.next()
        inss = []
        ins = listing.getInstructionAt(b.getMinAddress())
        while ins is not None and b.contains(ins.getAddress()):
            inss.append(ins)
            ins = ins.getNext()
        succ = []
        di = b.getDestinations(monitor)
        while di.hasNext():
            d = di.next()
            if d.getFlowType().isCall():
                continue
            if body.contains(d.getDestinationAddress()):
                succ.append(d.getDestinationAddress())
        blocks[b.getMinAddress()] = (inss, succ)
    ret_pop = None
    plain_ret = False
    for inss, _ in blocks.values():
        for ins in inss:
            m = ins.getMnemonicString()
            if m == "RET":
                if ins.getNumOperands() == 1:
                    ret_pop = int(ins.getScalar(0).getValue())
                else:
                    plain_ret = True
    return blocks, ret_pop, plain_ret


for f in funcs:
    try:
        info[f.getEntryPoint()] = decode(f)
    except Exception as e:
        print("decode failed", f.getName(), e)

params = {a: 0 for a in info}  # entry -> number of register params


def call_target(ins):
    flows = ins.getFlows()
    if flows and len(flows) == 1:
        g = fm.getFunctionAt(flows[0])
        if g is not None:
            return g.getEntryPoint()
    return None


def live_in(entry):
    blocks, _, _ = info[entry]
    if entry not in blocks:
        return set()
    # prologue: leading pushes / sub esp before any other instruction
    prologue = set()
    for ins in blocks[entry][0]:
        if ins.getMnemonicString() in ("PUSH", "SUB") :
            prologue.add(ins.getAddress())
        else:
            break
    state_in = {entry: set(ARGS)}
    work = [entry]
    live = set()
    seen_out = {}
    while work:
        b = work.pop()
        u = set(state_in[b])
        inss, succ = blocks[b]
        for ins in inss:
            m = ins.getMnemonicString()
            reads = regs(ins.getInputObjects())
            writes = regs(ins.getResultObjects())
            if m in ("XOR", "SUB") and ins.getNumOperands() == 2 and \
                    str(ins.getDefaultOperandRepresentation(0)) == str(ins.getDefaultOperandRepresentation(1)):
                reads = set()
            if m == "PUSH" and ins.getAddress() in prologue:
                reads = set()
            if ins.getFlowType().isCall():
                t = call_target(ins)
                if t is not None and t in params:
                    reads |= set(ARGS[:params[t]])
                writes = writes | {"EAX"}
            elif ins.getFlowType().isJump() and not ins.getFlowType().isConditional():
                t = call_target(ins)
                if t is not None and t != entry and t in params:  # tail call
                    reads |= set(ARGS[:params[t]])
            live |= reads & u
            u -= writes
        for s in succ:
            if s not in blocks:
                continue
            old = state_in.get(s)
            new = (old | u) if old is not None else set(u)
            if old is None or new != old:
                state_in[s] = new
                work.append(s)
    return live


for it_no in range(4):
    changed = 0
    for entry in info:
        lv = live_in(entry)
        n = max([ARGS.index(r) + 1 for r in lv] or [0])
        if n != params[entry]:
            params[entry] = max(n, params[entry])
            changed += 1
    print("pass", it_no, "changed", changed)
    if not changed:
        break

# ---- return values from call sites ----
ret_int, ret_flt = set(), set()
FPU_READS_ST0 = ("FST", "FSTP", "FADD", "FMUL", "FSUB", "FDIV", "FCOM", "FIST", "FXCH", "FCHS", "FABS", "FSUBR", "FDIVR")
# A call ends a basic block, so walk the listing linearly after each call
# (fallthrough is what matters here) instead of the block's instruction list.
text = program.getMemory().getBlock("AUTO")
for ins in listing.getInstructions(text.getStart(), True):
    if not text.contains(ins.getAddress()):
        break
    if not ins.getFlowType().isCall():
        continue
    t = call_target(ins)
    if t is None:
        continue
    fpu_done = False
    nxt = ins.getNext()
    for _ in range(10):
        if nxt is None:
            break
        m = nxt.getMnemonicString()
        rd = regs(nxt.getInputObjects())
        wr = regs(nxt.getResultObjects())
        if m in ("XOR", "SUB") and str(nxt.getDefaultOperandRepresentation(0)) == str(nxt.getDefaultOperandRepresentation(1)):
            rd = set()
        if not fpu_done and m.startswith("F"):
            if m.startswith(FPU_READS_ST0):
                ret_flt.add(t)
            fpu_done = True
        if "EAX" in rd:
            ret_int.add(t)
            break
        ft = nxt.getFlowType()
        if "EAX" in wr or ft.isCall() or ft.isJump() or ft.isTerminal():
            break
        nxt = nxt.getNext()

# ---- commit ----
u4 = Undefined4DataType()
committed = skipped = cdecl = 0
for f in funcs:
    e = f.getEntryPoint()
    if e not in info:
        continue
    if f.getSignatureSource() == SourceType.USER_DEFINED:
        skipped += 1
        continue
    _, ret_pop, plain_ret = info[e]
    nreg = params[e]
    nstack = (ret_pop // 4) if ret_pop else 0
    conv = "__watcall"
    if nstack and nreg < 4:
        nreg = 4  # stack args only appear after all four registers are used
    has_frame_params = f.getStackFrame().getParameterSize() > 0
    if nreg == 0 and plain_ret and has_frame_params:
        conv = "__cdecl"
        nstack = f.getStackFrame().getParameterSize() // 4
        cdecl += 1
    sig = FunctionDefinitionDataType(CategoryPath.ROOT, f.getName(), program.getDataTypeManager())
    ps = [ParameterDefinitionImpl("p%d" % (i + 1), u4, None) for i in range(nreg + nstack if conv == "__watcall" else nstack)]
    sig.setArguments(ps)
    if e in ret_flt:
        sig.setReturnType(Float10DataType())
    elif e in ret_int:
        sig.setReturnType(u4)
    else:
        sig.setReturnType(VoidDataType())
    sig.setCallingConvention(conv)
    cmd = ApplyFunctionSignatureCmd(e, sig, SourceType.ANALYSIS)
    if cmd.applyTo(program, monitor):
        committed += 1
print("committed", committed, "user-defined kept", skipped, "cdecl", cdecl,
      "int returns", len(ret_int), "float returns", len(ret_flt))
from collections import Counter
print("register param histogram", sorted(Counter(params.values()).items()))
