# spectator_node.cpp split plan (in progress)

Goal: break the spectator/replay god-object into human-navigable concern-files so
spectator + replay can be worked on properly. Behavior-preserving pure moves only.

## done

- `spec_wire.{cpp,h}` -- SessionEvent wire codecs + zero-RLE. (commit 630644d)
- `spectator_node_internal.h` -- the shared State model (constants, Subscriber,
  InitialMatch, MatchHeader, struct State) lifted out of the anon namespace;
  `g_state` now file-scope/external so sibling TUs can share it. (commit 5372d6d)

spectator_node.cpp is ~5200 lines and still holds every function group.

## the wrinkle

Almost everything below the state lives in ONE anonymous namespace (internal
linkage) and calls sibling functions bidirectionally. So moving a group out is
NOT a clean slice like spec_wire was. Each move is:

1. Pick a cohesive function group (below).
2. Un-anon those functions (they currently have internal linkage; a sibling TU
   can't see them). Declare the ones called from OTHER groups in an internal
   header (spectator_node_internal.h, or a per-group `spec_<g>.h`).
3. Any file-scope `static` the group OWNS moves with it (and if another group
   also touches it, it becomes `extern` in the internal header). Map below.
4. Move the function bodies to `spec_<group>.cpp`, `#include "spectator_node_internal.h"`.
5. Add `spec_<group>.cpp` to FM2KHOOK_SOURCES (builds BOTH FM2KHook + FM95Hook).
6. `ninja` -> both hooks + launcher green. Commit. Netplay smoke-test (host +
   spectator, a match + a rematch, plus an offline .fm2krep replay) before
   leaning on it.

Build-green is a strong gate here: pure moves of identical code can't change
rollback behavior; the only failure mode is compile/link, which the build catches.

## concern-files, safest-first

Function line numbers drift as moves land -- re-grep before each.

1. **spec_transport.cpp** -- OutboundBroadcast, OutboundSendTo, SendRaw,
   AppendEventToWire, CountInputs, FlushBatch, SendUdpInputBatches,
   SendOpBaselineTo, AddrEqual, FormatAddr, Fletcher32. "Cleanly separated"
   (relay-vs-TCP branching localized). Touches g_state.subscribers + the spec_relay
   rings. Most are called from the host event path -> declare those in the header.

2. **spec_backfill.cpp** -- SendSessionEventsTo, SendSessionBackfillTo,
   SendSessionBackfillFromFrame, BackfillFirstIdxForFrame, SendSnapshotTo (calls
   ZeroRleCompress in spec_wire -- already split). Read-mostly g_state.

3. **spec_session_file.cpp** -- WriteSessionFileImpl, EncodeEventSliceToBytes,
   ResolveMatchHeader/ResolveLatestMatchEnd/CountMatchesInSlice, IsStateInitForSeek,
   SpectatorNode_WriteSessionFile/WriteCurrentBattleFile, SpectatorNode_LoadSessionFile.
   Write path is read-only g_state (clean); load path writes playback state (keep
   together, moderate).

4. **spec_host_events.cpp** -- SpectatorNode_On{MatchStart,FrameConfirmed,MatchEnd},
   AppendOpAndFlush, all SpectatorNode_Append* , SpectatorFingerprint_*. OWNS the
   file-statics s_round_start_input_frame, s_last_seen_rounds_won_p1/p2,
   s_match_index_in_session, s_match_start_input_frame -- move them along.

5. **spec_join.cpp** -- BuildJoinAckPacket, SpectatorNode_Handle{JoinReq,Leave,
   Heartbeat,JoinAck,JoinRedirect}, RequestJoin, Get{SubscriberCount,Addrs},
   IsBroadcasting. OWNS g_gekko_spectator_addrs (+ ClearGekkoSpectatorTracking).

6. **spec_playback.cpp** -- ApplyResetInputState, ApplySessionEvent,
   SpectatorNode_PopFrameInputs + the snapshot-cache/apply API (StashSnapshot,
   ApplyPendingPinRng, ApplyPendingSnapshot, HasSnapshot, GetSnapshotInfo),
   Pop/Get/Pending helpers, the adaptive delay bank (StampInputAdmit/
   TargetDelayFrames + their g_admit_* statics), boundary/queue helpers.
   MOST entangled with g_state -- do LAST, keep ApplySessionEvent + PopFrameInputs
   together (they share the boundary state machine).

7. **spec_health.cpp** -- SpectatorNode_TickHealth, SpectatorNode_TickHostMaintenance,
   SpectatorNode_HandleSpecData, SpectatorNode_HandleUdpInputDatagram,
   OnUpstreamTcpDead, LeaveUpstream, SetRootAddr. OWNS g_pending_punch_sockets.
   Big (~900 lines) but a clear concern (the per-tick drivers).

What stays in spectator_node.cpp: Init/Shutdown/SetCapacity + whatever thin glue
is left -- it becomes the module's entry/lifecycle file.

## after spectator_node

Same recipe for netplay.cpp (barrier-sync cluster + control-channel glue +
session lifecycle) and hooks.cpp (determinism FPU/virtual-clock, input hooks,
mode detection, frame driver).
