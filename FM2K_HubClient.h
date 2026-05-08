#pragma once

// FM2K Hub client — WebSocket transport for hub.py.
// Phase-1 scaffold; talks the JSON protocol documented in
// docs/FM2K_Matchmaking_Design.md §15.2. No persistence, no auth.
//
// TODO(cross-platform): the implementation in FM2K_HubClient.cpp uses
// WinHTTP (Windows-only). The public API in this header is platform-
// agnostic. When we go Linux, swap the .cpp out for a libcurl /
// libwebsockets / asio-beast backend; the launcher UI and integration
// surface won't need to change.

#include <windows.h>
// NOTE: <winhttp.h> is NOT included here. It conflicts with <wininet.h>
// which is pulled in by FM2K_LauncherUI.cpp (both define INTERNET_SCHEME,
// URL_COMPONENTS, etc.). The public API only needs HINTERNET, which is
// already defined in <windows.h>. WinHTTP-specific use is contained in
// FM2K_HubClient.cpp.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fm2k {

struct HubUser {
    std::string id;
    std::string nick;
    std::string room_id;
    std::string status;        // "idle" | "challenging" | "in_match"
    std::string opponent_id;
    int rtt_ms = 0;
    // Patreon tier label sent by hub. "tester" ($5 — blue name) or
    // "thanks" ($10 Special Thanks — gold name). Empty string for legacy
    // hubs that don't include the field; the launcher treats empty/unknown
    // values as plain (no recolor).
    std::string tier;
};

struct HubRoom {
    std::string id;            // == game_id
    std::string name;
    int user_count = 0;
};

// Mirror of fm2k::game_ini::GamePlayConfig — kept in this header
// (instead of pulling FM2K_GameIni.h into the public hub-client API)
// so external consumers don't have to depend on the launcher's INI
// module just to inspect a challenge payload. Sentinel -1 = unset.
//
// random_seed != 0 (#56) signals "random stage enabled — both peers
// seed an xorshift PRNG with this value and roll a fresh stage at
// every Netplay_StartBattle. Lilith-equivalent semantics: one seed
// agreed at challenge time, deterministic re-rolls on rematches."
// random_stage_min / random_stage_max bound the roll; both peers
// must compute (next_xorshift() % (max-min+1)) + min identically.
struct MatchSettings {
    int player0_cpu      = -1;
    int player1_cpu      = -1;
    int game_speed       = -1;
    int hit_judge        = -1;
    int game_information = -1;
    int stage_nb         = -1;
    int joystick         = -1;
    int time             = -1;
    int exit_flag        = -1;
    int vs_mode          = -1;
    int vs_single_play   = -1;
    int vs_team_play     = -1;
    // Random-stage extension. Off when random_seed == 0 (legacy +
    // default). Range is INCLUSIVE on both ends.
    uint32_t random_seed       = 0;
    int      random_stage_min  = -1;
    int      random_stage_max  = -1;
};

// Events the launcher polls. Only carry the data the UI needs;
// transport / timing details stay inside HubClient.
struct HubEvent {
    enum class Kind {
        Connected,             // hello_ack
        Disconnected,          // socket dropped or local Disconnect()
        RoomList,              // initial / list_rooms response
        RoomJoined,            // self joined a room (carries user list)
        RoomLeft,
        UserJoined,
        UserLeft,
        UserStatus,            // status / opponent change
        UserRtt,               // RTT update
        ChallengeReceived,     // someone challenged us
        ChallengeFailed,       // ours failed (target unavailable)
        ChallengeCancelled,    // challenger cancelled
        ChallengeDeclined,     // target declined
        MatchStart,            // both sides go into punch + GekkoNet
        MatchRotated,          // hub minted fresh token for next FM2K match in same session
        PeerDisconnected,
        SpectateGranted,       // hub returned host addr for our spectate request
        SpectateDenied,        // hub rejected spectate (target not in_match etc)
        RecordReceived,        // hub answered our query_record (W/L/D)
        RecentMatchesReceived, // hub answered our recent_matches request
        CurrentMatchesReceived,// hub answered our current_matches request
        MatchInProgressStarted,// public broadcast: an in-flight match began
        MatchInProgressUpdated,// public broadcast: chars/stage filled in
        MatchInProgressEnded,  // public broadcast: in-flight match resolved
        Error,
    };

    Kind kind{Kind::Error};
    std::string error;

    // Generic payloads — only the fields relevant to a given Kind are set.
    std::vector<HubRoom> rooms;
    std::vector<HubUser> users;
    HubUser user;
    std::string user_id;       // for UserLeft / UserRtt
    std::string room_id;
    int rtt_ms = 0;

    struct {
        std::string from_id;
        std::string from_nick;
        std::string room_id;
        // Host's resolved [GamePlay] config (#54). Forwarded by the
        // hub from the challenger's `challenge` message so the target
        // can display "this match will be: 3 rounds, 60s timer, stage
        // 2…" before they accept. Anti-cheat clamps already applied
        // launcher-side. Sentinel -1 across the struct means the
        // challenger ran an older launcher that didn't include them.
        MatchSettings settings;
    } challenge;

    struct {
        std::string token;
        std::string role;      // "host" | "guest"
        HubUser peer;
        std::string peer_udp_ip;
        int peer_udp_port = 0;
        std::string peer_ws_addr;
        // Relay fallback. Hub fills these on match_start so the hook
        // can switch to relay mode if direct punch fails. Empty/zero
        // means hub didn't advertise a relay (older hub or disabled).
        std::string relay_ip;
        int         relay_port = 0;
        std::string relay_session_id;   // 32-hex-char string (= match token bytes)
        // Host's resolved [GamePlay] config (#54 followup). The hub
        // remembers what the challenger sent on `challenge` and echoes
        // it here so BOTH peers see the same authoritative settings
        // (the challenged user already saw it in their accept modal,
        // but the host needs it back too if their own override file
        // changed between sending the challenge and seeing match_start).
        // Used by ApplyForLaunch to write game.ini before spawn.
        MatchSettings settings;
    } match;

    // Spectate-grant payload. host_ip / host_port are where the spectator
    // FM2K should aim its FM2K_REMOTE_ADDR; target_nick / opponent_nick
    // are advisory for UI labelling ("Watching A vs B").
    struct {
        std::string target_id;
        std::string target_nick;
        std::string opponent_id;
        std::string opponent_nick;
        std::string host_ip;
        int         host_port = 0;
    } spectate;

    // RecordReceived payload. Filled when Kind::RecordReceived fires.
    // user_id is whose record this is (always the asker for now). If
    // opponent_id is set, the {wins,losses,draws} are filtered to that
    // opponent. vs_breakdown is always the top-32-by-match-count global
    // rows for the asker.
    struct VsRow {
        std::string opponent_id;
        std::string opponent_nick;
        int wins = 0, losses = 0, draws = 0;
    };
    struct {
        std::string user_id;
        std::string opponent_id;   // empty = global record
        std::string game_id;       // empty = all games
        int wins = 0, losses = 0, draws = 0;
        std::vector<VsRow> vs_breakdown;
    } record;

    // RecentMatchesReceived payload — newest match first.
    struct MatchRow {
        std::string id;
        std::string p1_id;
        std::string p1_nick;
        std::string p2_id;
        std::string p2_nick;
        std::string game_id;
        std::string winner_id;     // empty = draw
        double      finished_at = 0.0;
        // Char + stage detail. -1 / empty mean "missing" (peers
        // disagreed or didn't report — older client). UI uses
        // fm2k::FormatCharLabel / FormatStageLabel which fall back
        // gracefully to "Char #N" / "Stage #N" when names are absent.
        int32_t     p1_char_id   = -1;
        int32_t     p2_char_id   = -1;
        std::string p1_char_name;
        std::string p2_char_name;
        int32_t     stage_id     = -1;
        std::string stage_name;
    };
    std::vector<MatchRow> recent_matches;

    // In-progress match payload. Used by both the snapshot response
    // (`current_matches`) and the live broadcasts (`match_in_progress_*`).
    // -1 / empty char/stage fields mean the match-progress report
    // hasn't arrived yet (battle still in CSS, or the peer's launcher
    // is older). `started_at` is unix seconds from the hub clock.
    struct MatchInProgress {
        std::string token;
        std::string p1_id;
        std::string p1_nick;
        std::string p2_id;
        std::string p2_nick;
        std::string game_id;
        double      started_at = 0.0;
        int32_t     p1_char_id   = -1;
        int32_t     p2_char_id   = -1;
        std::string p1_char_name;
        std::string p2_char_name;
        int32_t     stage_id     = -1;
        std::string stage_name;
    };
    // CurrentMatchesReceived snapshot — full list. Live updates use
    // `current_match_update` (Started/Updated) or `current_match_token`
    // (Ended; only the token to evict).
    std::vector<MatchInProgress> current_matches;
    MatchInProgress              current_match_update;
    std::string                  current_match_token;
};

class HubClient {
public:
    HubClient();
    ~HubClient();

    HubClient(const HubClient&) = delete;
    HubClient& operator=(const HubClient&) = delete;

    // Begin async connect. Returns true on dispatch (thread launched);
    // actual connection state lands as a Connected/Disconnected event.
    // host: bare hostname or IP (no scheme). port: TCP. path: "/".
    // hub_token: opaque string the launcher caches after Discord OAuth
    // sign-in. The hub validates it on hello and rejects connections
    // with a `auth_required` error if empty/invalid. Pass "" to attempt
    // an unauthenticated connect (only works against a hub started
    // with FM2K_HUB_AUTH_DISABLE=1).
    // secure=true upgrades the WebSocket to TLS (wss://) — required
    // when fronting through Caddy / a reverse proxy that terminates
    // HTTPS. Defaults to false for the legacy direct-port path.
    bool Connect(const std::string& host, uint16_t port, const std::string& path,
                 const std::string& nick, const std::string& hub_token,
                 bool secure = false);
    // Compat overload — defaults hub_token to empty.
    bool Connect(const std::string& host, uint16_t port, const std::string& path,
                 const std::string& nick) {
        return Connect(host, port, path, nick, std::string{});
    }

    void Disconnect();

    bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

    // Drain pending events on the UI thread. Call once per frame.
    void Poll(const std::function<void(const HubEvent&)>& on_event);

    // Outbound. Safe to call any time after Connect; queued if not yet
    // connected, sent in order once the upgrade completes.
    void SendUdpAddr(const std::string& ip, int port);
    void ListRooms();
    void JoinRoom(const std::string& game_id, const std::string& display_name = "");
    void LeaveRoom();
    void Challenge(const std::string& target_id);
    // Variant — includes the host's resolved [GamePlay] config so the
    // target sees what they're agreeing to in the accept modal (#54).
    // Hub forwards `settings` verbatim; older hubs / launchers safely
    // ignore the extra fields. -1 in any int means "not set".
    void Challenge(const std::string& target_id,
                   const MatchSettings& settings);
    void CancelChallenge(const std::string& target_id);
    void AcceptChallenge(const std::string& challenger_id);
    void DeclineChallenge(const std::string& challenger_id);
    void MatchEnded();

    // Report end-of-match outcome to the hub for stats tracking. `match_id`
    // is the token from the `match_start` event we received (== relay
    // session_id). `outcome` is one of: "self_won" | "peer_won" | "draw"
    // | "disconnect". Hub correlates both peers' reports by match_id and
    // commits to the persistent match log only when they agree.
    //
    // The 4-arg overload also carries P1/P2 character IDs captured at
    // battle start (FM2K::P1/P2_CHARACTER_ID_ADDR). Pass 0xFFFFFFFFu to
    // omit a side. Per-character stats (matchup tables, char-specific
    // W/L/D) on the hub side need both reports to agree on the IDs;
    // mismatches log a warning and fall back to global counters only.
    void MatchResult(const std::string& match_id, const std::string& outcome);
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id);
    // Full overload — adds resolved .player filenames (UTF-8) so the
    // hub can store human-readable char names alongside the IDs.
    // Empty strings are omitted from the JSON payload to save bytes.
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id,
                     const std::string& p1_char_name,
                     const std::string& p2_char_name);
    // Full overload — adds stage_id + resolved stage_name (UTF-8). Stage
    // name is resolved on the launcher via FindKgtByGameId because FM2K
    // has no in-memory stage-filename table; pass empty string when the
    // game isn't installed locally and the hub will store id-only.
    // Pass 0xFFFFFFFFu / empty string to omit either field.
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id,
                     const std::string& p1_char_name,
                     const std::string& p2_char_name,
                     uint32_t stage_id,
                     const std::string& stage_name);

    // C10 — schema-2 overload. Adds session correlation + per-round results.
    // session_id is the 64-bit token generated at peer-connect time on the
    // host (shared across every match this pair plays until disconnect);
    // match_index_in_session is 1-based (1 = first match of the session).
    // rounds[] carries per-round mini-records sized to the actual count
    // (FM2K caps rounds at 8). Pass session_id == 0 to fall back to the
    // schema-1 wire shape (omits all session/round fields).
    struct RoundJson {
        uint8_t  winner_idx;       // 0=p1, 1=p2, 2=draw
        uint16_t p1_hp_remaining;
        uint16_t p2_hp_remaining;
        uint32_t frames_elapsed;
    };
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id,
                     const std::string& p1_char_name,
                     const std::string& p2_char_name,
                     uint32_t stage_id,
                     const std::string& stage_name,
                     uint64_t session_id,
                     uint8_t  match_index_in_session,
                     const std::vector<RoundJson>& rounds);

    // Ask hub for our W/L/D record. Both args optional — pass empty
    // string to omit. Hub responds with K::RecordReceived carrying
    // wins/losses/draws + per-opponent breakdown.
    void QueryRecord(const std::string& opponent_id = "",
                     const std::string& game_id     = "");

    // Ask hub for our most-recent N matches. Hub responds with
    // K::RecentMatchesReceived.
    void RequestRecentMatches(int limit = 50);

    // Ask hub for the snapshot of all currently-in-flight matches. Hub
    // responds with K::CurrentMatchesReceived. Live updates after the
    // snapshot land as MatchInProgressStarted/Updated/Ended events. Sent
    // typically once on connect + on room rejoin so a freshly-opened
    // launcher's lobby panel populates without waiting for the next
    // accept_challenge to fire a Started broadcast.
    void RequestCurrentMatches();

    // Report this peer's view of the live char/stage state for an
    // in-flight match. Called from the launcher right after the hook
    // publishes match_p1_char_id / etc. to shared mem at battle start.
    // The hub merges (last writer wins) and broadcasts Updated to all
    // lobby viewers. Pass 0xFFFFFFFFu / empty string for fields the
    // local launcher couldn't resolve (game not installed locally).
    void ReportMatchProgress(const std::string& match_id,
                             uint32_t p1_char_id, uint32_t p2_char_id,
                             const std::string& p1_char_name,
                             const std::string& p2_char_name,
                             uint32_t stage_id,
                             const std::string& stage_name);

    // Ask hub to introduce us to the host of an in-progress match.
    // target_id is the host user's id (the user we want to spectate).
    // Hub responds with SpectateGranted / SpectateDenied.
    void RequestSpectate(const std::string& target_id);

private:
    void EnqueueOut(std::string msg);
    void IoThread(std::string host, uint16_t port, std::string path, std::string nick);
    void OnMessage(const std::string& msg);
    void EmitEvent(HubEvent ev);

    void CleanupHandles();

    // HINTERNET is a typedef in <winhttp.h>/<wininet.h>, neither of
    // which we want to expose. Store as opaque void* and cast at the
    // use sites in FM2K_HubClient.cpp.
    void* session_ = nullptr;
    void* conn_    = nullptr;
    void* req_     = nullptr;
    void* ws_      = nullptr;

    std::thread io_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    // Discord OAuth hub_token, supplied at Connect() time and embedded
    // in the WS hello payload. Empty when running against a hub that
    // disables auth (FM2K_HUB_AUTH_DISABLE=1 server-side).
    std::string hub_token_;
    bool        use_tls_ = false;  // wss:// when true

    std::mutex out_mtx_;
    std::condition_variable out_cv_;
    std::deque<std::string> outbox_;

    std::mutex in_mtx_;
    std::deque<HubEvent> inbox_;
};

}  // namespace fm2k
