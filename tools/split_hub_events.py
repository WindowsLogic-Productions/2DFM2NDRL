#!/usr/bin/env python3
"""Extract the 358-line MatchStart case out of LauncherUI::HandleHubEvent
(launcher/ui/launcher_ui_hub_events.cpp 953) into a sibling method
LauncherUI::HandleMatchStartEvent in launcher_ui_hub_events_match.cpp.
Behavior-preserving: the method re-derives `hs = *hub_state_` + `using K` and
runs the identical body; the case becomes a one-line dispatch. Decl added to
the LauncherUI class separately. Ranges 1-based inclusive, verified.
"""
import pathlib

F = pathlib.Path("launcher/ui/launcher_ui_hub_events.cpp")
L = F.read_text().splitlines(keepends=True)
if len(L) < 900 or not any("case K::MatchStart: {" in l for l in L):
    raise SystemExit("hub_events not original (already split?)")
def R(a, b):
    return "".join(L[a-1:b])

PRE = R(6, 53)  # includes + `using namespace lui;`

# ---- sibling method file ----
match = (
    "// launcher_ui_hub_events_match.cpp -- LauncherUI::HandleMatchStartEvent.\n"
    "// Extracted from HandleHubEvent's 358-line K::MatchStart case (the heaviest\n"
    "// single hub-event handler: drops modals, resolves peer/host roles, seeds\n"
    "// the battle session, kicks the game launch). Re-derives hs/K like the\n"
    "// dispatcher; body is verbatim.\n"
    + PRE + "\n"
    "void LauncherUI::HandleMatchStartEvent(const fm2k::HubEvent& ev) {\n"
    "    auto& hs = *hub_state_;\n"
    "    using K = fm2k::HubEvent::Kind;  // body references K::RecordReceived\n"
    + R(386, 742) +
    "}\n"
)
pathlib.Path("launcher/ui/launcher_ui_hub_events_match.cpp").write_text(match)

# ---- slim hub_events.cpp: replace the case body (385-744) with a dispatch ----
stub = ("            case K::MatchStart:\n"
        "                HandleMatchStartEvent(ev);\n"
        "                break;\n")
F.write_text(R(1, 384) + stub + R(745, len(L)))

print("split done:")
for f in ("launcher_ui_hub_events.cpp", "launcher_ui_hub_events_match.cpp"):
    p = pathlib.Path("launcher/ui") / f
    print(f"  {f:36s} {sum(1 for _ in p.open())} lines")
