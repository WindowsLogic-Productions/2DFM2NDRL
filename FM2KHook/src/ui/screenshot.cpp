#include "screenshot.h"

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

// Single-file public-domain PNG writer. MinGW's GDI+ headers were
// the alternative and they break the build with PROPID-not-declared
// errors that nobody has bothered fixing upstream. stb_image_write
// has zero deps and produces fine PNGs at this scale (320x240 - 1280x960).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../vendored/stb/stb_image_write.h"

namespace FM2KCapture {

namespace {

std::string  g_capture_dir;
bool         g_active = false;

// Cached HWND. EnumWindows on every screenshot would re-walk the
// top-level window list; once the game window comes up it doesn't
// move so we resolve once and stash.
HWND g_cached_hwnd = nullptr;

struct FindOwnWindowCtx {
    DWORD pid;
    HWND  result;
};
static BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindOwnWindowCtx*>(lparam);
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid != ctx->pid) return TRUE;
    char cls[32] = {0};
    if (GetClassNameA(hwnd, cls, sizeof(cls)) == 0) return TRUE;
    if (lstrcmpA(cls, "KGT2KGAME") != 0 &&
        lstrcmpA(cls, "KGT95GAME") != 0) return TRUE;
    ctx->result = hwnd;
    return FALSE;
}

HWND FindGameWindow() {
    if (g_cached_hwnd && IsWindow(g_cached_hwnd)) return g_cached_hwnd;
    FindOwnWindowCtx ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows(FindOwnWindowProc, reinterpret_cast<LPARAM>(&ctx));
    g_cached_hwnd = ctx.result;
    return g_cached_hwnd;
}

// BitBlt the window contents into a buffer of RGBA bytes
// (top-down, row-major), allocated by us. Caller frees via the
// returned vector going out of scope. Returns empty on failure.
std::vector<uint8_t> CaptureWindowRGBA(HWND hwnd, int& out_w, int& out_h) {
    out_w = out_h = 0;
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return {};
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return {};

    HDC hdc_win = GetDC(hwnd);
    if (!hdc_win) return {};
    HDC hdc_mem = CreateCompatibleDC(hdc_win);
    HBITMAP bmp = CreateCompatibleBitmap(hdc_win, w, h);
    HBITMAP old_bmp = (HBITMAP)SelectObject(hdc_mem, bmp);
    BOOL blt_ok = BitBlt(hdc_mem, 0, 0, w, h, hdc_win, 0, 0, SRCCOPY);

    std::vector<uint8_t> out;
    if (blt_ok) {
        // Negative biHeight = top-down DIB, which matches what
        // stb_image_write_png expects. 32 bpp BGRA from BitBlt; we
        // swap to RGBA in the loop below.
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<uint8_t> bgra((size_t)w * h * 4);
        if (GetDIBits(hdc_mem, bmp, 0, h,
                      bgra.data(), &bi, DIB_RGB_COLORS) == h) {
            out.resize(bgra.size());
            for (size_t i = 0; i < bgra.size(); i += 4) {
                out[i + 0] = bgra[i + 2];   // R
                out[i + 1] = bgra[i + 1];   // G
                out[i + 2] = bgra[i + 0];   // B
                out[i + 3] = 0xFF;          // A (BitBlt ignores alpha)
            }
            out_w = w;
            out_h = h;
        }
    }

    SelectObject(hdc_mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(hdc_mem);
    ReleaseDC(hwnd, hdc_win);
    return out;
}

}  // namespace


void Init() {
    if (g_active) return;
    const char* dir = std::getenv("FM2K_CAPTURE_DIR");
    const char* on  = std::getenv("FM2K_AUTO_CAPTURE");
    const bool enabled = on && on[0] != '\0' && on[0] != '0';
    if (!enabled || !dir) return;
    g_capture_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(g_capture_dir, ec);
    if (ec) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: create_directories('%s') failed: %s",
            dir, ec.message().c_str());
    }
    g_active = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "FM2KCapture: active (capture_dir='%s')", dir);
}


void Shutdown() {
    g_active = false;
}


bool IsActive() { return g_active; }


bool SaveScreenshot(const std::string& filename) {
    if (!g_active) return false;
    HWND hwnd = FindGameWindow();
    if (!hwnd) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: no game window yet, dropping '%s'",
            filename.c_str());
        return false;
    }

    int w = 0, h = 0;
    std::vector<uint8_t> rgba = CaptureWindowRGBA(hwnd, w, h);
    if (rgba.empty() || w <= 0 || h <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: BitBlt failed for '%s'", filename.c_str());
        return false;
    }

    std::filesystem::path out_path =
        std::filesystem::path(g_capture_dir) / filename;
    // stb writes via fopen with the narrow C path, so JP install
    // dirs would mangle on the launcher side; the launcher is
    // responsible for picking an ASCII capture-dir for us.
    int ok = stbi_write_png(out_path.string().c_str(),
                            w, h, 4, rgba.data(), w * 4);
    if (ok) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: wrote %dx%d → %s",
            w, h, out_path.string().c_str());
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KCapture: stbi_write_png failed for %s",
            out_path.string().c_str());
    }
    return ok != 0;
}

}  // namespace FM2KCapture
