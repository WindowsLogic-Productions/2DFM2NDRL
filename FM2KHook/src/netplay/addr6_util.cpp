#include "addr6_util.h"
#include <windows.h>
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>

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

void Addr_NormalizeRecv(const sockaddr_in6& from6, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    if (!s_socket_is_v6) {
        // v4-fallback socket: the kernel wrote a sockaddr_in into the buffer.
        std::memcpy(&out, &from6, sizeof(sockaddr_in));
        return;
    }
    if (Addr_IsV4Mapped(from6)) {
        sockaddr_in a4 = Addr_Unmap4(from6);
        std::memcpy(&out, &a4, sizeof(a4));
    } else {
        // native IPv6 source
        std::memcpy(&out, &from6, sizeof(sockaddr_in6));
    }
}

std::string Addr_ActorString(const sockaddr_storage& sa) {
    char ip[INET6_ADDRSTRLEN] = {};
    char buf[80] = {};
    if (sa.ss_family == AF_INET6) {
        const sockaddr_in6* a6 = reinterpret_cast<const sockaddr_in6*>(&sa);
        inet_ntop(AF_INET6, const_cast<in6_addr*>(&a6->sin6_addr), ip, sizeof(ip));
        std::snprintf(buf, sizeof(buf), "[%s]:%d", ip, (int)ntohs(a6->sin6_port));
    } else {
        const sockaddr_in* a4 = reinterpret_cast<const sockaddr_in*>(&sa);
        // BYTE-IDENTICAL to the legacy v4 format (inet_ntoa -> "%s:%d"); using
        // inet_ntop(AF_INET) here is reentrant and yields the same dotted quad.
        inet_ntop(AF_INET, const_cast<in_addr*>(&a4->sin_addr), ip, sizeof(ip));
        std::snprintf(buf, sizeof(buf), "%s:%d", ip, (int)ntohs(a4->sin_port));
    }
    return buf;
}

bool Addr_Equal(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET6) {
        const sockaddr_in6* x = reinterpret_cast<const sockaddr_in6*>(&a);
        const sockaddr_in6* y = reinterpret_cast<const sockaddr_in6*>(&b);
        return x->sin6_port == y->sin6_port &&
               std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0;
    }
    const sockaddr_in* x = reinterpret_cast<const sockaddr_in*>(&a);
    const sockaddr_in* y = reinterpret_cast<const sockaddr_in*>(&b);
    return x->sin_port == y->sin_port &&
           x->sin_addr.s_addr == y->sin_addr.s_addr;
}

bool Addr_HasPort(const sockaddr_storage& sa) {
    if (sa.ss_family == AF_INET6)
        return reinterpret_cast<const sockaddr_in6*>(&sa)->sin6_port != 0;
    return reinterpret_cast<const sockaddr_in*>(&sa)->sin_port != 0;
}

static void LogFamilyErr(int r, bool v6) {
    if (r == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err == WSAEAFNOSUPPORT || err == WSAEINVAL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "addr6: send family error %d (is_v6=%d) -- dual-stack misroute",
                err, (int)v6);
        }
    }
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
    LogFamilyErr(r, s_socket_is_v6);
    return r;
}

int SendtoStorage(SOCKET s, const char* buf, int len, const sockaddr_storage& dst) {
    if (dst.ss_family == AF_INET6) {
        // Native v6 -- send directly (NOT via the v4-mapped wrapper).
        int r = sendto(s, buf, len, 0,
                       reinterpret_cast<const sockaddr*>(&dst), sizeof(sockaddr_in6));
        LogFamilyErr(r, true);
        return r;
    }
    // v4 family: reuse the v4-mapped/bare logic.
    return Sendto4or6(s, buf, len, *reinterpret_cast<const sockaddr_in*>(&dst));
}

} // namespace fm2k
