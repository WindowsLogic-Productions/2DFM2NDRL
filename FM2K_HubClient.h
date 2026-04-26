#pragma once

// FM2K Hub client — WinHTTP WebSocket transport for hub.py.
// Phase-1 scaffold; talks the JSON protocol documented in
// docs/FM2K_Matchmaking_Design.md §15.2. No persistence, no auth.

#include <windows.h>
#include <winhttp.h>

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
    } match;
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

private:
    void EnqueueOut(std::string msg);
    void IoThread(std::string host, uint16_t port, std::string path, std::string nick);
    void OnMessage(const std::string& msg);
    void EmitEvent(HubEvent ev);

    void CleanupHandles();

    HINTERNET session_ = nullptr;
    HINTERNET conn_    = nullptr;
    HINTERNET req_     = nullptr;
    HINTERNET ws_      = nullptr;

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
