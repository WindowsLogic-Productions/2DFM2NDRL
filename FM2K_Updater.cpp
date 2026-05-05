// FM2K_Updater — auto-updater glue. See FM2K_Updater.h.

#include "FM2K_Updater.h"
#include "version_local.h"

#include <SDL3/SDL_log.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace fm2k::updater {

namespace {

// ---------------------------------------------------------------------------
// State (file-static, accessed from worker thread + UI thread under mutex).
// ---------------------------------------------------------------------------

struct InternalState {
    Snapshot           snap;
    std::string        zip_path;        // %TEMP%\fm2k_v<ver>.zip after download
    std::thread        worker;
    std::atomic<bool>  busy{false};     // a check or download is in flight
    std::atomic<bool>  cancel{false};
    mutable std::mutex mtx;
};

InternalState g_st;

void SetState(State s) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.snap.state = s;
}

void SetFail(const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.snap.state        = State::Failed;
    g_st.snap.error_detail = detail;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Updater: %s", detail.c_str());
}

// ---------------------------------------------------------------------------
// Version compare. Splits on '.' and lex-compares the first 4 ints. Matches
// the way scripts/make_version.sh writes them. Tolerant of trailing text
// ("0.1.0-rc1" → (0,1,0,0)). Returns < 0 if a < b, 0 if equal, > 0 if a > b.
// ---------------------------------------------------------------------------

int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> parts;
        size_t p = 0;
        while (p < s.size() && parts.size() < 4) {
            int v = 0; bool got = false;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                v = v * 10 + (s[p] - '0');
                ++p; got = true;
            }
            if (got) parts.push_back(v);
            if (p < s.size() && s[p] == '.') ++p;
            else break;
        }
        while (parts.size() < 4) parts.push_back(0);
        return parts;
    };
    auto pa = split(a);
    auto pb = split(b);
    for (int i = 0; i < 4; ++i) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

// ---------------------------------------------------------------------------
// WinHTTP wrappers. We do GET (text) and GET (file-stream). Both follow
// redirects automatically — important because GitHub's release-asset
// download URLs 302 to a CDN.
// ---------------------------------------------------------------------------

bool ParseHttpsUrl(const std::string& url,
                   bool& https_out,
                   std::wstring& host_out,
                   uint16_t& port_out,
                   std::wstring& path_out)
{
    https_out = false;
    std::string s = url;
    if (s.compare(0, 8, "https://") == 0) { https_out = true;  s = s.substr(8); }
    else if (s.compare(0, 7, "http://") == 0) {                 s = s.substr(7); }
    else return false;
    size_t slash = s.find('/');
    std::string hp   = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hp.find(':');
    std::string host = hp;
    uint16_t port = https_out ? 443 : 80;
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = (uint16_t)std::atoi(hp.c_str() + colon + 1);
    }
    auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
    host_out = widen(host);
    path_out = widen(path);
    port_out = port;
    return true;
}

struct GetResp {
    int         status = 0;
    std::string body;
};

GetResp HttpGetText(const std::string& url, int timeout_ms = 8000) {
    GetResp out;
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!ParseHttpsUrl(url, https, host, port, path)) return out;

    HINTERNET hSes = WinHttpOpen(L"FM2K_Updater/0.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return out;
    WinHttpSetTimeouts(hSes, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return out; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return out; }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return out;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;

    std::vector<char> buf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        size_t off = buf.size();
        buf.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hReq, buf.data() + off, avail, &got)) break;
        buf.resize(off + got);
    }
    out.body.assign(buf.begin(), buf.end());

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return out;
}

// Streaming download with progress callback. Returns true on success.
// dst_path is created/truncated. Callback signature: (received, total).
template <class Cb>
bool HttpDownloadFile(const std::string& url,
                      const std::string& dst_path,
                      Cb on_progress,
                      int timeout_ms = 30000)
{
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!ParseHttpsUrl(url, https, host, port, path)) return false;

    HINTERNET hSes = WinHttpOpen(L"FM2K_Updater/0.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return false;
    WinHttpSetTimeouts(hSes, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return false; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return false; }

    // Auto-redirect handling — release/download/... 302s to objects.githubusercontent.com.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD content_len = 0;
    DWORD cl_sz = sizeof(content_len);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &content_len, &cl_sz, WINHTTP_NO_HEADER_INDEX);

    FILE* fp = std::fopen(dst_path.c_str(), "wb");
    if (!fp) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return false;
    }

    uint32_t received = 0;
    char     buf[16 * 1024];
    bool     ok_all = true;
    for (;;) {
        if (g_st.cancel.load()) { ok_all = false; break; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { ok_all = false; break; }
        if (avail == 0) break;
        DWORD to_read = (avail > sizeof(buf)) ? sizeof(buf) : avail;
        DWORD got = 0;
        if (!WinHttpReadData(hReq, buf, to_read, &got)) { ok_all = false; break; }
        if (got == 0) break;
        if (std::fwrite(buf, 1, got, fp) != got) { ok_all = false; break; }
        received += got;
        on_progress(received, content_len);
    }
    std::fclose(fp);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return ok_all;
}

// ---------------------------------------------------------------------------
// File-system helpers
// ---------------------------------------------------------------------------

std::string TempDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string s(buf);
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

std::string AppDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string s(buf);
    size_t slash = s.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : s.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Worker entry points
// ---------------------------------------------------------------------------

void CheckWorker() {
    SetState(State::Checking);
    char url[512];
    std::snprintf(url, sizeof(url),
        "https://raw.githubusercontent.com/%s/%s/main/LatestVersion",
        kUpdateRepoOwner, kUpdateRepoName);

    GetResp r = HttpGetText(url);
    if (r.status != 200) {
        SetFail("Couldn't reach update server (HTTP " + std::to_string(r.status) + ")");
        g_st.busy.store(false);
        return;
    }
    // Trim whitespace
    std::string remote = r.body;
    while (!remote.empty() && (remote.back() == '\n' || remote.back() == '\r' ||
                               remote.back() == ' '  || remote.back() == '\t')) {
        remote.pop_back();
    }
    while (!remote.empty() && (remote.front() == ' ' || remote.front() == '\t')) {
        remote.erase(remote.begin());
    }
    if (remote.empty()) {
        SetFail("Update server returned empty version");
        g_st.busy.store(false);
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Updater: local=%s remote=%s", kAppVersion, remote.c_str());

    const int cmp = CompareVersions(kAppVersion, remote);
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.snap.remote_version = remote;
        g_st.snap.state = (cmp < 0) ? State::UpdateAvailable : State::UpToDate;
    }
    g_st.busy.store(false);
}

void DownloadWorker(std::string remote_version) {
    SetState(State::Downloading);

    char url[512];
    std::snprintf(url, sizeof(url),
        "https://github.com/%s/%s/releases/download/v%s/fm2k_v%s.zip",
        kUpdateRepoOwner, kUpdateRepoName,
        remote_version.c_str(), remote_version.c_str());

    const std::string zip_path = TempDir() + "fm2k_update_v" + remote_version + ".zip";

    bool ok = HttpDownloadFile(url, zip_path,
        [](uint32_t got, uint32_t total) {
            std::lock_guard<std::mutex> lk(g_st.mtx);
            g_st.snap.downloaded_bytes = got;
            g_st.snap.total_bytes      = total;
        });
    if (!ok) {
        SetFail("Download failed (zip URL = " + std::string(url) + ")");
        std::error_code ec;
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.zip_path  = zip_path;
        g_st.snap.state = State::Ready;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Updater: downloaded %s", zip_path.c_str());
    g_st.busy.store(false);
}

void JoinWorker() {
    if (g_st.worker.joinable()) g_st.worker.join();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CheckForUpdates() {
    if (g_st.busy.exchange(true)) return;
    JoinWorker();
    g_st.worker = std::thread(CheckWorker);
}

void StartDownload() {
    Snapshot s = Get();
    if (s.state != State::UpdateAvailable) return;
    if (g_st.busy.exchange(true)) return;
    JoinWorker();
    g_st.worker = std::thread(DownloadWorker, s.remote_version);
}

bool ApplyUpdateAndExit() {
    std::string zip_path;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.snap.state != State::Ready) return false;
        zip_path = g_st.zip_path;
    }
    if (zip_path.empty()) return false;

    const std::string app_dir       = AppDir();
    const std::string app_updater   = app_dir + "\\FM2KUpdater.exe";
    const DWORD       parent_pid    = GetCurrentProcessId();

    // CCCaster-style: copy the updater EXE to %TEMP% and run it from
    // there. We can't run it in-place because the zip contains a NEW
    // FM2KUpdater.exe and a running EXE can't be overwritten on Windows
    // (ERROR_SHARING_VIOLATION → tar exits with status 1). The tmp
    // copy is fine to overwrite when the new launcher pulls a fresh
    // FM2KUpdater.exe out of the zip into app_dir.
    const std::string tmp_updater = TempDir() + "FM2KUpdater_run.exe";
    const BOOL copy_ok = CopyFileA(app_updater.c_str(), tmp_updater.c_str(),
                                   FALSE /* overwrite ok */);
    const std::string updater = copy_ok ? tmp_updater : app_updater;
    if (!copy_ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Updater: couldn't copy FM2KUpdater.exe to %TEMP% (err=%lu) — "
            "running from app dir; will fail to self-replace",
            (unsigned long)GetLastError());
    }

    // Argv: <updater_path> <parent_pid> <app_dir> <zip_path>
    // Quote app_dir + zip_path because they often contain spaces.
    char cmdline[2048];
    std::snprintf(cmdline, sizeof(cmdline),
        "\"%s\" %lu \"%s\" \"%s\"",
        updater.c_str(),
        (unsigned long)parent_pid,
        app_dir.c_str(),
        zip_path.c_str());

    STARTUPINFOA        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessA(updater.c_str(), cmdline,
                             nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr,
                             &si, &pi);
    if (!ok) {
        SetFail("Couldn't spawn FM2KUpdater.exe (err=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Updater: handed off to FM2KUpdater.exe — exiting launcher");

    // Give the updater a beat to come up before we tear down so the
    // first thing it does (waiting on our PID) sees a real handle.
    Sleep(200);
    std::exit(0);
}

Snapshot Get() {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    return g_st.snap;
}

void Shutdown() {
    g_st.cancel.store(true);
    JoinWorker();
}

}  // namespace fm2k::updater
