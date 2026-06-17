// FM2K_PortMapper -- UPnP-IGD automatic port mapping (Phase 1).
// See FM2K_PortMapper.h + docs/dev/nat_reachability_plan.md.

#include "FM2K_PortMapper.h"

// miniupnpc is a C library. MINIUPNP_STATICLIB is set by CMake (PUBLIC on
// the miniupnpc target) so its declspec.h drops the __declspec(dllimport)
// decoration -- required when linking against the static archive on
// Windows, otherwise we get __imp_ unresolved symbols.
extern "C" {
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
}

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>   // SDL_Delay for the renewal/teardown poll loop

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fm2k {

namespace {

// Mapping description shown in the router's UPnP table (pinned item 3b).
constexpr char   kMappingDesc[]   = "FM2K Rollback";
// Lease duration in seconds (D3). 1800s = 30 min; renewed at half-life so
// leased routers never drop us mid-session.
constexpr int    kLeaseSeconds    = 1800;
constexpr int    kRenewSeconds    = 900;   // half of kLeaseSeconds
// SSDP discovery budget (ms). 2s is the plan's budget -- long enough for a
// home IGD to answer the multicast, short enough not to stall the session.
constexpr int    kDiscoverDelayMs = 2000;

// UPnP SOAP error codes we special-case (pinned item 3b / D4):
constexpr int    kUpnpErrConflictInMapping       = 718; // entry already exists
constexpr int    kUpnpErrOnlyPermanentLeases     = 725; // router rejects leases

// Render a uint16 port to the decimal string miniupnpc's C API wants.
std::string PortStr(uint16_t p) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(p));
    return std::string(buf);
}

}  // namespace

PortMapper::~PortMapper() {
    // Make sure the worker is joined + the mapping torn down even if the
    // owner forgot to call Stop(). Stop() is idempotent.
    Stop();
}

void PortMapper::SetState(State s) {
    std::lock_guard<std::mutex> lk(status_mtx_);
    status_.state = s;
}

PortMapper::Status PortMapper::Snapshot() const {
    std::lock_guard<std::mutex> lk(status_mtx_);
    return status_;  // copy under the lock
}

void PortMapper::StartAsync(uint16_t udp_port) {
    // Escape hatch (pinned item 3b): FM2K_NO_UPNP=1 disables the whole
    // subsystem. Land in Failed/backend="disabled" so the UI can say
    // "UPnP disabled" without ever touching the network.
    const char* no_upnp = std::getenv("FM2K_NO_UPNP");
    if (no_upnp && std::strcmp(no_upnp, "1") == 0) {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_.state   = State::Failed;
        status_.backend = "disabled";
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] FM2K_NO_UPNP=1 -- skipping UPnP port mapping");
        return;
    }

    // Already running? Leave the existing worker/mapping in place. The
    // launcher starts mapping once per online session; a duplicate
    // StartAsync (e.g. a hub reconnect firing Connected twice) must not
    // spawn a second worker racing the first over the same router entry.
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] StartAsync ignored -- mapper already running");
        return;
    }

    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_          = Status{};            // reset
        status_.state    = State::Discovering;
        status_.backend  = "miniupnpc";
    }

    // Off-thread: SSDP discovery can block for the full kDiscoverDelayMs and
    // some routers stall longer on the description fetch. Never on the UI
    // thread.
    worker_ = std::thread([this, udp_port]() {
        WorkerMiniupnpc(udp_port);
        running_.store(false);
    });
}

void PortMapper::Stop() {
    // Idempotent: nothing to do if we never spawned a worker.
    if (!worker_.joinable()) {
        return;
    }
    // Ask the worker to unmap + exit; it polls stop_ in its renewal loop.
    stop_.store(true);
    worker_.join();
    // Worker has already done DeletePortMapping (it owns the urls/data it
    // needs for the SOAP call). Reset to Idle so a subsequent StartAsync on
    // a fresh online session starts clean.
    SetState(State::Idle);
}

// The single miniupnpc backend. Everything UPnP-specific lives here so a
// second backend (libnatpmp) can be added as a sibling without touching the
// rest of the class.
void PortMapper::WorkerMiniupnpc(uint16_t udp_port) {
    int discover_err = 0;
    // upnpDiscover: SSDP M-SEARCH on the LAN. localport=UPNP_LOCAL_PORT_ANY
    // lets the OS pick the source port (we never want to collide with the
    // game's UDP port). ipv6=0, ttl=2 per UDA 1.1.
    struct UPNPDev* devlist = upnpDiscover(
        kDiscoverDelayMs,
        nullptr,                 // multicastif: default interface
        nullptr,                 // minissdpdsock: default
        UPNP_LOCAL_PORT_ANY,
        0,                       // ipv4
        2,                       // ttl
        &discover_err);

    if (!devlist) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] SSDP discovery found no devices (err=%d)",
                    discover_err);
        SetState(State::NoIgd);
        return;
    }

    // Select a valid IGD from the discovered list. UPNP_GetValidIGD probes
    // each device's description and returns:
    //   1 = connected IGD (usable)
    //   2 = connected IGD but its WAN address is reserved/private -> CGNAT
    //   3 = IGD present but not connected
    //   4 = a UPnP device that isn't an IGD
    //   0 = none of the above
    struct UPNPUrls  urls;
    struct IGDdatas  data;
    char lanaddr[64] = {0};
    char wanaddr[64] = {0};
    std::memset(&urls, 0, sizeof(urls));
    std::memset(&data, 0, sizeof(data));

    int igd = UPNP_GetValidIGD(devlist, &urls, &data,
                               lanaddr, sizeof(lanaddr),
                               wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devlist);

    if (igd == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] no valid IGD among discovered devices");
        SetState(State::NoIgd);
        return;
    }

    // The control-URL host is the most stable identifier miniupnpc exposes
    // for an IGD (there is no friendlyName in IGDdatas). Log it + the LAN
    // addr we'll forward to.
    const std::string igd_desc =
        urls.controlURL ? std::string(urls.controlURL) : std::string();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[upnp] IGD selected: result=%d control=%s lan=%s",
                igd, igd_desc.c_str(), lanaddr);

    // CGNAT short-circuit #1: UPNP_PRIVATEIP_IGD (2) means the router itself
    // reports a private WAN IP -- it's behind another NAT (carrier-grade or
    // double-NAT), so any mapping we add is reachable only from inside that
    // inner network. Useless for internet peers. Record + bail. (The hub
    // re-validates this against the WS-source IP per D6, but catching it
    // here saves a pointless AddPortMapping and surfaces it in the UI.)
    if (igd == UPNP_PRIVATEIP_IGD) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] IGD WAN address is private (CGNAT/double-NAT) -- "
                    "mapping would be useless");
        {
            std::lock_guard<std::mutex> lk(status_mtx_);
            status_.state    = State::Cgnat;
            status_.igd_desc = igd_desc;
        }
        FreeUPNPUrls(&urls);
        return;
    }

    // Learn the external (WAN) IP. wanaddr from GetValidIGD may already hold
    // it, but call GetExternalIPAddress explicitly for the authoritative
    // value (some IGDs leave wanaddr empty).
    char ext_ip[64] = {0};
    int gei = UPNP_GetExternalIPAddress(urls.controlURL,
                                        data.first.servicetype, ext_ip);
    if (gei != UPNPCOMMAND_SUCCESS || ext_ip[0] == '\0') {
        // Fall back to the addr GetValidIGD gave us, if any.
        if (wanaddr[0] != '\0') {
            std::strncpy(ext_ip, wanaddr, sizeof(ext_ip) - 1);
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[upnp] external IP = %s (GetExternalIPAddress rc=%d)",
                ext_ip[0] ? ext_ip : "(unknown)", gei);

    // CGNAT short-circuit #2: even if GetValidIGD returned "connected", the
    // reported external IP can still be RFC1918/CGNAT (100.64/10 etc.). The
    // hub's D6 check is the authority (it compares against the real
    // WS-source IP), so we DON'T hard-fail here -- a mapping behind an inner
    // NAT is merely useless, not harmful, and the hub will ignore the claim.
    // We just note it. (No reliable client-side "is this CGNAT" without the
    // hub's outside view, so leave the verdict to the hub.)

    // The internal client for the mapping is our LAN address as seen by the
    // IGD (the address we reached it from). This is exactly what the game's
    // UDP socket binds on this host.
    const std::string in_port_str = PortStr(udp_port);
    const std::string in_client   = lanaddr;

    // D4 external-port selection: try ext==local first; on a 718
    // ConflictInMappingEntry, try local+1000, then 3 alternate high ports.
    // Advertise whatever was granted.
    uint16_t candidates[5];
    candidates[0] = udp_port;
    candidates[1] = static_cast<uint16_t>(udp_port + 1000);
    candidates[2] = 47000;
    candidates[3] = 48000;
    candidates[4] = 49000;

    bool     mapped     = false;
    uint16_t granted    = 0;
    // lease string; flipped to "0" (permanent) if the router only supports
    // permanent leases (725). Stop()'s DeletePortMapping cleans those up.
    std::string lease_str = PortStr(static_cast<uint16_t>(kLeaseSeconds));
    bool        permanent = false;

    for (int i = 0; i < 5 && !stop_.load(); ++i) {
        const uint16_t    ext_port = candidates[i];
        const std::string ext_str  = PortStr(ext_port);

        int r = UPNP_AddPortMapping(
            urls.controlURL, data.first.servicetype,
            ext_str.c_str(),     // external port
            in_port_str.c_str(), // internal port
            in_client.c_str(),   // internal client (our LAN IP)
            kMappingDesc,        // description
            "UDP",               // protocol
            nullptr,             // remoteHost: wildcard (any source)
            lease_str.c_str());  // lease duration

        if (r == UPNPCOMMAND_SUCCESS) {
            mapped  = true;
            granted = ext_port;
            break;
        }

        // 725 OnlyPermanentLeasesSupported: the router refuses a timed
        // lease. Retry the SAME external port with lease 0 (permanent); we
        // rely on Stop()'s DeletePortMapping to clean it up on exit. Don't
        // advance the candidate index -- it wasn't a conflict.
        if (r == kUpnpErrOnlyPermanentLeases && !permanent) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[upnp] router only supports permanent leases (725); "
                        "retrying ext %u with lease 0", ext_port);
            permanent = true;
            lease_str = "0";
            --i;             // retry this same candidate with the new lease
            continue;
        }

        // 718 ConflictInMappingEntry: the external port is taken by some
        // other host/mapping. Fall through to the next D4 candidate.
        if (r == kUpnpErrConflictInMapping) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[upnp] ext port %u conflict (718); trying next "
                        "candidate", ext_port);
            continue;
        }

        // Any other error: log it (with the UPnP error string) and keep
        // trying remaining candidates -- some routers return odd codes for
        // a busy port.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] AddPortMapping ext %u failed: rc=%d (%s)",
                    ext_port, r, strupnperror(r));
    }

    if (!mapped) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] all AddPortMapping candidates failed for in-port %u",
                    udp_port);
        {
            std::lock_guard<std::mutex> lk(status_mtx_);
            status_.state    = State::Failed;
            status_.ext_ip   = ext_ip;
            status_.igd_desc = igd_desc;
        }
        FreeUPNPUrls(&urls);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        status_.state        = State::Mapped;
        status_.ext_ip       = ext_ip;
        status_.ext_udp_port = granted;
        status_.igd_desc     = igd_desc;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[upnp] MAPPED ext %s:%u -> %s:%u (UDP, lease %s) via %s",
                ext_ip[0] ? ext_ip : "(unknown)", granted,
                in_client.c_str(), udp_port,
                permanent ? "permanent" : lease_str.c_str(),
                igd_desc.c_str());

    // Renewal loop: re-issue AddPortMapping at half-life so leased routers
    // don't drop us mid-session. Poll stop_ frequently (1s granularity) so
    // teardown is snappy rather than waiting out the full renew interval.
    const std::string ext_str = PortStr(granted);
    int slept = 0;
    while (!stop_.load()) {
        SDL_Delay(1000);
        slept += 1;
        if (slept < kRenewSeconds) {
            continue;
        }
        slept = 0;
        int r = UPNP_AddPortMapping(
            urls.controlURL, data.first.servicetype,
            ext_str.c_str(), in_port_str.c_str(), in_client.c_str(),
            kMappingDesc, "UDP", nullptr, lease_str.c_str());
        if (r == UPNPCOMMAND_SUCCESS) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[upnp] renewed mapping ext %u (lease %s)",
                        granted, permanent ? "permanent" : lease_str.c_str());
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[upnp] renewal failed for ext %u: rc=%d (%s)",
                        granted, r, strupnperror(r));
        }
    }

    // Teardown: delete the mapping we created. Best-effort; even if the
    // router already expired the lease, DeletePortMapping just returns an
    // error we log and move on.
    int dr = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
                                    ext_str.c_str(), "UDP", nullptr);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[upnp] unmapped ext %u (DeletePortMapping rc=%d)", granted, dr);

    FreeUPNPUrls(&urls);
}

}  // namespace fm2k
