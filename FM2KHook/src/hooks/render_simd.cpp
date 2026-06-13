#include "render_simd.h"
#include "version_local.h"   // fm2k::kAppBranch -- default-on gated to bleeding builds
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>   // SSE2 (file built with -msse2; see FM2KHook/CMakeLists.txt)

// Engine globals (absolute addresses, FM2K WonderfulWorld/RoHe layout).
// All verified in IDA (port 13337):
//   ppvBits @ 0x4246CC      : uint16_t* framebuffer base (640x480, 1280 B/row)
//   g_graphics_mode @ 0x424704 : !=0 -> RGB565, ==0 -> RGB555
//   g_object_data_ptr @ 0x4CFA00 : current object base; byte fields:
//     +0x10 render_mode (-1 sprite / -10 blur / >0 stage bg)
//     +0x50 alpha (mode 4, uint16) ; +0x54 blend mode (0=copy..4=alpha)
// Sprite struct (a1): +4 = source row stride (bytes), +8 = source height.
static constexpr uintptr_t ADDR_PPVBITS         = 0x4246CC;
static constexpr uintptr_t ADDR_GRAPHICS_MODE   = 0x424704;
static constexpr uintptr_t ADDR_OBJECT_DATA_PTR = 0x4CFA00;

namespace fm2k::render_simd {

Config ParseConfig() {
    Config c;
    const char* v = std::getenv("FM2K_BLIT_SIMD");
    if (!v || !v[0]) {
        // Unset -> default ON for bleeding (the dev/testing channel) so the
        // reimplementation gets broad real-world validation before it can ever
        // reach stable; OFF on stable/other branches, where the engine's own
        // renderer stays the proven default until this is validated. An explicit
        // FM2K_BLIT_SIMD always overrides (incl. "0" to opt out). Bleeding
        // releases are built ON the bleeding branch (cut_release builds the
        // working tree, no detached checkout), so kAppBranch is reliable here.
        if (std::strcmp(fm2k::kAppBranch, "bleeding") == 0) c.mode = Mode::Simd;
        return c;
    }
    if (v[0] == '0' && v[1] == '\0') return c;             // explicit opt-out
    if (std::strstr(v, "scalar"))      c.mode = Mode::Scalar;
    else                               c.mode = Mode::Simd;  // "1"/"simd"/other
    if (std::strstr(v, "verify"))      c.verify = true;
    return c;
}

// ---------------------------------------------------------------------------
// Per-pixel blend (bit-exact transcription of the 5 modes x 565/555 from the
// engine's BlitSpriteWithBlendMode @ 0x40C140). Transparency (color==0 -> keep
// dst) is folded in. Used by BOTH the scalar path AND the SSE2 remainder tail,
// so they can never drift from each other. `a`/`inv` are the mode-4 alpha
// weights (inv = 32 - a), read once by the caller.
// ---------------------------------------------------------------------------
static inline uint16_t blend_pixel(int blend, bool fmt565, uint16_t color,
                                   uint16_t dstv, int a, int inv) {
    if (blend != 0 && color == 0) return dstv;   // modes 1-4: transparent keeps dst
    switch (blend) {
    case 0:
        return color ? color : dstv;
    case 1: {
        const uint16_t m = fmt565 ? 0x7BEF : 0x3DEF;
        return (uint16_t)(((color >> 1) & m) + ((dstv >> 1) & m));
    }
    case 2:
        if (fmt565) {
            int rr = (dstv & 0x1F)   + (color & 0x1F);   if (rr > 31)    rr = 31;
            int gg = (dstv & 0x7E0)  + (color & 0x7E0);  if (gg > 2016)  gg = 2016;
            int bb = (dstv & 0xF800) + (color & 0xF800); if (bb > 63488) bb = 0xF800;
            return (uint16_t)(rr + gg + bb);
        } else {
            uint16_t v = (uint16_t)((color & 0x7BDF) + (dstv & 0x7BDF));
            return (uint16_t)(v | ((v & 0x8420) - ((v & 0x8420) >> 5)));
        }
    case 3:
        if (fmt565) {
            int rr = (dstv & 0x1F)   - (color & 0x1F);   if (rr < 0)    rr = 0;
            int gg = (dstv & 0x7E0)  - (color & 0x7E0);  if (gg < 64)   gg = 0;
            int bb = (dstv & 0xF800) - (color & 0xF800); if (bb < 2048) bb = 0;
            return (uint16_t)(rr + gg + bb);
        } else {
            int rr = (dstv & 0x1F)   - (color & 0x1F);   if (rr < 0)    rr = 0;
            int gg = (dstv & 0x3E0)  - (color & 0x3E0);  if (gg < 31)   gg = 0;
            int bb = (dstv & 0x7C00) - (color & 0x7C00); if (bb < 1023) bb = 0;
            return (uint16_t)(rr + gg + bb);
        }
    case 4:
        if (fmt565) {
            int gp = inv * (color & 0x7E0)  + a * (dstv & 0x7E0);
            int rp = inv * (color & 0x1F)   + a * (dstv & 0x1F);
            int part = ((rp >> 5) & 0x1F) + ((gp >> 5) & 0x7E0);
            int bp = inv * (color & 0xF800) + a * (dstv & 0xF800);
            return (uint16_t)((((bp) >> 5) & 0xF800) + part);
        } else {
            int gp = a * (dstv & 0x3E0)  + inv * (color & 0x3E0);
            int rp = a * (dstv & 0x1F)   + inv * (color & 0x1F);
            int part = ((rp >> 5) & 0x1F) + ((gp >> 5) & 0x3E0);
            int bp = a * (dstv & 0x7C00) + inv * (color & 0x7C00);
            return (uint16_t)((((bp) >> 5) & 0x7C00) + part);
        }
    default:
        return dstv;
    }
}

// SSE2 blend of 8 pixels for modes 0-3 (565/555). Returns the blended vector
// BEFORE transparency; the caller applies the color!=0 keep-mask. Each lane is
// the same arithmetic blend_pixel() does, proven bit-exact via FM2K_BLIT_SIMD
// =simd,verify. (Mode 4 is left to the scalar path -- its products need 32-bit
// intermediates that epi16 can't hold without unpacking.)
static inline __m128i blend8(int blend, bool fmt565, __m128i color, __m128i d) {
    switch (blend) {
    case 0:
        return color;
    case 1: {
        const __m128i m = _mm_set1_epi16(fmt565 ? 0x7BEF : 0x3DEF);
        return _mm_add_epi16(_mm_and_si128(_mm_srli_epi16(color, 1), m),
                             _mm_and_si128(_mm_srli_epi16(d,     1), m));
    }
    case 2:
        if (fmt565) {
            __m128i r = _mm_min_epi16(
                _mm_add_epi16(_mm_and_si128(color, _mm_set1_epi16(0x1F)),
                              _mm_and_si128(d,     _mm_set1_epi16(0x1F))),
                _mm_set1_epi16(31));
            __m128i g = _mm_min_epi16(
                _mm_add_epi16(_mm_and_si128(color, _mm_set1_epi16(0x7E0)),
                              _mm_and_si128(d,     _mm_set1_epi16(0x7E0))),
                _mm_set1_epi16(0x7E0));
            // blue shifted to low 5 bits to dodge the 0xF800 epi16 sign trap
            __m128i b = _mm_min_epi16(
                _mm_add_epi16(_mm_srli_epi16(color, 11), _mm_srli_epi16(d, 11)),
                _mm_set1_epi16(31));
            return _mm_or_si128(_mm_or_si128(r, g), _mm_slli_epi16(b, 11));
        } else {
            __m128i v = _mm_add_epi16(_mm_and_si128(color, _mm_set1_epi16(0x7BDF)),
                                      _mm_and_si128(d,     _mm_set1_epi16(0x7BDF)));
            __m128i carry = _mm_and_si128(v, _mm_set1_epi16(0x8420));
            return _mm_or_si128(v, _mm_sub_epi16(carry, _mm_srli_epi16(carry, 5)));
        }
    case 3: {
        const __m128i z = _mm_setzero_si128();
        __m128i r = _mm_max_epi16(
            _mm_sub_epi16(_mm_and_si128(d,     _mm_set1_epi16(0x1F)),
                          _mm_and_si128(color, _mm_set1_epi16(0x1F))), z);
        if (fmt565) {
            __m128i graw = _mm_sub_epi16(_mm_and_si128(d,     _mm_set1_epi16(0x7E0)),
                                         _mm_and_si128(color, _mm_set1_epi16(0x7E0)));
            __m128i g = _mm_andnot_si128(_mm_cmpgt_epi16(_mm_set1_epi16(0x40), graw), graw);
            // blue low (sign trap): keep diff only where >= 1
            __m128i bb = _mm_sub_epi16(_mm_srli_epi16(d, 11), _mm_srli_epi16(color, 11));
            __m128i b = _mm_slli_epi16(_mm_and_si128(_mm_cmpgt_epi16(bb, z), bb), 11);
            return _mm_or_si128(_mm_or_si128(r, g), b);
        } else {
            __m128i graw = _mm_sub_epi16(_mm_and_si128(d,     _mm_set1_epi16(0x3E0)),
                                         _mm_and_si128(color, _mm_set1_epi16(0x3E0)));
            __m128i g = _mm_andnot_si128(_mm_cmpgt_epi16(_mm_set1_epi16(0x1F), graw), graw);
            // 0x7C00 is positive as int16 (top bit unused in 555) -> in-position OK
            __m128i braw = _mm_sub_epi16(_mm_and_si128(d,     _mm_set1_epi16(0x7C00)),
                                         _mm_and_si128(color, _mm_set1_epi16(0x7C00)));
            __m128i b = _mm_andnot_si128(_mm_cmpgt_epi16(_mm_set1_epi16(0x3FF), braw), braw);
            return _mm_or_si128(_mm_or_si128(r, g), b);
        }
    }
    default:
        return d;
    }
}

// ---------------------------------------------------------------------------
// Row-banded parallel compositing. A blit's rows are independent (no cross-row
// dependency), so large blits split across a small worker pool (cores-1 +
// main), each band writing disjoint framebuffer rows. Pure pixel work -- no sim
// state, no DDraw -- barrier before return. This is what lifts the copy-bound
// heaviest stages (Aubeclisse) past 100fps: copy mode is at the scalar floor
// and can only be beaten by more cores.
// ---------------------------------------------------------------------------
namespace {

struct BlitJob {
    int mode; bool fmt565; bool use_simd;
    const uint16_t* LUT;
    uint16_t* dst_base;        // row-0 dest start
    const uint8_t* src_base;   // row-0 src start
    int clip_w, clip_h;
    int src_x_step, src_row_adv, dst_row_gap;
    int per_row_src;           // src advance per full row (band-start math)
    int a, inv;
};

// Composite rows [y0, y1) of `j`. Identical arithmetic to the single-threaded
// path -- only the row range differs -- so it stays bit-exact.
static void run_rows(const BlitJob& j, int y0, int y1) {
    const uint16_t* LUT = j.LUT;
    const int step = j.src_x_step, W = j.clip_w;
    const int mode = j.mode; const bool fmt565 = j.fmt565;
    const int a = j.a, inv = j.inv;
    uint16_t* dst = j.dst_base + (size_t)y0 * 640;
    const uint8_t* src = j.src_base + (ptrdiff_t)y0 * j.per_row_src;

    if (mode == 0) {                       // tight copy (no dst read) ~ engine
        for (int r = y0; r < y1; ++r) {
            for (int c = 0; c < W; ++c) { uint16_t col = LUT[*src]; if (col) *dst = col; ++dst; src += step; }
            src += j.src_row_adv; dst += j.dst_row_gap;
        }
    } else if (j.use_simd && mode >= 1 && mode <= 3) {
        const __m128i zero = _mm_setzero_si128();
        for (int r = y0; r < y1; ++r) {
            int c = 0;
            for (; c + 8 <= W; c += 8) {
                const uint8_t* sp = src;
                __m128i color = _mm_setzero_si128();
                color = _mm_insert_epi16(color, LUT[*sp], 0); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 1); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 2); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 3); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 4); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 5); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 6); sp += step;
                color = _mm_insert_epi16(color, LUT[*sp], 7);
                __m128i d = _mm_loadu_si128((const __m128i*)dst);
                __m128i keep = _mm_cmpeq_epi16(color, zero);
                __m128i blended = blend8(mode, fmt565, color, d);
                _mm_storeu_si128((__m128i*)dst,
                                 _mm_or_si128(_mm_and_si128(keep, d), _mm_andnot_si128(keep, blended)));
                dst += 8; src += 8 * step;
            }
            for (; c < W; ++c) { *dst = blend_pixel(mode, fmt565, LUT[*src], *dst, a, inv); ++dst; src += step; }
            src += j.src_row_adv; dst += j.dst_row_gap;
        }
    } else {                               // scalar (mode 4, or non-simd 1-3)
        for (int r = y0; r < y1; ++r) {
            for (int c = 0; c < W; ++c) { *dst = blend_pixel(mode, fmt565, LUT[*src], *dst, a, inv); ++dst; src += step; }
            src += j.src_row_adv; dst += j.dst_row_gap;
        }
    }
}

constexpr int kMaxWorkers = 7;            // + main thread = up to 8-way (clamped to cores-1)
struct Pool {
    bool started = false;
    int n = 0;
    HANDLE start[kMaxWorkers] = {};
    HANDLE done[kMaxWorkers]  = {};
    HANDLE th[kMaxWorkers]    = {};
    void (*fn)(void*, int, int) = nullptr;   // band callback: fn(ctx, lo, hi)
    void* ctx = nullptr;
    int band[kMaxWorkers + 2] = {};          // band i spans [band[i], band[i+1])
};
static Pool g_pool;

static DWORD WINAPI WorkerMain(void* p) {
    const int idx = (int)(intptr_t)p;
    for (;;) {
        WaitForSingleObject(g_pool.start[idx], INFINITE);
        g_pool.fn(g_pool.ctx, g_pool.band[idx], g_pool.band[idx + 1]);
        SetEvent(g_pool.done[idx]);
    }
}

static void EnsurePool() {
    if (g_pool.started) return;
    g_pool.started = true;
    SYSTEM_INFO si; GetSystemInfo(&si);
    int workers = (int)si.dwNumberOfProcessors - 1;
    if (workers < 0) workers = 0;
    if (workers > kMaxWorkers) workers = kMaxWorkers;
    g_pool.n = workers;
    for (int i = 0; i < workers; ++i) {
        g_pool.start[i] = CreateEvent(nullptr, FALSE, FALSE, nullptr);  // auto-reset
        g_pool.done[i]  = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        g_pool.th[i]    = CreateThread(nullptr, 0, WorkerMain, (void*)(intptr_t)i, 0, nullptr);
    }
}

// Generic parallel-for over [0,total): split into n+1 contiguous bands, workers
// take 0..n-1, main takes the last, barrier on the worker done-events. Used for
// both the blit (rows) and the blur (linear pixels).
static void run_parallel(void (*fn)(void*, int, int), void* ctx, int total) {
    EnsurePool();
    const int nb = g_pool.n + 1;
    if (nb < 2 || total < nb) { fn(ctx, 0, total); return; }
    const int base = total / nb, extra = total % nb;
    int y = 0;
    for (int i = 0; i < nb; ++i) { g_pool.band[i] = y; y += base + (i < extra ? 1 : 0); }
    g_pool.band[nb] = total;
    g_pool.fn = fn; g_pool.ctx = ctx;
    for (int i = 0; i < g_pool.n; ++i) SetEvent(g_pool.start[i]);
    fn(ctx, g_pool.band[g_pool.n], g_pool.band[nb]);       // main does the last band
    if (g_pool.n) WaitForMultipleObjects(g_pool.n, g_pool.done, TRUE, INFINITE);
}

static void blit_band(void* ctx, int y0, int y1) { run_rows(*(const BlitJob*)ctx, y0, y1); }

// case -10 blur band over linear pixels [i0,i1). Reads from `s` (a stable
// pre-pass copy) and writes fb -- so bands never read each other's writes.
struct BlurCtx { uint16_t* fb; const uint16_t* s; uint16_t mask; };
static void blur_band(void* c, int i0, int i1) {
    const BlurCtx& b = *(const BlurCtx*)c;
    uint16_t* fb = b.fb; const uint16_t* s = b.s; const uint16_t mv = b.mask;
    const __m128i m = _mm_set1_epi16((short)mv);
    int i = i0;
    for (; i + 8 <= i1; i += 8) {
        __m128i a  = _mm_and_si128(_mm_srli_epi16(_mm_loadu_si128((const __m128i*)(s + i)),       2), m);
        __m128i rt = _mm_and_si128(_mm_srli_epi16(_mm_loadu_si128((const __m128i*)(s + i + 1)),   2), m);
        __m128i bl = _mm_and_si128(_mm_srli_epi16(_mm_loadu_si128((const __m128i*)(s + i + 640)), 2), m);
        __m128i br = _mm_and_si128(_mm_srli_epi16(_mm_loadu_si128((const __m128i*)(s + i + 641)), 2), m);
        _mm_storeu_si128((__m128i*)(fb + i), _mm_add_epi16(_mm_add_epi16(a, rt), _mm_add_epi16(bl, br)));
    }
    for (; i < i1; ++i)
        fb[i] = (uint16_t)(((s[i] >> 2) & mv) + ((s[i + 1] >> 2) & mv)
                         + ((s[i + 640] >> 2) & mv) + ((s[i + 641] >> 2) & mv));
}

}  // namespace

// ---------------------------------------------------------------------------
// BlitSpriteWithBlendMode @ 0x40C140 reimplementation. The clip + source-walk
// setup mirrors the engine exactly (runs once), then the rows are composited
// via run_rows -- inline, or row-banded across the worker pool when
// allow_threads and the blit is tall enough. Display-only: no sim state.
// ---------------------------------------------------------------------------
int BlitSprite(int sprite_desc, int src_pixels, int palette_lut,
               int dst_x, int dst_y, int width, int height,
               short flags, bool use_simd, bool allow_threads) {
    const int stride = *(int*)(sprite_desc + 4);   // source row stride (bytes)
    const int src_h  = *(int*)(sprite_desc + 8);   // source height (rows)

    // Clip the dest rect to the 640x480 screen (mirrors the engine exactly).
    int x = dst_x, top_clip = 0, clip_w = width, left_clip = 0;
    if (dst_x + width >= 640) clip_w = 640 - dst_x;
    int ry = dst_y, clip_h = height;
    if (dst_y + height >= 480) clip_h = 480 - dst_y;
    if (dst_x < 0) { clip_w += dst_x; left_clip = -dst_x; x = 0; }
    if (dst_y < 0) { clip_h += dst_y; top_clip = -(dst_y * stride); ry = 0; }
    if (clip_w <= 0 || clip_h <= 0) return dst_y;  // engine's empty-rect return (unused)

    uint8_t* fb = *(uint8_t**)ADDR_PPVBITS;
    uint16_t* dst = (uint16_t*)(fb + 1280 * ry + 2 * x);
    const int dst_row_gap = 640 - clip_w;          // pixels skipped per dst row

    const uint8_t* src;                            // source pixel cursor
    int src_x_step;                                // +1, or -1 when h-flipped
    int src_row_adv;                               // src advance at each row end
    if (flags & 1) {                               // horizontal flip
        src_x_step = -1;
        if (flags >= 0) {
            src = (const uint8_t*)(src_pixels + top_clip + stride - left_clip - 1);
            src_row_adv = clip_w + stride;
        } else {                                   // horizontal + vertical flip
            src = (const uint8_t*)(src_pixels + stride * src_h - top_clip - left_clip - 1);
            src_row_adv = clip_w - stride;
        }
    } else {
        if (flags >= 0) {
            src = (const uint8_t*)(left_clip + top_clip + src_pixels);
            src_row_adv = stride - clip_w;
        } else {                                   // vertical flip
            src = (const uint8_t*)(left_clip + stride * (src_h - 1) - top_clip + src_pixels);
            src_row_adv = -(clip_w + stride);
        }
        src_x_step = 1;
    }

    const uint16_t* LUT = (const uint16_t*)palette_lut;
    const uint32_t obj  = *(uint32_t*)ADDR_OBJECT_DATA_PTR;
    const int blend     = obj ? *(int*)(obj + 0x54) : 0;
    const bool fmt565   = (*(int*)ADDR_GRAPHICS_MODE != 0);
    const int a         = obj ? *(uint16_t*)(obj + 0x50) : 0;  // mode-4 alpha
    const int inv       = 32 - a;

    BlitJob job;
    job.mode = blend; job.fmt565 = fmt565; job.use_simd = use_simd;
    job.LUT = LUT; job.dst_base = dst; job.src_base = src;
    job.clip_w = clip_w; job.clip_h = clip_h;
    job.src_x_step = src_x_step; job.src_row_adv = src_row_adv; job.dst_row_gap = dst_row_gap;
    job.per_row_src = clip_w * src_x_step + src_row_adv;
    job.a = a; job.inv = inv;

    if (allow_threads && clip_h >= 64) run_parallel(blit_band, &job, clip_h);
    else                               run_rows(job, 0, clip_h);

    return (int)(intptr_t)(dst + (size_t)clip_h * 640);  // engine returns advanced dst (unused)
}

// ---------------------------------------------------------------------------
// case -10 full-screen feedback blur -- bit-exact reimplementation.
// The engine walks ppvBits LINEARLY (the 478x638 nest is just a 305044
// counter; the 4 pointers self/right/below/belowright never reset per row).
// Each pixel = sum of (neighbor>>2)&mask over {self, +1, +640, +641}, written
// to self. All reads are of higher-index (not-yet-written) pixels, so a pass
// has no read-after-write hazard -> the SSE2 8-wide store is bit-exact too.
// ---------------------------------------------------------------------------
void BlurFullscreen(uint16_t* fb, int passes, bool fmt565, bool use_simd) {
    const uint16_t maskv = fmt565 ? 0x39E7 : 0x1CE7;
    const int N = 638 * 478;  // 305044 consecutive pixels from fb[0]

    if (!use_simd) {
        for (int p = 0; p < passes; ++p)
            for (int i = 0; i < N; ++i)
                fb[i] = (uint16_t)(((fb[i]       >> 2) & maskv) + ((fb[i + 1]   >> 2) & maskv)
                                 + ((fb[i + 640] >> 2) & maskv) + ((fb[i + 641] >> 2) & maskv));
        return;
    }

    // Threaded: double-buffer so every band reads stable (pre-pass) neighbor
    // values -- bands then write disjoint fb pixels with no races, bit-exact vs
    // the in-place engine blur (which also only ever reads pre-pass values).
    static uint16_t s_scratch[640 * 480];
    BlurCtx ctx{ fb, s_scratch, maskv };
    for (int p = 0; p < passes; ++p) {
        std::memcpy(s_scratch, fb, (size_t)(N + 641) * 2);   // cover neighbor reach
        run_parallel(blur_band, &ctx, N);
    }
}

}  // namespace fm2k::render_simd
