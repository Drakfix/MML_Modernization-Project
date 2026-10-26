# Ghidra decomp tooling

Everything here is our own code. The Ghidra project it builds
(`<game folder>/ghidra_decomp/` by default, or `MML_GHIDRA_DIR`) contains
Capcom's code and must never be committed or shared.

## One-time setup

Set `MML_GAME_DIR` to your game folder first. It needs `MEGAMAN.EXE.orig`, your
unmodified exe.

1. `python install_watcom_spec.py` adds a Watcom compiler spec (`__watcall`:
   args in EAX, EDX, EBX, ECX then stack, callee pops, everything but the
   return value preserved) to the Ghidra install as compiler id `watcom`.
   The original `x86.ldefs` is backed up next to it as `x86.ldefs.orig_backup`.
2. `python create_project.py` imports `MEGAMAN.EXE.orig` (vanilla) with that
   spec and runs auto-analysis (~4 min).
3. Start the server (`python -u ghidra_server.py`) and leave it running.
4. `python gq.py run fixups.py` (memory map + function discovery, see below).
5. `python gq.py run recover_watcom_params.py` (signatures, see below).
6. `python gq.py symbols symbols.csv` (our names).

### Fix-ups the stock loader/analyzer gets wrong

* **.bss is truncated.** Every section header has VirtualSize 0, so Windows
  maps SizeOfRawData; Ghidra stopped `.bss` at 0x6C55FF. The real end is
  0x8985FF, and most engine globals (0x77xxxx, 0x88xxxx) live past the cut.
  The block was extended to 0x898FFF and analysis re-run.
* **25% of the code was never disassembled**: functions reached only through
  pointer tables. Every relocation whose target lands in undisassembled code
  became a function entry (1972 new functions). Coverage 75% -> 95.5%,
  4306 functions. Cross-check: 260 refs to the packet arena cursor, same as
  the earlier manual byte-scan audit.
* **Parameters.** Because EDX/EBX/ECX are (correctly) callee-saved, the
  decompiler won't treat them as parameters on its own. `recover_watcom_params.py`
  infers each function's register params (read-before-write dataflow,
  prologue pushes ignored, callee params propagated to a fixpoint), stack
  params from `ret N`, and int/float returns from how call sites use
  EAX/ST0, then commits signatures as ANALYSIS. User-defined signatures are
  never overwritten, so re-running it is safe.

## Daily use

```
python -u ghidra_server.py          # keeps the project open (holds the lock)
python gq.py decomp cam_apply_angles
python gq.py xrefs gfx_arena_cursor
python gq.py rename func:0x4F87F0 gfx_set_op_2c
python gq.py setsig gfx_add_prim "void gfx_add_prim(void **bucket, void *pkt)"
python gq.py symbols symbols.csv    # (re)apply the shared symbol list
python gq.py quit
```

Stop the server before opening the project in the Ghidra GUI (project lock).
`symbols.csv` is the source of truth for names we're confident in; add to it
as functions get identified so the names survive a project rebuild.
