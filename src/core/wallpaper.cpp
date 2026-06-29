#include "core/wallpaper.h"
#include "core/config.h"
#include "core/monitors.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mfidl.h>
#include <evr.h>
#include <propvarutil.h>
#include <timeapi.h>
#include <dwmapi.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "evr.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")

namespace motion {
namespace {

constexpr wchar_t kStopEventName[] = L"Local\\MotionCLI_StopEvent";
constexpr wchar_t kWindowClass[]   = L"MotionCLIWallpaperPane";
constexpr wchar_t kTrayClass[]     = L"MotionCLITray";
constexpr UINT WM_TRAY_CALLBACK    = WM_APP + 1;
constexpr UINT WM_PLAYER_EVENT     = WM_APP + 2;
constexpr UINT TRAY_ICON_ID        = 1;
constexpr UINT OCCLUSION_TIMER     = 1;
constexpr UINT GRACE_TIMER         = 2;
constexpr UINT HOST_TIMER          = 3;
enum { IDM_MUTE = 1001, IDM_OPEN = 1002, IDM_STOP = 1003 };

static std::wofstream g_log;

void logLine(const wchar_t* msg) {
    if (!g_log.is_open()) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    g_log << L"[" << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"] "
          << msg << L"\n";
    g_log.flush();
}

void logf(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    logLine(buf);
}

void openLog() {
    std::wstring dir = Config::dataDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_log.open(dir + L"\\engine.log", std::ios::out | std::ios::trunc);
    logLine(L"=== MotionCLI engine started ===");
}

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

HICON appIcon() {
    return LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
}

bool isDesktopClass(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, _countof(cls));
    return lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0;
}

bool processAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

static HWND g_progman       = nullptr;
static HWND g_workerW       = nullptr;
static HWND g_defviewHost   = nullptr;
static HWND g_defview       = nullptr;
static HWND g_listview      = nullptr;
static bool g_raisedDesktop = false;

BOOL CALLBACK enumWorkerWCb(HWND hwnd, LPARAM) {
    HWND dv = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!dv) return TRUE;

    g_defviewHost = hwnd;
    g_defview     = dv;
    g_listview    = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
    g_workerW     = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);

    logf(L"enumWorkerW: iconHost=0x%p defview=0x%p listview=0x%p wallpaperWorkerW=0x%p",
         hwnd, dv, g_listview, g_workerW);
    return FALSE;
}

void makeIconsTransparent() {
    if (!g_listview || !IsWindow(g_listview)) return;
    SendMessageW(g_listview, 0x1001, 0, (LPARAM)-1);
    SendMessageW(g_listview, 0x1026, 0, (LPARAM)-1);
    InvalidateRect(g_listview, nullptr, TRUE);
    logf(L"Icon background cleared: listview=0x%p", g_listview);
}

HWND findWallpaperHost() {
    g_progman = g_workerW = g_defviewHost = g_defview = g_listview = nullptr;
    g_raisedDesktop = false;

    g_progman = FindWindowW(L"Progman", nullptr);
    if (!g_progman) { logLine(L"ERROR: Progman not found"); return nullptr; }

    g_raisedDesktop = !!(GetWindowLongPtrW(g_progman, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP);
    logf(L"Progman=0x%p  raisedDesktop=%d", g_progman, (int)g_raisedDesktop);

    DWORD_PTR res = 0;
    SendMessageTimeoutW(g_progman, 0x052C, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &res);

    for (int i = 0; i < 20; ++i) {
        g_workerW = nullptr;
        EnumWindows(enumWorkerWCb, 0);
        if (g_workerW && IsWindow(g_workerW)) {
            makeIconsTransparent();
            logf(L"Host: WorkerW=0x%p", g_workerW);
            return g_workerW;
        }
        Sleep(50);
    }

    logLine(L"No empty WorkerW found — using Progman as host.");
    makeIconsTransparent();
    return g_progman;
}


struct PaneRT;

class PlayerCB final : public IMFPMediaPlayerCallback {
public:
    explicit PlayerCB(PaneRT* p) : m_refs(1), m_pane(p) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --m_refs; if (!r) delete this; return r;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* hdr) override;

private:
    std::atomic<ULONG> m_refs;
    PaneRT* m_pane;
};

struct PaneRT {
    HWND             hwnd         = nullptr;
    IMFPMediaPlayer* player       = nullptr;
    PlayerCB*        cb           = nullptr;
    RECT             absRect      = {};
    bool             isSpan       = false;
    bool             paused       = false;
    bool             muted        = false;
    bool             fullyStopped = false;
    float            rate         = 1.0f;
    DWORD            pauseTick    = 0;
    std::wstring     media;
};

struct EngineState {
    std::vector<PaneRT> panes;
    HWND  host               = nullptr;
    bool  muted              = false;
    bool  pauseOnFullscreen  = true;
    bool  pauseWhenMaximized = true;
    bool  pauseUnlessDesktop = false;
    bool  pauseOnBattery     = false;
    bool  lowEndMode         = false;
    float playbackSpeed      = 1.0f;
    bool  occlusionActive    = false;
    int   occlusionTimeoutSec= 0;
    int   occlusionPollMs    = 150;
    int   occlusionGraceMs   = 0;
};

void applySettings(PaneRT& p) {
    if (!p.player) return;
    p.player->SetMute(p.muted ? TRUE : FALSE);
    if (p.rate > 0.f && p.rate != 1.f) p.player->SetRate(p.rate);
}

void stopPlayer(PaneRT& p) {
    if (!p.player) return;
    p.player->Stop();
    p.player->Shutdown();
    p.player->Release();
    p.player = nullptr;
    if (p.cb) { p.cb->Release(); p.cb = nullptr; }
    p.fullyStopped = true;
}

bool startPlayer(PaneRT& p) {
    if (p.player || !p.hwnd) return true;
    p.cb = new (std::nothrow) PlayerCB(&p);
    if (!p.cb) return false;

    HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_FREE_THREADED_CALLBACK,
                                      p.cb, p.hwnd, &p.player);
    if (SUCCEEDED(hr)) hr = p.player->CreateMediaItemFromURL(p.media.c_str(), FALSE, 0, nullptr);
    if (FAILED(hr)) {
        if (p.player) { p.player->Shutdown(); p.player->Release(); p.player = nullptr; }
        if (p.cb)     { p.cb->Release(); p.cb = nullptr; }
        return false;
    }
    p.fullyStopped = false;
    return true;
}

void pausePlayer(PaneRT& p) {
    if (p.player && !p.paused) { p.player->Pause(); p.paused = true; }
}

void resumePlayer(PaneRT& p) {
    if (p.player && p.paused) { p.player->Play(); p.paused = false; }
}

void restartPlayback(PaneRT& p) {
    if (!p.player) return;
    PROPVARIANT t; PropVariantInit(&t);
    t.vt = VT_I8; t.hVal.QuadPart = 0;
    p.player->SetPosition(MFP_POSITIONTYPE_100NS, &t);
    PropVariantClear(&t);
    p.player->Play();
}

void PlayerCB::OnMediaPlayerEvent(MFP_EVENT_HEADER* hdr) {
    if (!hdr || !m_pane) return;
    if (FAILED(hdr->hrEvent)) {
        logf(L"MFPlay event failed type=%u hr=0x%08X", hdr->eEventType, (unsigned)hdr->hrEvent);
        return;
    }
    switch (hdr->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
            auto* ev = MFP_GET_MEDIAITEM_CREATED_EVENT(hdr);
            HRESULT hr = m_pane->player->SetMediaItem(ev->pMediaItem);
            logf(L"MediaItemCreated hr=0x%08X", (unsigned)hr);
            break;
        }
        case MFP_EVENT_TYPE_MEDIAITEM_SET: {
            m_pane->player->UpdateVideo();
            m_pane->player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
            applySettings(*m_pane);

            IMFVideoDisplayControl* vdc = nullptr;
            if (SUCCEEDED(MFGetService(m_pane->player, MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&vdc)))) {
                vdc->SetRenderingPrefs(MFVideoRenderPrefs_AllowOutputThrottling);
                vdc->Release();
            }

            if (!m_pane->paused) m_pane->player->Play();
            break;
        }
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            if (m_pane->hwnd) PostMessageW(m_pane->hwnd, WM_PLAYER_EVENT, 1, 0);
            break;
        case MFP_EVENT_TYPE_ERROR:
            logLine(L"MFPlay error event.");
            break;
        default: break;
    }
}

LRESULT CALLBACK wallpaperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<PaneRT*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCHITTEST:     return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND:    return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
        case WM_DISPLAYCHANGE:
            if (p && p->player && !p->fullyStopped) p->player->UpdateVideo();
            return 0;
        case WM_PLAYER_EVENT:
            if (p && wp == 1 && !p->paused) restartPlayback(*p);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool registerWallpaperClass(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wallpaperWndProc;
    wc.hInstance     = inst;
    wc.hIcon         = appIcon();
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool probeForeground(RECT& monRect, bool& fullscreen, bool& maximized) {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg) || isDesktopClass(fg)) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, _countof(cls));
    if (!lstrcmpW(cls, kWindowClass) || !lstrcmpW(cls, kTrayClass)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    monRect   = mi.rcMonitor;
    maximized = IsZoomed(fg) != 0;
    fullscreen = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                 wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    return true;
}

bool onBattery() {
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
}

void updateOcclusion(EngineState* st, HWND trayHwnd) {
    if (!st || !st->occlusionActive) return;

    RECT monRect{};
    bool fullscreen = false, maximized = false;
    bool haveFg  = probeForeground(monRect, fullscreen, maximized);
    bool forceAll = (st->pauseUnlessDesktop && haveFg) || (st->pauseOnBattery && onBattery());

    bool allStopped = true;
    for (PaneRT& pane : st->panes) {
        bool occluded = forceAll || (haveFg && (fullscreen || maximized) &&
                        (pane.isSpan || PtInRect(&monRect, {(pane.absRect.left + pane.absRect.right)/2,
                                                             (pane.absRect.top + pane.absRect.bottom)/2})));

        if (occluded) pausePlayer(pane);
        else resumePlayer(pane);

        if (!pane.paused) allStopped = false;
    }

    if (allStopped) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    if (trayHwnd) {
        UINT ms = (UINT)st->occlusionPollMs;
        KillTimer(trayHwnd, OCCLUSION_TIMER);
        SetTimer(trayHwnd, OCCLUSION_TIMER, ms, nullptr);
    }
}

void showTrayMenu(HWND hwnd, EngineState* st) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Motion CLI - live wallpaper");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (st && st->muted ? MF_CHECKED : 0), IDM_MUTE, L"Mute audio");
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Motion CLI...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_STOP, L"Stop wallpaper");
    SetForegroundWindow(hwnd);
    POINT pt{}; GetCursorPos(&pt);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<EngineState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_TIMER:
            if      (wp == OCCLUSION_TIMER) updateOcclusion(st, hwnd);
            else if (wp == GRACE_TIMER)     { KillTimer(hwnd, GRACE_TIMER); if (st) st->occlusionActive = true; }
            else if (wp == HOST_TIMER && st && st->host && !IsWindow(st->host))
                PostQuitMessage(0);
            return 0;
        case WM_TRAY_CALLBACK:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU ||
                LOWORD(lp) == WM_LBUTTONUP)
                showTrayMenu(hwnd, st);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_MUTE:
                    if (st) {
                        st->muted = !st->muted;
                        for (PaneRT& p : st->panes) { p.muted = st->muted; applySettings(p); }
                    }
                    return 0;
                case IDM_OPEN: {
                    std::wstring cmd = L"\"" + exePath() + L"\"";
                    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
                    buf.push_back(0);
                    STARTUPINFOW si{}; si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                       CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
                        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                    }
                    return 0;
                }
                case IDM_STOP: PostQuitMessage(0); return 0;
                default:       return 0;
            }
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND createTray(HINSTANCE inst, EngineState* st, NOTIFYICONDATAW& nid) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = trayWndProc;
    wc.hInstance = inst; wc.lpszClassName = kTrayClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"MotionCLITray",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    nid = {}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd; nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = appIcon();
    wcscpy_s(nid.szTip, L"Motion CLI - live wallpaper running");
    Shell_NotifyIconW(NIM_ADD, &nid);

    SetTimer(hwnd, OCCLUSION_TIMER, st ? (UINT)st->occlusionPollMs : 150, nullptr);
    SetTimer(hwnd, HOST_TIMER, 5000, nullptr);
    if (st && st->occlusionGraceMs > 0)
        SetTimer(hwnd, GRACE_TIMER, (UINT)st->occlusionGraceMs, nullptr);
    else if (st)
        st->occlusionActive = true;
    return hwnd;
}

struct Pane {
    RECT         absRect = {};
    bool         isSpan  = false;
    std::wstring media;
};

std::vector<Pane> buildPanes(const Config& cfg) {
    std::vector<Pane> out;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (cfg.mode == WallpaperMode::PerMonitor) {
        for (const MonitorInfo& m : enumerateMonitors()) {
            std::wstring media = cfg.currentMediaPath;
            auto it = cfg.monitorAssignments.find(m.device);
            if (it != cfg.monitorAssignments.end()) media = it->second;
            if (media.empty() || !fileExists(media)) continue;
            Pane p;
            p.absRect = { m.x, m.y, m.x + m.width, m.y + m.height };
            p.media   = media;
            out.push_back(std::move(p));
        }
    } else if (!cfg.currentMediaPath.empty() && fileExists(cfg.currentMediaPath)) {
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 0) vw = GetSystemMetrics(SM_CXSCREEN);
        if (vh <= 0) vh = GetSystemMetrics(SM_CYSCREEN);
        Pane p;
        p.absRect = { vx, vy, vx + vw, vy + vh };
        p.isSpan  = true;
        p.media   = cfg.currentMediaPath;
        out.push_back(std::move(p));
    }
    return out;
}

bool startPane(HINSTANCE inst, HWND host, const Pane& def, EngineState& st) {
    int ax = def.absRect.left, ay = def.absRect.top;
    int w  = def.absRect.right - ax, h = def.absRect.bottom - ay;

    HWND hwnd;

    if (host) {
        POINT pt = { ax, ay };
        ScreenToClient(host, &pt);

        hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE,
            kWindowClass, L"MotionCLI Wallpaper",
            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            pt.x, pt.y, w, h,
            host, nullptr, inst, nullptr);
        if (!hwnd) { logf(L"CreateWindowEx(child) failed err=%lu", GetLastError()); return false; }

        HWND zafter = (g_raisedDesktop && g_defview && IsWindow(g_defview)) ? g_defview : HWND_BOTTOM;
        SetWindowPos(hwnd, zafter, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        logf(L"startPane(child): hwnd=0x%p host=0x%p pt=(%d,%d) w=%d h=%d raised=%d",
             hwnd, host, pt.x, pt.y, w, h, (int)g_raisedDesktop);
    } else {
        hwnd = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kWindowClass, L"MotionCLI Wallpaper",
            WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            ax, ay, w, h,
            nullptr, nullptr, inst, nullptr);
        if (!hwnd) { logf(L"CreateWindowEx(popup) failed err=%lu", GetLastError()); return false; }
        SetWindowPos(hwnd, HWND_BOTTOM, ax, ay, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        logf(L"startPane(popup/nohost): hwnd=0x%p abs=(%d,%d) w=%d h=%d", hwnd, ax, ay, w, h);
    }

    st.panes.emplace_back();
    PaneRT& rt = st.panes.back();
    rt.hwnd        = hwnd;
    rt.absRect     = def.absRect;
    rt.isSpan      = def.isSpan;
    rt.muted       = st.muted;
    rt.rate        = st.lowEndMode ? 0.5f : st.playbackSpeed;
    if (rt.rate < 0.25f || rt.rate > 4.0f) rt.rate = 1.0f;
    rt.media       = def.media;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&rt));

    rt.cb = new (std::nothrow) PlayerCB(&rt);
    if (!rt.cb) { logLine(L"PlayerCB alloc failed"); DestroyWindow(hwnd); st.panes.pop_back(); return false; }

    HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_FREE_THREADED_CALLBACK, rt.cb, hwnd, &rt.player);
    if (SUCCEEDED(hr)) hr = rt.player->CreateMediaItemFromURL(def.media.c_str(), FALSE, 0, nullptr);
    logf(L"MFPCreateMediaPlayer hr=0x%08X media=%s", (unsigned)hr, def.media.c_str());

    if (FAILED(hr)) {
        if (rt.player) { rt.player->Shutdown(); rt.player->Release(); rt.player = nullptr; }
        if (rt.cb)     { rt.cb->Release(); rt.cb = nullptr; }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        st.panes.pop_back();
        return false;
    }
    return true;
}

void shutdownPane(PaneRT& p) {
    stopPlayer(p);
    if (p.hwnd && IsWindow(p.hwnd)) {
        SetWindowLongPtrW(p.hwnd, GWLP_USERDATA, 0);
        DestroyWindow(p.hwnd);
    }
    p.hwnd = nullptr;
}

static bool launchAsShellUser(const std::wstring& cmd, DWORD& outPid, std::string& err) {
    DWORD shellPid = 0;
    if (HWND sw = GetShellWindow()) GetWindowThreadProcessId(sw, &shellPid);

    if (!shellPid) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe))
                do { if (!_wcsicmp(pe.szExeFile, L"explorer.exe")) { shellPid = pe.th32ProcessID; break; } }
                while (Process32NextW(snap, &pe));
            CloseHandle(snap);
        }
    }

    if (shellPid) {
        HANDLE ep = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shellPid);
        if (ep) {
            HANDLE st = nullptr;
            if (OpenProcessToken(ep, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &st)) {
                HANDLE dup = nullptr;
                if (DuplicateTokenEx(st, MAXIMUM_ALLOWED, nullptr,
                                     SecurityImpersonation, TokenPrimary, &dup)) {
                    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
                    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
                    BOOL ok = CreateProcessAsUserW(dup, nullptr, buf.data(), nullptr, nullptr,
                                                   FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
                    CloseHandle(dup); CloseHandle(st); CloseHandle(ep);
                    if (ok) { outPid = pi.dwProcessId; CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true; }
                    goto fallback;
                }
                CloseHandle(st);
            }
            CloseHandle(ep);
        }
    }

fallback:
    {
        STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            outPid = pi.dwProcessId; CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
        }
        char b[96]; _snprintf_s(b, sizeof(b), _TRUNCATE, "CreateProcess failed (%lu)", GetLastError());
        err = b; return false;
    }
}

} // namespace

int runEngineFromConfig() {
    openLog();

    Config cfg = Config::load();
    auto panes  = buildPanes(cfg);
    logf(L"Panes requested: %d", (int)panes.size());
    if (panes.empty()) { logLine(L"No playable panes — exiting."); return 0; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { logf(L"CoInitializeEx hr=0x%08X", (unsigned)hr); return 1; }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { logf(L"MFStartup hr=0x%08X", (unsigned)hr); CoUninitialize(); return 1; }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    timeBeginPeriod(1);
    DwmEnableMMCSS(TRUE);

    HINSTANCE inst   = GetModuleHandleW(nullptr);
    HANDLE    stopEv = CreateEventW(nullptr, FALSE, FALSE, kStopEventName);

    EngineState state;
    state.muted              = cfg.muteByDefault;
    state.pauseOnFullscreen  = cfg.pauseOnFullscreen;
    state.pauseWhenMaximized = cfg.pauseWhenMaximized;
    state.pauseUnlessDesktop = cfg.pauseUnlessDesktop;
    state.pauseOnBattery     = cfg.pauseOnBattery;
    state.lowEndMode         = cfg.lowEndMode;
    state.playbackSpeed      = (float)cfg.playbackSpeed;
    if (state.playbackSpeed < 0.25f || state.playbackSpeed > 4.0f) state.playbackSpeed = 1.0f;
    state.occlusionTimeoutSec = cfg.occlusionTimeoutSec;
    state.occlusionPollMs     = cfg.occlusionPollMs;
    state.occlusionGraceMs    = cfg.occlusionGraceMs;
    state.host = findWallpaperHost();

    logf(L"host=0x%p raisedDesktop=%d workerW=0x%p defview=0x%p",
         state.host, (int)g_raisedDesktop, g_workerW, g_defview);

    if (!registerWallpaperClass(inst)) {
        logf(L"RegisterClassEx failed err=%lu", GetLastError());
        if (stopEv) CloseHandle(stopEv);
        MFShutdown(); CoUninitialize(); return 1;
    }

    state.panes.reserve(panes.size());
    for (const Pane& p : panes) startPane(inst, state.host, p, state);

    logf(L"Active panes: %d", (int)state.panes.size());
    if (state.panes.empty()) {
        if (stopEv) CloseHandle(stopEv);
        MFShutdown(); CoUninitialize(); return 1;
    }

    NOTIFYICONDATAW nid{};
    HWND tray = createTray(inst, &state, nid);

    for (bool quit = false; !quit; ) {
        HANDLE waits[] = { stopEv };
        DWORD  wr = MsgWaitForMultipleObjects(stopEv ? 1 : 0, stopEv ? waits : nullptr,
                                              FALSE, INFINITE, QS_ALLINPUT);
        if (stopEv && wr == WAIT_OBJECT_0) break;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (tray) {
        KillTimer(tray, OCCLUSION_TIMER);
        KillTimer(tray, GRACE_TIMER);
        KillTimer(tray, HOST_TIMER);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (IsWindow(tray)) DestroyWindow(tray);
    }

    for (PaneRT& p : state.panes) shutdownPane(p);
    if (stopEv) CloseHandle(stopEv);
    DwmEnableMMCSS(FALSE);
    timeEndPeriod(1);
    MFShutdown();
    CoUninitialize();
    logLine(L"=== MotionCLI engine stopped ===");
    g_log.flush();
    return 0;
}

bool EngineController::restart(std::string& err) {
    stop();
    if (HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, kStopEventName)) { ResetEvent(ev); CloseHandle(ev); }
    std::wstring cmd = L"\"" + exePath() + L"\" --render";
    DWORD pid = 0;
    if (!launchAsShellUser(cmd, pid, err)) return false;
    m_config.enginePid = pid;
    m_config.save();
    return true;
}

void EngineController::stop() {
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName)) { SetEvent(ev); CloseHandle(ev); }
    if (DWORD pid = m_config.enginePid) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (h) { if (WaitForSingleObject(h, 3000) != WAIT_OBJECT_0) TerminateProcess(h, 0); CloseHandle(h); }
    }
    m_config.enginePid = 0;
    m_config.save();
}

bool EngineController::isRunning() const { return processAlive(m_config.enginePid); }

} // namespace motion
