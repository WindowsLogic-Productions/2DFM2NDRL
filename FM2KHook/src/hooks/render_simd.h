#ifndef FM2K_RENDER_SIMD_H
#define FM2K_RENDER_SIMD_H
#include <cstdint>

// Display-only software-renderer reimplementation (FM2K_BLIT_SIMD).
//
// The heavy-stage cost is FM2K's per-sprite software compositing: render_game
// is 75-90% BlitSpriteWithBlendMode pushing 20-35 Mpx/frame of scalar 16-bit
// blits (RoHe Aubeclisse 10.75ms blit / 12ms render), plus an intermittent
// full-screen blur (case -10) on a few stages. Render NEVER feeds sim state
// (writes only the framebuffer + isolated render RNG), so reimplementing these
// loops is desync-safe -- the only risk is visual, caught by the pixel-diff
// verify mode.
//
// Mode is chosen by FM2K_BLIT_SIMD (read once at hook install):
//   unset / 0  -> original engine code (default; hooks not installed)
//   scalar     -> bit-exact scalar reimplementation (correctness reference)
//   simd       -> SSE2 vectorized (the speedup)
// Append ",verify" (e.g. "scalar,verify") to double-render each blit and
// compare our output against the original's, logging the first mismatch.
namespace fm2k::render_simd {

enum class Mode { Off, Scalar, Simd };

struct Config { Mode mode = Mode::Off; bool verify = false; };

// Parse FM2K_BLIT_SIMD once. Returns Off if unset/0.
Config ParseConfig();

// Faithful reimplementation of BlitSpriteWithBlendMode @ 0x40C140.
// Param names match the IDB prototype. `use_simd` picks SSE2 vs the bit-exact
// scalar path. Returns the engine's return value (advanced dest ptr as int),
// for drop-in replacement.
//   sprite_desc : sprite struct ([+4]=src row stride, [+8]=src height)
//   src_pixels  : 8-bit indexed source pixels
//   palette_lut : 256x uint16 color LUT (entry 0 == transparent)
//   dst_x/dst_y : destination top-left ; width/height : sprite size
//   flags       : bit0 = h-flip, sign bit = v-flip
// `allow_threads` row-bands large blits across a worker pool (cores-1 + main).
// Each band writes disjoint framebuffer rows -- pure pixel parallelism, no sim
// state, barrier before return. Small blits run inline regardless.
int BlitSprite(int sprite_desc, int src_pixels, int palette_lut,
               int dst_x, int dst_y, int width, int height,
               short flags, bool use_simd, bool allow_threads = false);

// Faithful reimplementation of the case -10 full-screen feedback blur.
// fb = framebuffer base (ppvBits), passes = obj[+342]/20, fmt565 from
// g_graphics_mode.
void BlurFullscreen(uint16_t* fb, int passes, bool fmt565, bool use_simd);

}  // namespace fm2k::render_simd
#endif
