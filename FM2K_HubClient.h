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
};

struct HubRoom {
    std::string id;            // == game_id
    std::string name;
    int user_count = 0;
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
        PeerDisconnected,
        SpectateGranted,       // hub returned host addr for our spectate request
        SpectateDenied,        // hub rejected spectate (target not in_match etc)
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
    bool Connect(const std::string& host, uint16_t port, const std::string& path,
                 const std::string& nick);

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
    void CancelChallenge(const std::string& target_id);
    void AcceptChallenge(const std::string& challenger_id);
    void DeclineChallenge(const std::string& challenger_id);
    void MatchEnded();

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

    std::mutex out_mtx_;
    std::condition_variable out_cv_;
    std::deque<std::string> outbox_;

    std::mutex in_mtx_;
    std::deque<HubEvent> inbox_;
};

}  // namespace fm2k
