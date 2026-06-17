#pragma once
// Shared mirror of the SessionEvent wire protocol (split from
// test_spectator_protocol.cpp). Used by both the event encode/decode tests
// and the session-file/seek tests. Free functions are inline for multi-TU use.
#include <cstdint>
#include <cstring>
#include <vector>

namespace mirror_event {
enum class SessionEventType : uint8_t {
    INPUT             = 1,
    PIN_RNG           = 2,
    RESET_INPUT_STATE = 3,
    SOUND_INIT        = 4,
    MATCH_START       = 5,
    MATCH_END         = 6,
    FINGERPRINT       = 7,
    ROUND_START       = 8,
    ROUND_END         = 9,
    SESSION_ID        = 10,  // C7
};

constexpr size_t SESSION_EVENT_MATCH_HDR_SIZE = 96;
constexpr size_t SESSION_EVENT_MAX_WIRE_SIZE  = 1 + SESSION_EVENT_MATCH_HDR_SIZE;

#pragma pack(push, 1)
struct RoundStartPayload {
    uint8_t  round_idx;
    uint16_t p1_hp_max;
    uint16_t p2_hp_max;
    uint16_t timer_seconds;
};
struct RoundEndPayload {
    uint8_t  winner_idx;
    uint16_t p1_hp_remaining;
    uint16_t p2_hp_remaining;
    uint32_t frames_elapsed;
};
struct MatchEndPayload {
    uint8_t  winner_idx;
    uint8_t  rounds_won_p1;
    uint8_t  rounds_won_p2;
    uint32_t frames_total;
};
struct SessionEvent {
    SessionEventType type;
    union {
        struct { uint16_t p1; uint16_t p2; } input;
        uint32_t                             pin_rng_seed;
        uint32_t                             fingerprint_hash;
        uint16_t                             match_start_idx;
        RoundStartPayload                    round_start;
        RoundEndPayload                      round_end;
        MatchEndPayload                      match_end;
        uint64_t                             session_id;
        uint8_t                              raw[9];
    } u;
};
#pragma pack(pop)

// Reimplemented encoders/decoders matching spectator_node.cpp. If these go
// out of sync with production, the contract tests below stay green but the
// DLL produces packets the spectator can't parse — that's the regression
// the production-side analogous tests at the C++ build layer would catch
// (added in a follow-up commit once we have a way to link DLL-builds on
// Linux). For now the symmetry is hand-enforced.
inline size_t WirePayloadSize(SessionEventType t) {
    switch (t) {
        case SessionEventType::INPUT:             return 4;
        case SessionEventType::PIN_RNG:           return 4;
        case SessionEventType::RESET_INPUT_STATE: return 0;
        case SessionEventType::SOUND_INIT:        return 0;
        case SessionEventType::MATCH_START:       return SESSION_EVENT_MATCH_HDR_SIZE;
        case SessionEventType::MATCH_END:         return sizeof(MatchEndPayload);
        case SessionEventType::FINGERPRINT:       return 4;
        case SessionEventType::ROUND_START:       return sizeof(RoundStartPayload);
        case SessionEventType::ROUND_END:         return sizeof(RoundEndPayload);
        case SessionEventType::SESSION_ID:        return 8;
    }
    return SIZE_MAX;
}

inline bool IsValidEventTag(uint8_t tag) {
    return tag >= 1 && tag <= 10;
}

inline size_t EncodeSessionId(uint8_t* out, size_t cap, uint64_t session_id) {
    if (cap < 9) return 0;
    out[0] = (uint8_t)SessionEventType::SESSION_ID;
    std::memcpy(out + 1, &session_id, 8);
    return 9;
}

inline size_t EncodeRoundStart(uint8_t* out, size_t cap, const RoundStartPayload& p) {
    if (cap < 1 + sizeof(p)) return 0;
    out[0] = (uint8_t)SessionEventType::ROUND_START;
    std::memcpy(out + 1, &p, sizeof(p));
    return 1 + sizeof(p);
}

inline size_t EncodeRoundEnd(uint8_t* out, size_t cap, const RoundEndPayload& p) {
    if (cap < 1 + sizeof(p)) return 0;
    out[0] = (uint8_t)SessionEventType::ROUND_END;
    std::memcpy(out + 1, &p, sizeof(p));
    return 1 + sizeof(p);
}

inline size_t EncodeInput(uint8_t* out, size_t cap, uint16_t p1, uint16_t p2) {
    if (cap < 5) return 0;
    out[0] = (uint8_t)SessionEventType::INPUT;
    std::memcpy(out + 1, &p1, 2);
    std::memcpy(out + 3, &p2, 2);
    return 5;
}
inline size_t EncodePinRng(uint8_t* out, size_t cap, uint32_t seed) {
    if (cap < 5) return 0;
    out[0] = (uint8_t)SessionEventType::PIN_RNG;
    std::memcpy(out + 1, &seed, 4);
    return 5;
}
inline size_t EncodeResetInputState(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = (uint8_t)SessionEventType::RESET_INPUT_STATE;
    return 1;
}
inline size_t EncodeSoundInit(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = (uint8_t)SessionEventType::SOUND_INIT;
    return 1;
}
inline size_t EncodeMatchStart(uint8_t* out, size_t cap, const uint8_t header[96]) {
    if (cap < 1 + 96) return 0;
    out[0] = (uint8_t)SessionEventType::MATCH_START;
    std::memcpy(out + 1, header, 96);
    return 1 + 96;
}
inline size_t EncodeMatchEnd(uint8_t* out, size_t cap, const MatchEndPayload& p) {
    if (cap < 1 + sizeof(p)) return 0;
    out[0] = (uint8_t)SessionEventType::MATCH_END;
    std::memcpy(out + 1, &p, sizeof(p));
    return 1 + sizeof(p);
}
inline size_t EncodeFingerprint(uint8_t* out, size_t cap, uint32_t hash) {
    if (cap < 5) return 0;
    out[0] = (uint8_t)SessionEventType::FINGERPRINT;
    std::memcpy(out + 1, &hash, 4);
    return 5;
}

inline size_t Decode(const uint8_t* in, size_t in_len, SessionEvent* out_event,
              uint8_t out_match_header[96]) {
    if (in_len < 1) return 0;
    const uint8_t tag = in[0];
    if (!IsValidEventTag(tag)) return 0;
    const SessionEventType type = (SessionEventType)tag;
    const size_t payload = WirePayloadSize(type);
    if (in_len < 1 + payload) return 0;

    if (out_event) {
        out_event->type = type;
        std::memset(&out_event->u, 0, sizeof(out_event->u));
        switch (type) {
            case SessionEventType::INPUT:
                std::memcpy(&out_event->u.input.p1, in + 1, 2);
                std::memcpy(&out_event->u.input.p2, in + 3, 2);
                break;
            case SessionEventType::PIN_RNG:
                std::memcpy(&out_event->u.pin_rng_seed, in + 1, 4);
                break;
            case SessionEventType::FINGERPRINT:
                std::memcpy(&out_event->u.fingerprint_hash, in + 1, 4);
                break;
            case SessionEventType::ROUND_START:
                std::memcpy(&out_event->u.round_start, in + 1, sizeof(RoundStartPayload));
                break;
            case SessionEventType::ROUND_END:
                std::memcpy(&out_event->u.round_end, in + 1, sizeof(RoundEndPayload));
                break;
            case SessionEventType::MATCH_END:
                std::memcpy(&out_event->u.match_end, in + 1, sizeof(MatchEndPayload));
                break;
            case SessionEventType::SESSION_ID:
                std::memcpy(&out_event->u.session_id, in + 1, 8);
                break;
            default: break;
        }
    }
    if (type == SessionEventType::MATCH_START && out_match_header) {
        std::memcpy(out_match_header, in + 1, 96);
    }
    return 1 + payload;
}
} // namespace mirror_event
