#!/usr/bin/env python3
"""One-shot umbrella-split for FM2K_Integration.h (1309 lines).

Zero-churn for the 36 includers: FM2K_Integration.h keeps the namespace FM2K
engine layout + config structs + forward decls, then #includes two new class-
declaration headers in the ORIGINAL order (FM2KLauncher before LauncherUI, so
FM2KLauncher still sees only the LauncherUI forward-decl -- behaviour-identical
to the single-file layout, where its unique_ptr<LauncherUI> dtor is already
out-of-line in the .cpp).
  FM2K_Launcher_decl.h    class FM2KLauncher   (orig 569-766)
  FM2K_LauncherUI_decl.h  class LauncherUI     (orig 768-1309)
Ranges 1-based inclusive, verified against the read.
"""
import pathlib

SRC = pathlib.Path("FM2K_Integration.h")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 1200 or not any(ln.startswith("class FM2KLauncher {") for ln in L):
    raise SystemExit("FM2K_Integration.h is not the original monolith "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

SUBHDR_NOTE = (
    "// NOTE: included ONLY via FM2K_Integration.h (umbrella). Relies on that\n"
    "// header's includes + the namespace FM2K layout / config structs / the\n"
    "// `class LauncherUI;` forward-decl being in scope -- not standalone.\n")

# ---- FM2K_Launcher_decl.h ----
pathlib.Path("FM2K_Launcher_decl.h").write_text(
    "#pragma once\n" + SUBHDR_NOTE + "\n" + R(569, 766) + "\n")

# ---- FM2K_LauncherUI_decl.h ----
pathlib.Path("FM2K_LauncherUI_decl.h").write_text(
    "#pragma once\n" + SUBHDR_NOTE + "\n" + R(768, len(L)) + "\n")

# ---- umbrella: keep 1-568, then include the two class headers in order ----
umbrella = R(1, 568) + (
    "\n"
    "// ---------------------------------------------------------------------------\n"
    "// Launcher app classes -- split out of this header (umbrella include). Order\n"
    "// matters: FM2KLauncher first (it only needs the LauncherUI forward-decl\n"
    "// above for its unique_ptr member), LauncherUI second.\n"
    "// ---------------------------------------------------------------------------\n"
    '#include "FM2K_Launcher_decl.h"\n'
    '#include "FM2K_LauncherUI_decl.h"\n'
)
SRC.write_text(umbrella)

print("split done:")
for f in ("FM2K_Integration.h", "FM2K_Launcher_decl.h", "FM2K_LauncherUI_decl.h"):
    print(f"  {f:26s} {sum(1 for _ in open(f))} lines")
