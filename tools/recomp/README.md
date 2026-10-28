# Static recompiler

Translates the x86 code of `MEGAMAN.EXE` into C, as the basis for running the game on
platforms that can't execute the original executable (Android, Linux).

The generated C is derived from the game's code. It is written to `native/build/gen/` and
is never committed.

## How it works

- `recomp.py` runs inside the Ghidra query server ([tools/ghidra](../ghidra)), which has the
  analysed executable open.
- Each x86 function becomes one C function over a guest CPU state (`RCtx`, see
  [native/src/recomp_rt.h](../../native/src/recomp_rt.h)).
- Instructions are translated through Ghidra's P-code, which describes the exact effect of
  every instruction, including flags and the x87 FPU. Each P-code operation becomes one C
  statement.
- Registers live in a byte array laid out like Ghidra's register space, so partial registers
  (AL, AX, EAX) overlap as on the real CPU. Guest memory is accessed through `LD`/`ST`
  helpers that take 32-bit guest addresses.
- x87 values are kept as doubles. The FPU control word's precision and rounding settings are
  applied, because the game depends on both.
- Direct calls become C calls. A jump out of a function is a tail call: the function returns
  the target address to a dispatcher loop (`rt_dispatch`), so the C stack stays flat however
  long the game's code keeps jumping between functions. Jumps into the middle of another
  function become extra entry points. Indirect calls and jumps go through the runtime.
- `mml.dll`'s own changes to the game's code are translated in. With a patch list
  (`patch_sites.txt`), each hook point becomes a call to the hook's handler (`rt_hook`), and
  each patched byte range is read from memory or follows the patched branch. The translated
  code is the modded game, so no original machine code is needed.

## Usage

With the Ghidra server running (see [tools/ghidra/README.md](../ghidra/README.md)):

```
python tools/ghidra/gq.py py "RECOMP_FILE='tools/recomp/recomp.py'; exec(open(RECOMP_FILE).read()); main(roots=all_roots(), out='native/build/gen/recomp_gen.c', shards=96, patches='tools/recomp/patch_sites.txt')"
native\build.bat
```

`roots` are function addresses (`all_roots()` is the whole executable); every function they
reach through calls and jumps is included. `shards` splits the output into that many files
for a parallel build. `native\build.bat` compiles the generated files into `mml.dll` when
they exist. The whole executable takes about 10 minutes to translate and 3 to build.

The Ghidra project needs two one-time passes first, `discover()` and `jump_orphans()` (see
[Finding all the code](#finding-all-the-code)). Both change the project and are safe to run
again.

Translating the whole executable is memory-heavy. The Ghidra server limits its own memory
(an 8 GB Java heap and 16 GB for the process; `MML_GHIDRA_HEAP` and `MML_GHIDRA_MAXMEM`
change them). Use a freshly started server for each full translation.

## Testing against the original

`mml.dll` contains a test harness ([native/src/recomp.c](../../native/src/recomp.c)), enabled
in `mml.ini`:

```
[debug]
recomp=diff          ; or replace
recomp_roots=all     ; or a list of function addresses: 4EF638,4F915C
recomp_native=       ; address ranges to keep as machine code: 401000-44FFFF (bisecting)
```

| Mode | Behaviour |
|------|-----------|
| `diff` | Each hooked function runs as recompiled C first, with every memory write logged. The writes are undone, the original runs, and registers, memory and the x87 stack are compared. Results go to `mml.log`. |
| `replace` | The recompiled function runs instead of the original. The game's threads (its scheduler at `0x4EED08` and the create, yield, exec and exit primitives) run on host fibers. Calls from recompiled code to Windows, to `mml.dll` and to functions our mods patch go through a native bridge. |

Only functions that callers reach with a `CALL` can be hooked. Shared code that other
functions enter by jumping is tested through the functions that use it.

## Finding all the code

Ghidra's analysis misses code that the game only reaches in unusual ways. Two passes repair
the project before translating:

- `discover()` puts the installer's code patches into the project (it holds the vanilla
  executable), clears wrong "never returns" flags that cut functions short after a call, and
  creates functions at code addresses from the relocation table (handlers only reached
  through pointer tables).
- `jump_orphans()` creates functions at code that no function contains: call and jump
  targets, fall-throughs off the end of a body, and pointer targets that Ghidra never
  disassembled. It also dissolves the "functions" Ghidra made out of jump tables. Those were
  decoded as instructions and ran misaligned into the real code after the table.

Without these passes, recompiled code reaches addresses that have no translation and falls
back to machine code, which the ports don't have.

## Status

- The whole executable translates: 11,224 entry points.
- Diff mode: every pure function that our mods don't touch (980) matched the original on
  27.9 million calls, and the 90 pure GTE and vertex functions (`0x4EE201` to `0x4F25C0`)
  on 22 million more.
- Replace mode, with `mml.dll`'s mods translated in: every hookable function (2,026 roots)
  is replaced, including the ones our mods change, and the game's cooperative threads run
  on host fibers. The game plays correctly in Downtown, the Forest, City Hall and the first
  dungeon, at 30 and 60 fps, with no original machine code apart from calls into Windows.
- It is slow: the game thread needs about three times the CPU time of the original code, so
  busy scenes fall below the frame rate. The game then skips drawing frames, as it was
  designed to; see [Falling behind](#falling-behind).
- Known gaps: 7 pointer targets overlap instructions of other functions (bytes with two
  valid decodings, which Ghidra can't represent), and 3 entries are not code. None of them
  are reached on the tested routes.

Next: make the generated code fast (the x87 stack resolved at translation time, flags only
where something reads them, registers in local variables, a direct dispatch table, cheaper
calls into machine code), then a portable runtime without Windows (M3).

## Falling behind

The game's frame loop runs one logic step per frame and draws only if it is still on
schedule. When it is late, it skips the drawing and runs the next logic step straight away.
The original game has no limit on this, and a few pieces of state are only cleaned up by a
draw. `mml.dll` (`frame_drop_check` in `native/src/mml.c`) makes this safe in every build:

- At most 2 draws are skipped in a row (`[debug] max_skipped_draws`). After that the game
  draws and drops the time it lost, so an overloaded game slows down instead of freezing its
  image, and doesn't race to catch up afterwards.
- On a skipped draw it does what the draw would have done: flip the frame parity, and clear
  the deferred list of shadows and translucent planes (a bug in the original game: they stayed
  queued, pointing into reused memory). Our own per-frame state (60 fps interpolation, the
  smooth sky, the near-plane texture fix) drops the undrawn step as well.

`[debug] slow_logic_ms=N` keeps the game thread busy for N ms in every logic step, which makes
the normal game fall behind the same way; `slow_ms=N` does the same per drawn frame.
