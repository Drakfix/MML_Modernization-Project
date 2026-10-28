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
- Direct calls become C calls. Jumps into the middle of another function become extra entry
  points. Indirect calls and jumps go through the runtime.

## Usage

With the Ghidra server running (see [tools/ghidra/README.md](../ghidra/README.md)):

```
python tools/ghidra/gq.py py "exec(open(r'tools/recomp/recomp.py').read()); main(roots=[0x42961B], out=r'native/build/gen/recomp_gen.c')"
native\build.bat
```

`roots` are function addresses; every function they reach through calls and jumps is
included. `native\build.bat` compiles `recomp_gen.c` into `mml.dll` when it exists.

## Testing against the original

`mml.dll` contains a test harness ([native/src/recomp.c](../../native/src/recomp.c)), enabled
in `mml.ini`:

```
[debug]
recomp=diff          ; or replace
recomp_roots=all     ; or a list of function addresses: 4EF638,4F915C
```

| Mode | Behaviour |
|------|-----------|
| `diff` | Each hooked function runs as recompiled C first, with every memory write logged. The writes are undone, the original runs, and registers, memory and the x87 stack are compared. Results go to `mml.log`. |
| `replace` | The recompiled function runs instead of the original. |

Only functions that callers reach with a `CALL` can be hooked. Shared code that other
functions enter by jumping is tested through the functions that use it.

## Status

The camera and matrix math (18 functions) matched the original on all 10.7 million calls of
a test session (boot, title, Downtown), and the game plays normally with them replaced.
Next: the whole executable, including indirect calls, the game's cooperative threads and
calls into Windows.
