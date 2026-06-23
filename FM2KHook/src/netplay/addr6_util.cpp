#include "addr6_util.h"
#include <windows.h>
#include <SDL3/SDL_log.h>
#include <cstring>

namespace fm2k {

// Set once at init (NetSocket_Init) before any send/recv -- write-once, then
// read concurrently from the poll + punch threads, which is safe.
static bool s_socket_is_v6 = false;

void Addr_SetSocketV6(bool is_v6) { s_socket_is_v6 = is_v6; }

bool Addr_IsV4Mapped(const sockaddr_in6& a6) {
    return a6.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&a6.sin6_addr);
}

sockaddr_in Addr_Unmap4(const sockaddr_in6& a6) {
    sockaddr_in a4{};
    a4.sin_family = AF_INET;
    std::memcpy(&a4.sin_addr, &a6.sin6_addr.s6_addr[12], 4);
    a4.sin_port = a6.sin6_port;
    return a4;
}

sockaddr_in6 Addr_Map4(const sockaddr_in& a4) {
    sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    a6.sin6_addr.s6_addr[10] = 0xFF;  // ::ffff: prefix
    a6.sin6_addr.s6_addr[11] = 0xFF;
    std::memcpy(&a6.sin6_addr.s6_addr[12], &a4.sin_addr, 4);
    a6.sin6_port = a4.sin_port;
    return a6;
}

bool Addr_RecvSourceToV4(const sockaddr_in6& from6, sockaddr_in& out) {
    if (s_socket_is_v6) {
        if (!Addr_IsV4Mapped(from6)) return false;  // native v6 -- caller drops
        out = Addr_Unmap4(from6);
    } else {
        // v4-fallback socket: the kernel wrote a sockaddr_in into the buffer.
        std::memcpy(&out, &from6, sizeof(out));
    }
    return true;
}

int Sendto4or6(SOCKET s, const char* buf, int len, const sockaddr_in& dst4) {
    int r;
    if (s_socket_is_v6) {
        sockaddr_in6 d6 = Addr_Map4(dst4);
        r = sendto(s, buf, len, 0,
                   reinterpret_cast<const sockaddr*>(&d6), sizeof(d6));
    } else {
        r = sendto(s, buf, len, 0,
                   reinterpret_cast<const sockaddr*>(&dst4), sizeof(dst4));
    }
    if (r == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        // WSAEWOULDBLOCK is normal on a non-blocking socket; the caller handles
        // it. A family error means a v4 send hit the dual-stack socket wrong --
        // that would silently break the whole path, so surface it.
        if (err == WSAEAFNOSUPPORT || err == WSAEINVAL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Sendto4or6: family error %d (is_v6=%d) -- dual-stack misroute",
                err, (int)s_socket_is_v6);
        }
    }
    return r;
}

} // namespace fm2k
