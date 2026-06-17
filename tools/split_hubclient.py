#!/usr/bin/env python3
"""One-shot 5-way splitter for FM2K_HubClient.cpp (1352 lines, launcher).

Pure behaviour-preserving move into:
  FM2K_HubClient.cpp           core: ctor/dtor + Connect/Disconnect/Poll/queue
  FM2K_HubClient_json.cpp      minimal JSON encode/decode helpers
  FM2K_HubClient_outbound.cpp  Send*/Join/Challenge/MatchResult/Query senders
  FM2K_HubClient_transport.cpp WinHTTP WS IoThread + CleanupHandles
  FM2K_HubClient_dispatch.cpp  OnMessage inbound dispatch
All are HubClient member fns (class in FM2K_HubClient.h) -- no state header
needed; only the JSON helpers are shared (FM2K_HubClient_internal.h).
Ranges 1-based inclusive, verified against the read.
"""
import pathlib

SRC = pathlib.Path("FM2K_HubClient.cpp")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 1200 or not any("void HubClient::OnMessage" in ln for ln in L):
    raise SystemExit("FM2K_HubClient.cpp is not the original monolith "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

INCLUDES = R(1, 39)               # _WIN32_WINNT bump + WinHTTP + std includes
NS_OPEN  = "namespace fm2k {\n"
NS_CLOSE = "}  // namespace fm2k\n"
INT_INC  = '#include "FM2K_HubClient_internal.h"\n'

def tu(path, body, want_json=True):
    parts = [INCLUDES]
    if want_json:
        parts.append(INT_INC)
    parts.append("\n")
    parts.append(NS_OPEN)
    parts.append("\n")
    parts.append(body)
    parts.append(NS_CLOSE)
    pathlib.Path(path).write_text("".join(parts))

# ---- json ----
json_body = R(48, 278)
# GetInt's default arg belongs on the declaration (internal.h), not the def.
json_body = json_body.replace(
    "int GetInt(const std::string& s, const std::string& key, int def = 0) {",
    "int GetInt(const std::string& s, const std::string& key, int def) {")
tu("FM2K_HubClient_json.cpp", json_body)

# ---- outbound / transport / dispatch ----
tu("FM2K_HubClient_outbound.cpp",  R(364, 714))
tu("FM2K_HubClient_transport.cpp", R(715, 941))
tu("FM2K_HubClient_dispatch.cpp",  R(942, 1351))

# ---- core (overwrites SRC last) ----
core = [INCLUDES, "\n", NS_OPEN, "\n", R(282, 363), NS_CLOSE]
SRC.write_text("".join(core))

print("split done:")
for f in ("FM2K_HubClient.cpp", "FM2K_HubClient_json.cpp",
          "FM2K_HubClient_outbound.cpp", "FM2K_HubClient_transport.cpp",
          "FM2K_HubClient_dispatch.cpp"):
    print(f"  {f:32s} {sum(1 for _ in open(f))} lines")
