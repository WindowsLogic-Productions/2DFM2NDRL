#!/usr/bin/env python3
"""One-shot splitter for FM2K_GameInstance.cpp (925 lines).

Pure member-fn move (class in FM2K_GameInstance.h, no internal header):
  FM2K_GameInstance.cpp      process management: lifecycle + spawn + DLL injection
                             (ctor/dtor/Initialize + Launch + Terminate +
                              Install/UninstallHooks + SetupProcessForHooking +
                              LoadGameExecutable + ExecuteRemoteFunction + the
                              anon-ns env helpers, which only these methods use)
  FM2K_GameInstance_ipc.cpp  runtime IPC surface: rollback ops + DLL-event pump +
                             shared memory + the 21 Trigger/Set/Get/Step debug cmds
Ranges 1-based inclusive, verified against the read.
"""
import pathlib

SRC = pathlib.Path("FM2K_GameInstance.cpp")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 850 or not any("FM2KGameInstance::Launch(" in ln for ln in L):
    raise SystemExit("FM2K_GameInstance.cpp is not the original monolith "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

# ---- IPC TU ----
ipc = []
ipc.append("// FM2K_GameInstance_ipc.cpp -- runtime IPC surface (split from\n")
ipc.append("// FM2K_GameInstance.cpp): rollback save/load/advance/inject, the DLL-event\n")
ipc.append("// pump, shared-memory setup, and the Trigger/Set/Get/Step debug commands.\n")
ipc.append("// Member fns of FM2KGameInstance (class in FM2K_GameInstance.h).\n")
ipc.append(R(1, 14))   # include block (FM2K_GameInstance.h + shared_mem.h + SDL + windows + std)
ipc.append("\n")
ipc.append(R(450, 495))   # SaveState / LoadState / AdvanceFrame / InjectInputs
ipc.append(R(650, 723))   # ProcessDLLEvents / SetNetworkConfig / HandleDLLEvent
ipc.append(R(748, len(L)))  # Init/CleanupSharedMemory + Poll + Trigger*/Set*/Get*/Step*
pathlib.Path("FM2K_GameInstance_ipc.cpp").write_text("".join(ipc))

# ---- core (overwrites SRC): everything except the IPC ranges ----
core = R(1, 449) + R(496, 649) + R(724, 747)
SRC.write_text(core)

print("split done:")
for f in ("FM2K_GameInstance.cpp", "FM2K_GameInstance_ipc.cpp"):
    print(f"  {f:30s} {sum(1 for _ in open(f))} lines")
