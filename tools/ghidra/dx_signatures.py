"""Signatures for the DirectX entry points MEGAMAN.EXE imports (Ghidra's
bundled Windows archive doesn't cover DirectX). Run via `gq.py run`."""
from java.util import ArrayList
from ghidra.program.model.data import PointerDataType, VoidDataType, LongDataType, UnsignedLongDataType
from ghidra.program.model.listing import ParameterImpl, ReturnParameterImpl
from ghidra.program.model.listing.Function import FunctionUpdateType
from ghidra.program.model.symbol import SourceType

P = PointerDataType()
PP = PointerDataType(PointerDataType())
PROTOS = {
    "DirectDrawCreate": [("lpGUID", P), ("lplpDD", PP), ("pUnkOuter", P)],
    "DirectDrawEnumerateA": [("lpCallback", P), ("lpContext", P)],
    "DirectSoundCreate": [("lpGuid", P), ("ppDS", PP), ("pUnkOuter", P)],
    "DirectInputCreateA": [("hinst", P), ("dwVersion", UnsignedLongDataType()), ("ppDI", PP), ("punkOuter", P)],
}
thunks = {}
for f in fm.getFunctions(True):
    if f.isThunk():
        t = f.getThunkedFunction(True)
        if t is not None and t.getName() in PROTOS:
            thunks[t.getName()] = t
for name, params in PROTOS.items():
    t = thunks.get(name)
    if t is None:
        print("no thunk for", name)
        continue
    plist = ArrayList()
    for pname, dt in params:
        plist.add(ParameterImpl(pname, dt, program))
    t.updateFunction("__stdcall", ReturnParameterImpl(LongDataType(), program), plist,
                     FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, True, SourceType.USER_DEFINED)
    print(name, "->", t.getSignature().getPrototypeString())
