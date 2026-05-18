// Spec hub-relay shared-memory queue impl. See spec_relay_queue.h for
// the design rationale + memory ordering notes.
//
// Shared by hook (FM2KHook.dll) and launcher (FM2K_RollbackLauncher.exe)
// -- both compile this same .cpp. CMake includes the source from both
// targets (FM2KHook/CMakeLists.txt for the hook, top-level CMakeLists
// for the launcher). The functions are namespaced, so symbol collision
// at link time isn't a concern (each binary has its own copy).

#include "spec_relay_queue.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL_log.h>

namespace fm2k::spec_relay {

void MakeMappingName(char* buf, size_t buf_sz, bool is_outbound, uint32_t pid) {
    std::snprintf(buf, buf_sz, "FM2K_SpecRelay%s_%u",
                  is_outbound ? "Out" : "In", (unsigned)pid);
}

namespace {

// Common create-or-open helper. createIfMissing = true is the producer
// side (creates the kernel mapping); false is the consumer side (must
// have been created already by the producer).
Ring* OpenMapping(bool is_outbound, uint32_t pid, bool createIfMissing) {
    char name[128];
    MakeMappingName(name, sizeof(name), is_outbound, pid);

    HANDLE h = nullptr;
    if (createIfMissing) {
        h = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, (DWORD)sizeof(Ring), name);
        if (!h) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpecRelayQueue: CreateFileMapping(%s) failed (err=%lu)",
                name, GetLastError());
            return nullptr;
        }
    } else {
        h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!h) {
            // Not necessarily an error -- producer might not have started
            // yet, or might be in TCP mode and never created the mapping.
            // Caller decides whether to log.
            return nullptr;
        }
    }

    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring));
    if (!view) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpecRelayQueue: MapViewOfFile(%s) failed (err=%lu)",
            name, GetLastError());
        CloseHandle(h);
        return nullptr;
    }

    Ring* ring = static_cast<Ring*>(view);
    // Producer side initializes layout. Consumer side validates.
    if (createIfMissing) {
        std::memset(ring, 0, sizeof(Ring));
        ring->magic     = QUEUE_MAGIC;
        ring->version   = QUEUE_VERSION;
        ring->capacity  = QUEUE_CAPACITY;
        ring->slot_size = (uint32_t)sizeof(Slot);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpecRelayQueue: created '%s' (%u slots * %u B = %u KB)",
            name, (unsigned)QUEUE_CAPACITY, (unsigned)sizeof(Slot),
            (unsigned)(sizeof(Slot) * QUEUE_CAPACITY / 1024));
    } else {
        if (ring->magic != QUEUE_MAGIC ||
            ring->version != QUEUE_VERSION ||
            ring->capacity != QUEUE_CAPACITY ||
            ring->slot_size != sizeof(Slot)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpecRelayQueue: '%s' layout mismatch "
                "(magic=0x%X ver=%u cap=%u slot=%u); refusing to use",
                name, ring->magic, ring->version,
                ring->capacity, ring->slot_size);
            UnmapViewOfFile(view);
            CloseHandle(h);
            return nullptr;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpecRelayQueue: opened existing '%s'", name);
    }
    // We intentionally leak the HANDLE here. The kernel mapping object
    // is reference-counted; closing the HANDLE while the view is mapped
    // unmaps OK on UnmapViewOfFile. To support clean shutdown we'd
    // stash the handle alongside the Ring pointer, but the launcher
    // exits and the game DLL unloads naturally on quit. Re-open does a
    // fresh CreateFileMapping which is idempotent against existing
    // kernel objects of the same name.
    return ring;
}

}  // namespace

Ring* CreateOutboundHere() {
    return OpenMapping(/*outbound=*/true, GetCurrentProcessId(),
                       /*create=*/true);
}

Ring* OpenOutboundFor(uint32_t game_pid) {
    return OpenMapping(/*outbound=*/true, game_pid, /*create=*/false);
}

Ring* CreateInboundHere() {
    return OpenMapping(/*outbound=*/false, GetCurrentProcessId(),
                       /*create=*/true);
}

Ring* OpenInboundFor(uint32_t game_pid) {
    return OpenMapping(/*outbound=*/false, game_pid, /*create=*/false);
}

void Close(Ring* ring) {
    if (!ring) return;
    UnmapViewOfFile(ring);
    // Handle is leaked per the comment in OpenMapping. Kernel will reap
    // when the process exits.
}

bool Enqueue(Ring* ring,
             uint32_t target_kind,
             const char* spec_user_id,
             uint32_t spec_data_type,
             uint32_t frame_count,
             uint32_t spec_data_flags,
             const void* payload,
             uint32_t payload_len)
{
    if (!ring || ring->magic != QUEUE_MAGIC) return false;
    if (payload_len > SLOT_PAYLOAD_MAX) return false;

    const uint64_t write_idx = ring->write_idx;
    const uint64_t read_idx  = ring->read_idx;
    if (write_idx - read_idx >= QUEUE_CAPACITY) {
        ring->total_dropped = ring->total_dropped + 1;
        return false;
    }

    Slot* slot = &ring->slots[write_idx & (QUEUE_CAPACITY - 1)];
    slot->target_kind     = target_kind;
    slot->spec_data_type  = spec_data_type;
    slot->frame_count     = frame_count;
    slot->spec_data_flags = spec_data_flags;
    slot->payload_len     = payload_len;
    if (target_kind == TARGET_DIRECT && spec_user_id && spec_user_id[0]) {
        std::strncpy(slot->spec_user_id, spec_user_id,
                     sizeof(slot->spec_user_id) - 1);
        slot->spec_user_id[sizeof(slot->spec_user_id) - 1] = '\0';
    } else {
        slot->spec_user_id[0] = '\0';
    }
    if (payload && payload_len > 0) {
        std::memcpy(slot->payload, payload, payload_len);
    }

    // Release-store of write_idx: slot fields above must be visible to
    // the consumer before the index bump signals the slot is ready.
    // On x86 plain stores have release semantics; on weaker targets a
    // memory barrier would be needed. _ReadWriteBarrier is the MSVC /
    // MinGW intrinsic for compiler-fence; that's sufficient on x86.
    _ReadWriteBarrier();
    ring->write_idx = write_idx + 1;
    ring->total_enqueued = ring->total_enqueued + 1;
    return true;
}

const Slot* PeekFront(Ring* ring) {
    if (!ring || ring->magic != QUEUE_MAGIC) return nullptr;
    const uint64_t read_idx  = ring->read_idx;
    const uint64_t write_idx = ring->write_idx;
    _ReadWriteBarrier();  // acquire-side fence for write_idx
    if (read_idx == write_idx) return nullptr;
    return &ring->slots[read_idx & (QUEUE_CAPACITY - 1)];
}

void PopFront(Ring* ring) {
    if (!ring || ring->magic != QUEUE_MAGIC) return;
    const uint64_t read_idx = ring->read_idx;
    _ReadWriteBarrier();
    ring->read_idx = read_idx + 1;
    ring->total_dequeued = ring->total_dequeued + 1;
}

}  // namespace fm2k::spec_relay
