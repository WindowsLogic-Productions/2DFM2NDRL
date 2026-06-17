#pragma once

// FM2K_PortMapper -- UPnP-IGD automatic port mapping for the rollback
// launcher (Phase 1 of the NAT reachability overhaul, see
// docs/dev/nat_reachability_plan.md).
//
// The whole point: make THIS peer explicitly reachable from the internet
// by asking the home router (an UPnP Internet Gateway Device) to forward
// the game's UDP port inbound. A peer with a working mapping accepts a
// datagram from ANY source address, so one mapped side makes a pairing
// connect regardless of the other side's NAT behavior -- this is the
// single highest-leverage fix for the "both behind NAT, punch fails" case
// short of the relay.
//
// Design constraints (from the plan doc):
//  - Discovery (SSDP) can take seconds, so ALL UPnP work happens on a
//    worker thread. The UI/main thread only ever calls Snapshot(), which
//    copies a mutex-guarded status. Nothing here blocks the render loop.
//  - PortMapper does NOT bind any socket. The mapping lives router-side;
//    the game's hook owns the actual UDP socket on the same port. So this
//    adds no socket-handoff hazard to the existing launcher preflight.
//  - Best-effort: every failure path falls through to exactly today's
//    behavior (punch + peer-learning + relay still back us up). A stale or
//    absent mapping is harmless.
//  - Backend-pluggable: the miniupnpc calls are isolated behind one
//    internal worker function so a libnatpmp (NAT-PMP/PCP) backend can slot
//    in as a fast-follow (D2) without touching the public API or the
//    launcher wire-in.
//
// Thread-safety: StartAsync / Stop / Snapshot are all safe to call from the
// UI thread. The worker mutates status_ only under status_mtx_; Snapshot
// copies under the same lock.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fm2k {

class PortMapper {
public:
    // Lifecycle state surfaced to the UI + the hub re-send logic.
    enum class State {
        Idle,        // never started, or Stop()'d back to rest
        Discovering, // worker running: SSDP discover / IGD select in flight
        Mapped,      // AddPortMapping succeeded; ext_ip / ext_udp_port valid
        NoIgd,       // SSDP found no usable IGD on the LAN
        Failed,      // discovery/mapping error, or FM2K_NO_UPNP=1 escape hatch
        Cgnat,       // IGD mapped, but its WAN IP is private (RFC1918/CGNAT):
                     // the mapping is on an inner NAT and useless. The hub
                     // double-checks this against the WS-source IP (D6).
    };

    struct Status {
        State       state = State::Idle;
        std::string ext_ip;            // IGD-reported external (WAN) IPv4
        uint16_t    ext_udp_port = 0;  // the port actually granted on the WAN
        std::string backend;           // "miniupnpc" | "disabled" | ""
        std::string igd_desc;          // identifying string for the IGD
                                       // (control-URL host -- there is no
                                       // friendlyName in miniupnpc's IGDdatas)
    };

    PortMapper() = default;
    ~PortMapper();

    PortMapper(const PortMapper&)            = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    // Kick off mapping on a background thread for the given UDP port. The
    // worker: upnpDiscover (SSDP, ~2s budget) -> UPNP_GetValidIGD ->
    // GetExternalIPAddress -> AddPortMapping (ext==local first, D4 conflict
    // fallbacks) with a 1800s lease, then renews at half-life (900s) until
    // Stop(). Idempotent-ish: a second StartAsync while already running is
    // ignored (the existing mapping/worker stays). If FM2K_NO_UPNP=1, this
    // is a no-op that lands in state Failed with backend "disabled".
    void StartAsync(uint16_t udp_port);

    // Thread-safe copy of the current status for the UI loop / re-send check.
    Status Snapshot() const;

    // DeletePortMapping for whatever we mapped (if anything) + join the
    // worker thread. Idempotent: safe to call when never started or already
    // stopped. Called on session teardown / launcher exit.
    void Stop();

private:
    // The one place the miniupnpc backend lives. Runs on the worker thread:
    // discover -> map -> renew-loop until stop_ is set, then unmap. A second
    // backend (libnatpmp) would be a sibling of this function selected at
    // StartAsync time; nothing else in the class touches the UPnP API.
    void WorkerMiniupnpc(uint16_t udp_port);

    // Set status_ atomically under the lock + (optionally) update fields.
    void SetState(State s);

    mutable std::mutex status_mtx_;
    Status             status_;          // guarded by status_mtx_

    std::thread        worker_;
    std::atomic<bool>  running_{false};  // worker thread alive
    std::atomic<bool>  stop_{false};     // request: unmap + exit the worker
    // Note: the granted external port + the mapping's renewal/teardown state
    // are kept as locals inside WorkerMiniupnpc (which owns the SOAP
    // urls/data needed for AddPortMapping/DeletePortMapping anyway); the
    // public Snapshot() surfaces the granted ext port via Status.
};

}  // namespace fm2k
