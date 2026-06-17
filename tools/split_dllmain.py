#!/usr/bin/env python3
"""One-shot splitter for FM2KHook/src/core/dllmain.cpp (1030 lines).

Pure behaviour-preserving move: extracts the async-logging + crash-handler
section (Fm2k_BuildLogPath, LogOutputFunction, InitFileLogging, CrashHandler,
VectoredRenderGuard, InstallCrashHandler, ShutdownFileLogging) into
dllmain_logging.cpp. Boot patches STAY in core with DllMain -- 3 of them
(BypassMultiInstanceCheck / ApplyBootToCharacterSelectPatches /
ApplyCharacterSelectModePatches) are intentional `static` duplicates of
game_patches.cpp symbols, so un-static'ing them would multiply-define at link.
DllMain calls the 3 logging entry points via dllmain_internal.h.
Ranges 1-based inclusive, verified against the read.
"""
import pathlib

SRC = pathlib.Path("FM2KHook/src/core/dllmain.cpp")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 950 or not any("InitFileLogging()" in ln for ln in L):
    raise SystemExit("dllmain.cpp is not the original monolith "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

INCLUDES = R(1, 49)
INT_INC  = '#include "dllmain_internal.h"\n'

def with_int_inc(text):
    # insert the internal-header include right after globals.h
    return text.replace('#include "globals.h"\n',
                        '#include "globals.h"\n' + INT_INC, 1)

# ---- logging TU ----
log = []
log.append('// dllmain_logging.cpp -- async (quill) file logging + crash/SEH handlers.\n')
log.append('// Split VERBATIM from dllmain.cpp. Init/Install/Shutdown are the entry\n')
log.append('// points DllMain calls (declared in dllmain_internal.h); the logger state\n')
log.append('// + LogOutputFunction/CrashHandler/VectoredRenderGuard stay file-local.\n')
log_text = with_int_inc(INCLUDES) + "\n" + R(50, 495)
for fn in ("InitFileLogging", "InstallCrashHandler", "ShutdownFileLogging"):
    log_text = log_text.replace(f"static void {fn}()", f"void {fn}()", 1)
pathlib.Path("FM2KHook/src/core/dllmain_logging.cpp").write_text(
    "".join(log) + log_text)

# ---- core (overwrites SRC) ----
core = with_int_inc(INCLUDES) + "\n" + R(496, len(L))
SRC.write_text(core)

print("split done:")
for f in ("dllmain.cpp", "dllmain_logging.cpp"):
    p = pathlib.Path("FM2KHook/src/core") / f
    print(f"  {f:24s} {sum(1 for _ in p.open())} lines")
