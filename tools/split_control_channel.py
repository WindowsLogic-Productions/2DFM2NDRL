#!/usr/bin/env python3
"""One-shot splitter for FM2KHook/src/netplay/control_channel.cpp.

Pure behaviour-preserving move: slices the 1187-line monolith into
  control_channel.cpp        (core: state defs + socket lifecycle + poll/accessors)
  control_channel_io.cpp     (RawSend / RawReceive + race guard)
  control_channel_send.cpp   (ControlChannel_Send* packet builders)
  control_channel_gekko.cpp  (GekkoNet MultiplexAdapter)
shared state externed via control_channel_internal.h (already written).

Line ranges are 1-based inclusive, verified against the read of the original.
"""
import sys, pathlib

SRC = pathlib.Path("FM2KHook/src/netplay/control_channel.cpp")
orig = SRC.read_text().splitlines(keepends=True)
def R(a, b):  # 1-based inclusive
    return orig[a-1:b]

# ---- de-static transform for the state block (21..119) -----------------------
SKIP_SUBSTR = ("kRttRingCap = 64", "RECV_BUFFER_SIZE = 2048")  # now in the header
def destatic_state(lines):
    out = []
    for ln in lines:
        if any(s in ln for s in SKIP_SUBSTR):
            continue
        stripped = ln.lstrip()
        if stripped.startswith("static "):
            indent = ln[:len(ln) - len(stripped)]
            out.append(indent + stripped[len("static "):])
        else:
            out.append(ln)
    return out

# ============================================================================
# control_channel.cpp  (core)
# ============================================================================
core = []
core += R(1, 19)                       # original includes
core.append('#include "control_channel_internal.h"  // shared state + GetTimeMs + RawSend/RawReceive\n')
core += R(21, 23)                      # INTERNAL STATE banner
core += destatic_state(R(25, 119))     # globals, de-static'd (constants moved to header)
core.append("\n")
core += R(128, 362)                    # SOCKET MANAGEMENT: keepalive + NetSocket_*
core.append("\n")
core += R(597, 837)                    # CONTROL CHANNEL: poll + accessors + delay
core.append("\n")
core += R(1163, 1187)                  # HandlePong + SetConnected
SRC.write_text("".join(core))

# ============================================================================
# control_channel_io.cpp
# ============================================================================
io_hdr = '''// Control Channel -- RAW UDP send/receive (split from control_channel.cpp).
// RawSend wraps relay/direct egress; RawReceive demuxes control (0xCC) / NAT
// (0xCD) / spectator-UDP (0xCE) / GekkoNet packets. Shares state via
// control_channel_internal.h. CONTRACT: run under g_poll_mutex (RawReceive
// mutates g_gekko_packet_queue / g_recv_buffer). ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include "globals.h"            // g_player_index (peer-addr learning context)
#include "nat_traversal.h"      // fm2k::nat relay wrap/unwrap + 0xCD datagrams
#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <ws2tcpip.h>
#include <winsock2.h>
#include <windows.h>

'''
io_body = R(364, 595)
io_text = io_hdr + "".join(io_body)
# un-static the two cross-TU entry points
io_text = io_text.replace("static void RawSend(", "void RawSend(")
io_text = io_text.replace("static void RawReceive()", "void RawReceive()")
pathlib.Path("FM2KHook/src/netplay/control_channel_io.cpp").write_text(io_text)

# ============================================================================
# control_channel_send.cpp
# ============================================================================
send_hdr = '''// Control Channel -- packet builders (ControlChannel_Send* convenience fns).
// Split from control_channel.cpp; each fills a CtrlPacket and hands it to
// ControlChannel_Send. Shares state via control_channel_internal.h.
// ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include <SDL3/SDL_log.h>
#include <cstring>

'''
pathlib.Path("FM2KHook/src/netplay/control_channel_send.cpp").write_text(
    send_hdr + "".join(R(839, 1013)))

# ============================================================================
# control_channel_gekko.cpp
# ============================================================================
gekko_hdr = '''// Control Channel -- GekkoNet custom adapter (MultiplexAdapter). Split from
// control_channel.cpp; shares the control socket + queues, drains RawReceive
// under g_poll_mutex (the cross-thread heap-corruption fix lives in
// MultiplexAdapter_Receive). Shares state via control_channel_internal.h.
// ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include "gekkonet.h"
#include "nat_traversal.h"      // fm2k::nat::IsRelayMode
#include <SDL3/SDL_log.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <ws2tcpip.h>
#include <winsock2.h>

'''
pathlib.Path("FM2KHook/src/netplay/control_channel_gekko.cpp").write_text(
    gekko_hdr + "".join(R(1015, 1161)))

print("split done:")
for f in ("control_channel.cpp", "control_channel_io.cpp",
          "control_channel_send.cpp", "control_channel_gekko.cpp"):
    p = pathlib.Path("FM2KHook/src/netplay") / f
    print(f"  {f:32s} {sum(1 for _ in p.open())} lines")
