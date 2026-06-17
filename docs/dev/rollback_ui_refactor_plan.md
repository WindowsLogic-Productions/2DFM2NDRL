# Rollback-driver + UI refactor plan (function-refactor + Slint prep)

Continuation after the pure-move file splits. Two new dimensions the user called out:
1. The 984-line `Netplay_ProcessBattleInputPhase` -- a FUNCTION refactor (not a move).
2. UI decomposition aimed at an eventual **Slint** UI rewrite (separate render from logic).
Plus a cross-cutting constraint: **FM95 will reuse the SAME GekkoNet rollback** as FM2K,
so the rollback driver must stay engine-agnostic.

Validation: clumsy is ON (240ms RTT / 20% loss). Every hook-side step is harness-gated
with `replay_selftest` + `replay_netplay_selftest --frames 1500` under clumsy, expecting
clean `ALL ... IDENTICAL` (input fields no longer compared -- prediction artifact, fixed).

## Phase A -- Netplay_ProcessBattleInputPhase (984 lines) [DET, clumsy window]

Structure (netplay_battle_phase.cpp:79-1063):
- 79-112  session guard + network poll + battle-entry-signal insurance
- 114-190 local-input collection (stress autoplay / netplay autoplay / production) -> gekko_add_local_input
- 192-247 gekko_session_events loop (Connected/Disconnected/DesyncDetected)
- 250     gekko_update_session
- 282-1007 update dispatch: GekkoSaveEvent (283-325) / GekkoLoadEvent (326-407) /
           GekkoAdvanceEvent (408-1007, ~600 lines: apply inputs -> PGI -> update_game ->
           RNG patch -> desync check -> confirm-ring -> spectator handoff -> timing)
- 1008-1063 post (frames_ahead -> HandleFrameTime)

Refactor (behavior-preserving function extraction, NOT a move):
- Extract the three update handlers to named helpers:
    HandleSaveEvent(const GekkoGameEvent&), HandleLoadEvent(...), HandleAdvanceEvent(...).
  The Advance handler is the bulk; if it's still >~400 after extraction, sub-extract its
  internal phases (apply-inputs / re-sim / post-sim-rng-patch / desync / confirm).
- Extract local-input collection -> CollectAndAddLocalInput(uint16_t local).
- Extract the session-events loop -> HandleSessionEvents().
- The main fn becomes a slim driver: collect -> add -> session-events -> update -> dispatch.
- To get the FILE <1000: move the extracted handlers into a sibling
  `netplay_battle_events.cpp` (shares state via the existing netplay_internal.h; the
  handlers are file-local helpers, declared there). Driver + spectator phase stay in
  netplay_battle_phase.cpp.

ENGINE-AGNOSTIC (FM95 reuse): the driver + handlers must NOT hardcode FM2K addresses.
They already route engine-specifics through SaveState_* (engine-split) and
original_process_game_inputs/update_game (globals). Audit the Advance handler for any
literal FM2K addresses (RNG patch, input write) -- if found, note them for an FM2K::ADDR_*
abstraction so FM95 can share. Same treatment for Netplay_ProcessSpectatorPhase (the
smaller sibling loop, 1076+) -- keep it engine-agnostic.

Gate: build BOTH DLLs; replay_selftest + netplay_selftest (1500, under clumsy) IDENTICAL,
after EACH extraction. Commit per extraction.

## Phase B -- UI decomposition for Slint prep [separate RENDER from LOGIC]

The Slint rewrite replaces the IMGUI RENDER layer; the model/state/logic is reused. So
split each UI monolith so the ImGui draw code is isolated in its own TU (replaceable) and
the data/logic (binding store, capture, persistence, sampling, hub state) stays put.

- B1 input_binder.cpp (1592, hook+launcher shared):
    input_binder.cpp        -> core: lifecycle + binding store (g_players) + ApplyDefaults
    input_binder_gamepads.cpp -> SDL3 gamepad discovery/lifecycle
    input_binder_profiles.cpp -> INI persistence + per-game profiles
    input_binder_sample.cpp   -> SDL3 + Win32 sampling (the engine-facing read; NOT ImGui)
    input_binder_ui.cpp       -> RenderBody/RenderWindow + capture state machine (IMGUI;
                                  the ONLY file Slint replaces)
  Shared state via input_binder_internal.h. All <1000. Build + launcher launch (+ in-game
  binder still samples) to verify.
- B2 imgui_overlay.cpp (1080):
    imgui_overlay.cpp        -> hook/D3D9 plumbing + lifecycle + hotkey + window-find
    imgui_overlay_render.cpp -> Hook_EndScene + RenderDebugOverlay + viewport math (IMGUI;
                                Slint-replaceable)
  Shared state via imgui_overlay_internal.h.

## Phase C -- remaining leftovers (lower priority)

- C1 control_channel.cpp (1187, DET netcode): 4-file split (core / io / send / gekko-adapter)
    via control_channel_internal.h. Harness-gate under clumsy (it's the netplay transport).
    ENGINE-AGNOSTIC already (FM95 reuses it).
- C2 FM2K_HubClient.cpp (1352, launcher): 5-file split (json-helpers / outbound / dispatch /
    transport / stub) -- member-fn moves, no internal header. Build + launch.
- C3 game_discovery.cpp (1191, launcher): cache-IO split via game_discovery_internal.h
    (Utils:: helpers un-static'd). Build + launch.
- C4 dllmain.cpp (1030): LEAVE (cohesive boot, 30 over) unless trivial patches-extraction.
- C5 FM2K_Integration.h (1305, header) + tests/test_spectator_protocol.cpp (1256): defer.

## Order (continue-in-loop)
A (984-line fn, clumsy window) -> C1 control_channel (DET, clumsy window) -> B1 input_binder
-> B2 imgui_overlay -> C2 HubClient -> C3 game_discovery. DET/netcode first while clumsy is
on; launcher/UI after. Commit + harness-gate each.
