#pragma once
// IPv4/IPv6 address helpers -- the SINGLE owner of the dual-stack-socket
// concern. The rest of the netcode keeps its v4 (sockaddr_in) assumptions.
//
// The shared UDP socket is opened AF_INET6 dual-stack (IPV6_V6ONLY=0) when the
// platform allows it; v4 peers are reached via v4-mapped (::ffff:a.b.c.d)
// addresses on that SAME handle. On hosts where IPv6 is disabled we soft-fall-
// back to a legacy AF_INET socket. Which one won is recorded here (set once at
// init via Addr_SetSocketV6); Sendto4or6 + Addr_RecvSourceToV4 branch on it so
// no v4/v6 logic leaks into the callers.
//
// LOAD-BEARING INVARIANT: GekkoNet drops any packet whose source string is not
// a byte-exact match of the actor string, which is formatted from a v4
// sockaddr_in (inet_ntoa). So all IN-MEMORY peer/local addresses stay
// sockaddr_in: inbound v4-mapped sources are un-mapped to AF_INET at the recv
// boundary (Addr_RecvSourceToV4), and Sendto4or6 re-maps only on the wire.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace fm2k {

// Record whether the shared socket is AF_INET6 dual-stack (true) or a legacy
// AF_INET fallback (false). Called once from NetSocket_Init before any send or
// receive runs -- write-once, then read concurrently (safe).
void Addr_SetSocketV6(bool is_v6);

// Low-level v4-mapped helpers (pure).
bool Addr_IsV4Mapped(const sockaddr_in6& a6);
sockaddr_in Addr_Unmap4(const sockaddr_in6& a6);   // precondition: v4-mapped
sockaddr_in6 Addr_Map4(const sockaddr_in& a4);     // -> ::ffff:<a4>

// Normalize a recvfrom source (received into a sockaddr_in6) to a real AF_INET
// sockaddr_in for the v4-only peer-learning / actor-string path. Returns true
// and fills `out` for a usable v4 source (v4-mapped on the dual-stack socket,
// or the plain sockaddr_in the kernel wrote on the v4-fallback socket); returns
// false for a native-IPv6 source (no v6 candidates in this build -- caller
// drops it).
bool Addr_RecvSourceToV4(const sockaddr_in6& from6, sockaddr_in& out);

// sendto a v4 destination on socket s: bare on the v4-fallback socket, or
// v4-mapped on the dual-stack socket. Returns the raw sendto() result (caller
// keeps its own WSAEWOULDBLOCK handling); logs only the family-mismatch errors
// that would otherwise be silent.
int Sendto4or6(SOCKET s, const char* buf, int len, const sockaddr_in& dst4);

} // namespace fm2k
