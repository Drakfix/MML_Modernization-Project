"""Install a Watcom compiler spec into a Ghidra install (idempotent).

MEGAMAN.EXE was built with Watcom C, whose default __watcall convention passes
the first four integer args in EAX, EDX, EBX, ECX (rest on the stack, callee
pops) and preserves every register except the return value. Stock Ghidra has
no such spec, so its decompiler loses parameters and invents register
garbage. This derives x86watcom.cspec from x86win.cspec with __watcall as the
default prototype and registers it as compiler id "watcom" for x86:LE:32:default.

Usage: python install_watcom_spec.py [GHIDRA_INSTALL_DIR]
"""
import os
import re
import sys

WATCALL = """  <default_proto>
    <prototype name="__watcall" extrapop="unknown" stackshift="4">
      <input>
        <pentry minsize="1" maxsize="4">
          <register name="EAX"/>
        </pentry>
        <pentry minsize="1" maxsize="4">
          <register name="EDX"/>
        </pentry>
        <pentry minsize="1" maxsize="4">
          <register name="EBX"/>
        </pentry>
        <pentry minsize="1" maxsize="4">
          <register name="ECX"/>
        </pentry>
        <pentry minsize="1" maxsize="500" align="4">
          <addr offset="4" space="stack"/>
        </pentry>
      </input>
      <output killedbycall="true">
        <pentry minsize="4" maxsize="10" metatype="float" extension="float">
          <register name="ST0"/>
        </pentry>
        <pentry minsize="1" maxsize="4">
          <register name="EAX"/>
        </pentry>
        <pentry minsize="5" maxsize="8">
          <addr space="join" piece1="EDX" piece2="EAX"/>
        </pentry>
      </output>
      <unaffected>
        <varnode space="ram" offset="0" size="4"/>
        <register name="ESP"/>
        <register name="EBP"/>
        <register name="ESI"/>
        <register name="EDI"/>
        <register name="EBX"/>
        <register name="ECX"/>
        <register name="EDX"/>
        <register name="DF"/>
        <register name="FS_OFFSET"/>
      </unaffected>
      <killedbycall>
        <register name="ST0"/>
        <register name="ST1"/>
      </killedbycall>
    </prototype>
  </default_proto>
"""

LDEFS_LINE = '    <compiler name="Watcom" spec="x86watcom.cspec" id="watcom"/>\n'


def main():
    ghidra = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
        "GHIDRA_INSTALL_DIR", r"C:\tools\ghidra_12.1.2_PUBLIC")
    lang = os.path.join(ghidra, "Ghidra", "Processors", "x86", "data", "languages")
    src = open(os.path.join(lang, "x86win.cspec"), encoding="utf-8").read()

    # Demote the stock default (__stdcall) to an ordinary prototype and put
    # __watcall in front as the default.
    m = re.search(r"  <default_proto>\n(.*?)  </default_proto>\n", src, re.S)
    if not m:
        sys.exit("x86win.cspec layout not recognised")
    out = src[:m.start()] + WATCALL + m.group(1) + src[m.end():]
    with open(os.path.join(lang, "x86watcom.cspec"), "w", encoding="utf-8") as f:
        f.write(out)

    ldefs_path = os.path.join(lang, "x86.ldefs")
    ldefs = open(ldefs_path, encoding="utf-8").read()
    if 'id="watcom"' not in ldefs:
        # First language block is x86:LE:32:default; add after its golang entry.
        anchor = '<compiler name="golang" spec="x86-32-golang.cspec" id="golang"/>\n'
        i = ldefs.index(anchor) + len(anchor)
        ldefs = ldefs[:i] + LDEFS_LINE + ldefs[i:]
        with open(ldefs_path, "w", encoding="utf-8") as f:
            f.write(ldefs)
        print("registered compiler id 'watcom' in x86.ldefs")
    print("wrote", os.path.join(lang, "x86watcom.cspec"))


if __name__ == "__main__":
    main()
