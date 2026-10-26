"""Create the decomp Ghidra project from the vanilla exe and auto-analyse it.

Project: <game>/ghidra_decomp/mml_pc.gpr (override with MML_GHIDRA_DIR),
program "MEGAMAN.EXE" imported from <game>/MEGAMAN.EXE.orig (vanilla bytes; our patches are runtime/patcher
concerns, the decomp targets the original code). Language x86:LE:32:default
with the Watcom compiler spec from install_watcom_spec.py.

Never commit the project directory: it contains Capcom's code.
"""
import os
import time

import pyghidra

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("MML_GAME_DIR")
if not GAME:
    raise SystemExit("Set MML_GAME_DIR to your game folder (with MEGAMAN.EXE.orig).")
PROJ_DIR = os.environ.get("MML_GHIDRA_DIR", os.path.join(GAME, "ghidra_decomp"))
EXE = os.path.join(GAME, "MEGAMAN.EXE.orig")

os.environ.setdefault("GHIDRA_INSTALL_DIR", r"C:\tools\ghidra_12.1.2_PUBLIC")
t0 = time.time()
pyghidra.start()

from ghidra.base.project import GhidraProject
from ghidra.program.model.lang import LanguageID, CompilerSpecID
from ghidra.program.util import DefaultLanguageService
from ghidra.app.script import GhidraScriptUtil
from ghidra.util.task import ConsoleTaskMonitor
from java.io import File

os.makedirs(PROJ_DIR, exist_ok=True)
lang = DefaultLanguageService.getLanguageService().getLanguage(LanguageID("x86:LE:32:default"))
cspec = lang.getCompilerSpecByID(CompilerSpecID("watcom"))

proj = GhidraProject.createProject(PROJ_DIR, "mml_pc", False)
try:
    program = proj.importProgram(File(EXE), lang, cspec)
    program.setName("MEGAMAN.EXE")
    print("imported in %.1fs, cspec=%s" % (time.time() - t0, program.getCompilerSpec().getCompilerSpecID()))

    from ghidra.program.util import GhidraProgramUtilities
    from ghidra.app.plugin.core.analysis import AutoAnalysisManager
    mgr = AutoAnalysisManager.getAnalysisManager(program)
    tx = program.startTransaction("auto-analysis")
    try:
        mgr.initializeOptions()
        mgr.reAnalyzeAll(None)
        mgr.startAnalysis(ConsoleTaskMonitor())
        GhidraProgramUtilities.markProgramAnalyzed(program)
    finally:
        program.endTransaction(tx, True)
    print("analysed in %.1fs, functions=%d" % (time.time() - t0, program.getFunctionManager().getFunctionCount()))
    proj.saveAs(program, "/", "MEGAMAN.EXE", True)
    print("saved")
finally:
    proj.close()
