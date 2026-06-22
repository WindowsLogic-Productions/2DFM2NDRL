// frontend/AppShell.h — public surface of the new shell render path.
// One entry point, one cleanup-time hook. Shell pulls real state from
// the LauncherUI instance the parent forwards in.
//
// TODO(naming): the `fm2k::` namespace is misleading — this launcher
// targets BOTH FM2K and FM95 engines (the wider 2DFM scene). The whole
// project's `fm2k::` namespace should eventually rename to something
// engine-neutral (`dfm::` / `twodfm::` / similar) in a single sweep
// across HubClient, Locale, discord_auth, pii, game_ini, etc. Doing
// it as a one-shot keeps the codebase coherent; new shell code uses
// `fm2k::shell` for now to match the existing convention.
#pragma once

#include <cstdint>
#include <string>

class LauncherUI;

namespace fm2k::shell {

// Fills the SDL viewport with the AppShell. Reads through const
// accessors on LauncherUI; mutating calls go through public setters
// (SetActiveRoom, SetGames already exists, etc.) and the existing
// on_* callback hooks. No owned state besides the static ShellState
// singleton.
void Render(LauncherUI& lu);

// Push an incoming chat line into the shell's per-room ring buffer.
// Called from LauncherUI::PollHubEvents on K::ChatReceived (docs/
// hub_protocol_v2.md §4.2). The "me" flag is set internally based on
// whether `user_id` matches lu.hub_my_id() at render time, so the
// caller doesn't need to pre-classify. `ts_unix_seconds` is hub-
// authoritative; pass 0 for system lines that don't carry a stamp.
void PushIncomingChat(const std::string& room_id,
                      const std::string& user_id,
                      const std::string& nick,
                      const std::string& text,
                      int64_t            ts_unix_seconds,
                      bool               system);

}  // namespace fm2k::shell
