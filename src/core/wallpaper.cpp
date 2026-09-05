#include "core/wallpaper.h"
#include "core/config.h"
#include "core/monitors.h"
#include "util/str.h"
#include "util/log.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfmediaengine.h>

#include <timeapi.h>
#include <vector>
#include <set>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cwchar>

namespace motion {

namespace {

const wchar_t* kWindowClass = L"MotionCLI_PaneClass";
const wchar_t* kStopEventName = L"Local\\MotionCLI_StopEvent";
const wchar_t* kReloadEventName = L"Local\\MotionCLI_ReloadEvent";
const wchar_t* kEngineMutexName = L"Local\\MotionCLI_EngineMutex";

HWND g_progman = nullptr;
HWND g_workerW = nullptr;
HWND g_listview = nullptr;
HWND g_defView = nullptr;

Config g_currentCfg;
std::atomic<bool> g_manualPaused{false};

std::string toHex(uintptr_t val) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%08llX", (unsigned long long)val);
    return buf;
}

struct PaneRT;
struct EngineState {
    std::vector<PaneRT*> panes;
    bool muted = true;
    bool lowEndMode = false;
    float playbackSpeed = 1.0f;
    int targetFps = 30;
    ~EngineState();
};

EngineState* g_currentEngineState = nullptr;

void makeIconsTransparent() {
    if (g_listview) {
        SendMessageW(g_listview, 0x1001, 0, (LPARAM)-1);
        SendMessageW(g_listview, 0x1026, 0, (LPARAM)-1);
        InvalidateRect(g_listview, nullptr, TRUE);
    }
}

DWORD getWindowsBuildNumber() {
    typedef LONG(NTAPI* pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (hNt) {
        pfnRtlGetVersion rtlGetVersion = (pfnRtlGetVersion)GetProcAddress(hNt, "RtlGetVersion");
        if (rtlGetVersion) {
            RTL_OSVERSIONINFOW rovi = { sizeof(rovi) };
            if (rtlGetVersion(&rovi) == 0) {
                return rovi.dwBuildNumber;
            }
        }
    }
    return 0;
}

HWND findWallpaperHost() {
    log::info("locatng desktp wallaper host...");
    g_progman = FindWindowW(L"Progman", nullptr);
    if (!g_progman) {
        g_progman = GetShellWindow();
    }
    if (!g_progman) {
        log::error("progman wnd not foudn");
        return nullptr;
    }
    log::info("found progamn: 0x" + toHex(reinterpret_cast<uintptr_t>(g_progman)));

    DWORD buildNum = getWindowsBuildNumber();
    log::info("detected os build number: " + std::to_string(buildNum));

    if (buildNum >= 22000) {
        HWND defViewInProgman = FindWindowExW(g_progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defViewInProgman) {
            g_defView = defViewInProgman;
            g_listview = FindWindowExW(defViewInProgman, nullptr, L"SysListView32", nullptr);
            g_workerW = nullptr;

            LONG_PTR style = GetWindowLongPtrW(g_progman, GWL_STYLE);
            if (!(style & WS_CLIPCHILDREN)) {
                SetWindowLongPtrW(g_progman, GWL_STYLE, style | WS_CLIPCHILDREN);
            }
            makeIconsTransparent();
            log::info("attached to Windows 11 progman host: 0x" + toHex(reinterpret_cast<uintptr_t>(g_progman)));
            return g_progman;
        }
    }

    DWORD_PTR dummy = 0;
    SendMessageTimeoutW(g_progman, 0x052C, 0x0000000D, 0, SMTO_NORMAL, 1000, &dummy);
    SendMessageTimeoutW(g_progman, 0x052C, 0x0000000D, 1, SMTO_NORMAL, 1000, &dummy);
    SendMessageTimeoutW(g_progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &dummy);

    for (int retry = 0; retry < 30; ++retry) {
        HWND shellContainer = nullptr;
        EnumWindows([](HWND top, LPARAM lp) -> BOOL {
            HWND shell = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
            if (shell) {
                *reinterpret_cast<HWND*>(lp) = top;
                g_defView = shell;
                g_listview = FindWindowExW(shell, nullptr, L"SysListView32", nullptr);
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&shellContainer));

        if (shellContainer) {
            HWND worker = FindWindowExW(nullptr, shellContainer, L"WorkerW", nullptr);
            if (worker && IsWindow(worker)) {
                g_workerW = worker;
                LONG_PTR style = GetWindowLongPtrW(g_workerW, GWL_STYLE);
                if (!(style & WS_CLIPCHILDREN)) {
                    SetWindowLongPtrW(g_workerW, GWL_STYLE, style | WS_CLIPCHILDREN);
                }
                makeIconsTransparent();
                ShowWindow(g_workerW, SW_SHOW);
                SetWindowPos(g_workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                if (g_listview) {
                    UpdateWindow(g_listview);
                    InvalidateRect(g_listview, nullptr, TRUE);
                }
                log::info("attached to toplevel workerw host: 0x" + toHex(reinterpret_cast<uintptr_t>(g_workerW)));
                return g_workerW;
            }

            HWND anyWorker = nullptr;
            EnumWindows([](HWND top, LPARAM lp) -> BOOL {
                char cls[64] = {0};
                if (GetClassNameA(top, cls, sizeof(cls)) && strcmp(cls, "WorkerW") == 0) {
                    if (!FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
                        *reinterpret_cast<HWND*>(lp) = top;
                        return FALSE;
                    }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&anyWorker));

            if (anyWorker && IsWindow(anyWorker)) {
                g_workerW = anyWorker;
                LONG_PTR style = GetWindowLongPtrW(g_workerW, GWL_STYLE);
                if (!(style & WS_CLIPCHILDREN)) {
                    SetWindowLongPtrW(g_workerW, GWL_STYLE, style | WS_CLIPCHILDREN);
                }
                makeIconsTransparent();
                ShowWindow(g_workerW, SW_SHOW);
                SetWindowPos(g_workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                if (g_listview) {
                    UpdateWindow(g_listview);
                    InvalidateRect(g_listview, nullptr, TRUE);
                }
                log::info("attached to detached workerw host: 0x" + toHex(reinterpret_cast<uintptr_t>(g_workerW)));
                return g_workerW;
            }
        }
        Sleep(50);
    }

    HWND defViewInProgman = FindWindowExW(g_progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defViewInProgman) {
        g_defView = defViewInProgman;
        g_listview = FindWindowExW(defViewInProgman, nullptr, L"SysListView32", nullptr);
    }
    g_workerW = nullptr;

    LONG_PTR style = GetWindowLongPtrW(g_progman, GWL_STYLE);
    if (!(style & WS_CLIPCHILDREN)) {
        SetWindowLongPtrW(g_progman, GWL_STYLE, style | WS_CLIPCHILDREN);
    }
    makeIconsTransparent();
    log::info("attached to progman host (24h2/25h2 or fallback): 0x" + toHex(reinterpret_cast<uintptr_t>(g_progman)));
    return g_progman;
}

struct PaneDef {
    RECT absRect;
    std::wstring media;
    bool isSpan;
};

std::vector<PaneDef> buildPanes(const Config& cfg) {
    std::vector<PaneDef> panes;
    if (cfg.mode == WallpaperMode::Span) {
        if (!cfg.currentMediaPath.empty()) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (vw <= 0) vw = GetSystemMetrics(SM_CXSCREEN);
            if (vh <= 0) vh = GetSystemMetrics(SM_CYSCREEN);
            panes.push_back(PaneDef{ { vx, vy, vx + vw, vy + vh }, cfg.currentMediaPath, true });
        }
    } else {
        auto monitors = enumerateMonitors();
        for (const auto& kv : cfg.monitorAssignments) {
            for (const auto& m : monitors) {
                if (m.device == kv.first && !kv.second.empty()) {
                    panes.push_back(PaneDef{ { m.x, m.y, m.x + m.width, m.y + m.height }, kv.second, false });
                    break;
                }
            }
        }
    }
    return panes;
}

ID3D11Device* g_d3dDevice = nullptr;
ID3D11DeviceContext* g_d3dContext = nullptr;
IMFDXGIDeviceManager* g_dxgiManager = nullptr;
UINT g_resetToken = 0;

bool initD3D11() {
    log::info("initilizing d3d11 devce and context...");
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3
    };
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels, 5, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
    if (FAILED(hr)) {
        creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels, 5, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
    }
    if (FAILED(hr)) {
        log::warn("d3d11 hw devce faild (hr = 0x" + toHex(hr) + "), retry warp softwre");
        creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, creationFlags, featureLevels, 5, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
        if (FAILED(hr)) {
            log::error("d3d11 devce cmpletely faild (hr = 0x" + toHex(hr) + ")");
            return false;
        }
        log::info("d3d11 using warp softwre rasterizr");
    } else {
        log::info("d3d11 hw devce created, fl: 0x" + toHex((unsigned int)fl));
    }

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_d3dContext->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    IDXGIDevice1* dxgiDev1 = nullptr;
    if (SUCCEEDED(g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev1)) && dxgiDev1) {
        dxgiDev1->SetMaximumFrameLatency(1);
        dxgiDev1->Release();
    }

    hr = MFCreateDXGIDeviceManager(&g_resetToken, &g_dxgiManager);
    if (FAILED(hr)) {
        log::error("mfcreatedxgidevicemanager faild (hr = 0x" + toHex(hr) + ")");
        return false;
    }

    hr = g_dxgiManager->ResetDevice(g_d3dDevice, g_resetToken);
    if (FAILED(hr)) {
        log::error("dxgimanager resetdevce faild (hr = 0x" + toHex(hr) + ")");
        return false;
    }

    log::info("dxgi devce manager registred ok");
    return true;
}

void cleanupD3D11() {
    if (g_dxgiManager) { g_dxgiManager->Release(); g_dxgiManager = nullptr; }
    if (g_d3dContext) { g_d3dContext->Release(); g_d3dContext = nullptr; }
    if (g_d3dDevice) { g_d3dDevice->Release(); g_d3dDevice = nullptr; }
}

void trimWorkingSet() {
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

void trimMemory() {
    if (g_d3dDevice) {
        IDXGIDevice3* dxgi3 = nullptr;
        if (SUCCEEDED(g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice3), (void**)&dxgi3)) && dxgi3) {
            dxgi3->Trim();
            dxgi3->Release();
        }
    }
    trimWorkingSet();
}

struct PaneRT {
    HWND hwnd = nullptr;
    RECT absRect = {};
    bool isSpan = false;
    bool paused = false;
    bool muted = false;
    std::wstring media;

    IDXGISwapChain1* swapChain = nullptr;
    IMFMediaEngine* engine = nullptr;
    
    std::atomic<bool> threadRunning{false};
    std::atomic<int> targetFps{30};
    std::atomic<bool> lowEndMode{false};
    std::thread renderThread;

    ~PaneRT() {
        threadRunning = false;
        if (renderThread.joinable()) renderThread.join();
        if (engine) { engine->Shutdown(); engine->Release(); engine = nullptr; }
        if (swapChain) { swapChain->Release(); swapChain = nullptr; }
        if (hwnd) DestroyWindow(hwnd);
    }
};

EngineState::~EngineState() {
    for (auto p : panes) delete p;
    panes.clear();
}

class EngineNotify : public IMFMediaEngineNotify {
    long m_cRef = 1;
    PaneRT* m_pane;
public:
    EngineNotify(PaneRT* p) : m_pane(p) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_cRef);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY ||
            event == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH ||
            event == MF_MEDIA_ENGINE_EVENT_LOADEDDATA ||
            event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA) {
            if (m_pane && m_pane->engine && !m_pane->paused && !g_manualPaused) {
                m_pane->engine->Play();
            }
        } else if (event == MF_MEDIA_ENGINE_EVENT_PLAYING) {
            log::info("MediaEngine event: PLAYING");
        } else if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            log::error("MediaEngine playback error code: " + std::to_string(param1) + ", HRESULT: 0x" + toHex((uintptr_t)param2));
        } else if (event == MF_MEDIA_ENGINE_EVENT_ENDED) {
            if (m_pane && m_pane->engine) {
                m_pane->engine->SetCurrentTime(0.0);
                if (!m_pane->paused && !g_manualPaused) m_pane->engine->Play();
            }
        }
        return S_OK;
    }
};

static LRESULT CALLBACK wallpaperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    PaneRT* p = reinterpret_cast<PaneRT*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_USER + 1: {
            UINT event = LOWORD(lp);
            if (event == WM_LBUTTONDBLCLK) {
                wchar_t exePath[MAX_PATH] = {0};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (event == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED, 100, L"Motion CLI · Live Wallpaper");
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, 101, L"Open Motion CLI");
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (g_manualPaused ? MF_CHECKED : MF_UNCHECKED), 102, g_manualPaused ? L"Resume Wallpaper" : L"Pause Wallpaper");
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (g_currentCfg.muteByDefault ? MF_CHECKED : MF_UNCHECKED), 103, L"Mute Audio");

                HMENU hSubAutoPause = CreatePopupMenu();
                bool isFsOnly = g_currentCfg.pauseOnFullscreen && !g_currentCfg.pauseWhenMaximized && !g_currentCfg.pauseUnlessDesktop;
                bool isMaxAndFs = g_currentCfg.pauseOnFullscreen && g_currentCfg.pauseWhenMaximized && !g_currentCfg.pauseUnlessDesktop;
                bool isAnyFocus = g_currentCfg.pauseUnlessDesktop;
                bool isNever = !g_currentCfg.pauseOnFullscreen && !g_currentCfg.pauseWhenMaximized && !g_currentCfg.pauseUnlessDesktop;

                InsertMenuW(hSubAutoPause, -1, MF_BYPOSITION | MF_STRING | (isMaxAndFs ? MF_CHECKED : MF_UNCHECKED), 201, L"Maximized + Fullscreen (Default)");
                InsertMenuW(hSubAutoPause, -1, MF_BYPOSITION | MF_STRING | (isFsOnly ? MF_CHECKED : MF_UNCHECKED), 202, L"Fullscreen Only (Games/Videos)");
                InsertMenuW(hSubAutoPause, -1, MF_BYPOSITION | MF_STRING | (isAnyFocus ? MF_CHECKED : MF_UNCHECKED), 203, L"When any App is Focused");
                InsertMenuW(hSubAutoPause, -1, MF_BYPOSITION | MF_STRING | (isNever ? MF_CHECKED : MF_UNCHECKED), 204, L"Never Pause (Always Play)");

                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hSubAutoPause, L"Auto-Pause Behavior");
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, 105, L"Exit Motion CLI");

                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hSubAutoPause);
                DestroyMenu(hMenu);

                if (cmd == 101) {
                    wchar_t exePath[MAX_PATH] = {0};
                    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                    ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
                } else if (cmd == 102) {
                    g_manualPaused = !g_manualPaused;
                    if (g_currentEngineState) {
                        for (auto pane : g_currentEngineState->panes) {
                            if (pane->engine) {
                                if (g_manualPaused) pane->engine->Pause();
                                else pane->engine->Play();
                            }
                        }
                    }
                } else if (cmd == 103) {
                    g_currentCfg.muteByDefault = !g_currentCfg.muteByDefault;
                    g_currentCfg.save();
                    if (g_currentEngineState) {
                        for (auto pane : g_currentEngineState->panes) {
                            pane->muted = g_currentCfg.muteByDefault;
                            if (pane->engine) pane->engine->SetMuted(pane->muted);
                        }
                    }
                } else if (cmd == 201) {
                    g_currentCfg.pauseOnFullscreen = true;
                    g_currentCfg.pauseWhenMaximized = true;
                    g_currentCfg.pauseUnlessDesktop = false;
                    g_currentCfg.save();
                } else if (cmd == 202) {
                    g_currentCfg.pauseOnFullscreen = true;
                    g_currentCfg.pauseWhenMaximized = false;
                    g_currentCfg.pauseUnlessDesktop = false;
                    g_currentCfg.save();
                } else if (cmd == 203) {
                    g_currentCfg.pauseOnFullscreen = true;
                    g_currentCfg.pauseWhenMaximized = true;
                    g_currentCfg.pauseUnlessDesktop = true;
                    g_currentCfg.save();
                } else if (cmd == 204) {
                    g_currentCfg.pauseOnFullscreen = false;
                    g_currentCfg.pauseWhenMaximized = false;
                    g_currentCfg.pauseUnlessDesktop = false;
                    g_currentCfg.save();
                } else if (cmd == 105) {
                    PostQuitMessage(0);
                }
            }
            return 0;
        }
        case WM_SIZE:
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            if (p) {
                if (p->isSpan) {
                    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                    if (vw <= 0) vw = GetSystemMetrics(SM_CXSCREEN);
                    if (vh <= 0) vh = GetSystemMetrics(SM_CYSCREEN);
                    p->absRect = { vx, vy, vx + vw, vy + vh };
                } else {
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(mon, &mi)) p->absRect = mi.rcMonitor;
                }
                int ax = p->absRect.left, ay = p->absRect.top;
                int w  = p->absRect.right - ax, h = p->absRect.bottom - ay;
                HWND parent = GetParent(hwnd);
                if (parent) {
                    POINT pt = { ax, ay };
                    ScreenToClient(parent, &pt);
                    HWND insertAfter = (parent == g_progman && g_defView) ? g_defView : nullptr;
                    UINT flags = SWP_NOACTIVATE;
                    if (!insertAfter) flags |= SWP_NOZORDER;
                    SetWindowPos(hwnd, insertAfter, pt.x, pt.y, w, h, flags);
                } else {
                    SetWindowPos(hwnd, nullptr, ax, ay, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            break;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_DESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void optimizeVideoAsync(const std::wstring& mediaPath, const std::wstring& optPath, UINT32 inW, UINT32 inH, UINT32 fpsNum, UINT32 fpsDen, int maxW) {
    std::wstring tmpPath = optPath + L".tmp";
    DeleteFileW(tmpPath.c_str());

    IMFAttributes* readerAttr = nullptr;
    MFCreateAttributes(&readerAttr, 2);
    if (readerAttr) {
        readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        readerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(mediaPath.c_str(), readerAttr, &reader);
    if (readerAttr) readerAttr->Release();
    if (FAILED(hr) || !reader) return;

    IMFMediaType* partialType = nullptr;
    MFCreateMediaType(&partialType);
    partialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    partialType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, partialType);
    partialType->Release();
    if (FAILED(hr)) {
        reader->Release();
        return;
    }

    IMFMediaType* decType = nullptr;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &decType);
    if (!decType) {
        reader->Release();
        return;
    }

    UINT32 targetW = (UINT32)maxW;
    if (targetW < 1920 && inW >= 1920) targetW = 1920;
    UINT32 targetH = (UINT32)((DWORD64)targetW * inH / inW);
    targetW = (targetW + 1) & ~1;
    targetH = (targetH + 1) & ~1;

    UINT32 outFpsNum = fpsNum;
    UINT32 outFpsDen = fpsDen;
    if (outFpsNum > 30 && outFpsDen == 1) {
        outFpsNum = 30;
    }

    IMFAttributes* writerAttr = nullptr;
    MFCreateAttributes(&writerAttr, 1);
    if (writerAttr) writerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    IMFSinkWriter* writer = nullptr;
    hr = MFCreateSinkWriterFromURL(tmpPath.c_str(), nullptr, writerAttr, &writer);
    if (writerAttr) writerAttr->Release();
    if (FAILED(hr) || !writer) {
        decType->Release();
        reader->Release();
        return;
    }

    IMFMediaType* encType = nullptr;
    MFCreateMediaType(&encType);
    encType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    encType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    encType->SetUINT32(MF_MT_AVG_BITRATE, 8500000);
    encType->SetUINT32(MF_MT_MPEG2_PROFILE, 100);
    MFSetAttributeSize(encType, MF_MT_FRAME_SIZE, targetW, targetH);
    MFSetAttributeRatio(encType, MF_MT_FRAME_RATE, outFpsNum, outFpsDen);
    MFSetAttributeRatio(encType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    encType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    DWORD streamIdx = 0;
    hr = writer->AddStream(encType, &streamIdx);
    encType->Release();
    if (FAILED(hr)) {
        decType->Release();
        writer->Release();
        reader->Release();
        DeleteFileW(tmpPath.c_str());
        return;
    }

    hr = writer->SetInputMediaType(streamIdx, decType, nullptr);
    decType->Release();
    if (FAILED(hr)) {
        writer->Release();
        reader->Release();
        DeleteFileW(tmpPath.c_str());
        return;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        writer->Release();
        reader->Release();
        DeleteFileW(tmpPath.c_str());
        return;
    }

    while (true) {
        DWORD flags = 0;
        LONGLONG pts = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &pts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (sample) sample->Release();
            break;
        }
        if (sample) {
            writer->WriteSample(streamIdx, sample);
            sample->Release();
        }
    }

    writer->Finalize();
    writer->Release();
    reader->Release();

    if (MoveFileExW(tmpPath.c_str(), optPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        HANDLE reloadEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kReloadEventName);
        if (reloadEvent) {
            SetEvent(reloadEvent);
            CloseHandle(reloadEvent);
        }
    } else {
        DeleteFileW(tmpPath.c_str());
    }
}

std::wstring optimizeVideoIfNeeded(const std::wstring& mediaPath, bool lowEndMode, int maxW) {
    if (mediaPath.empty() || !lowEndMode) return mediaPath;

    size_t dot = mediaPath.rfind(L'.');
    if (dot == std::wstring::npos) return mediaPath;

    std::wstring ext = mediaPath.substr(dot);
    if (ext != L".mp4" && ext != L".mov" && ext != L".mkv" && ext != L".webm") return mediaPath;

    if (mediaPath.find(L".opt.") != std::wstring::npos) return mediaPath;

    std::wstring optPath = mediaPath.substr(0, dot) + L".opt.mp4";
    if (GetFileAttributesW(optPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return optPath;
    }

    IMFAttributes* readerAttr = nullptr;
    MFCreateAttributes(&readerAttr, 1);
    if (readerAttr) readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(mediaPath.c_str(), readerAttr, &reader);
    if (readerAttr) readerAttr->Release();
    if (FAILED(hr) || !reader) return mediaPath;

    IMFMediaType* nativeType = nullptr;
    UINT32 inW = 0, inH = 0;
    UINT32 fpsNum = 30, fpsDen = 1;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType)) && nativeType) {
        MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &inW, &inH);
        MFGetAttributeRatio(nativeType, MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        nativeType->Release();
    }
    reader->Release();

    if (inW <= (UINT32)maxW && inH <= 1080) {
        return mediaPath;
    }

    static std::set<std::wstring> s_inProgress;
    if (s_inProgress.count(mediaPath) == 0) {
        s_inProgress.insert(mediaPath);
        std::thread([mediaPath, optPath, inW, inH, fpsNum, fpsDen, maxW]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MFStartup(MF_VERSION);
            optimizeVideoAsync(mediaPath, optPath, inW, inH, fpsNum, fpsDen, maxW);
            MFShutdown();
            CoUninitialize();
        }).detach();
    }

    return mediaPath;
}

bool startPane(HINSTANCE inst, HWND host, const PaneDef& def, EngineState& st) {
    int ax = def.absRect.left, ay = def.absRect.top;
    int w = def.absRect.right - ax, h = def.absRect.bottom - ay;
    if (w <= 0 || h <= 0) return false;

    int sw = w, sh = h;
    if (st.lowEndMode && sw > 1920) {
        sh = (int)((int64_t)sh * 1920 / sw);
        sw = 1920;
    }

    std::wstring playMedia = optimizeVideoIfNeeded(def.media, st.lowEndMode, 1920);

    log::info("startng pane: bounds [" + std::to_string(ax) + ", " +
              std::to_string(ay) + ", " + std::to_string(w) + "x" + std::to_string(h) +
              "], meda: " + narrow(playMedia));

    if (!playMedia.empty()) {
        DWORD attr = GetFileAttributesW(playMedia.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            log::error("meda file not found on dsk: " + narrow(playMedia));
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(playMedia.c_str(), GetFileExInfoStandard, &fad)) {
                ULARGE_INTEGER sz;
                sz.LowPart = fad.nFileSizeLow;
                sz.HighPart = fad.nFileSizeHigh;
                log::info("meda file foudn, sz: " + std::to_string(sz.QuadPart) + " bytes");
            }
        }
    }

    HWND parent = host ? host : nullptr;
    DWORD style = parent ? (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN) : (WS_POPUP | WS_VISIBLE);
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    POINT pt = { ax, ay };
    if (parent) ScreenToClient(parent, &pt);

    HWND hwnd = CreateWindowExW(
        exStyle, kWindowClass, L"MotionCLI Wallpaper",
        style, pt.x, pt.y, w, h, parent, nullptr, inst, nullptr);
    if (!hwnd) {
        log::error("createwindowexw faild, err = " + std::to_string(GetLastError()));
        return false;
    }

    if (parent) {
        HWND insertAfter = (parent == g_progman && g_defView) ? g_defView : HWND_BOTTOM;
        SetWindowPos(hwnd, insertAfter, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        if (g_defView) {
            InvalidateRect(g_defView, nullptr, TRUE);
            UpdateWindow(g_defView);
        }
    }

    PaneRT* rt = new PaneRT();
    rt->hwnd = hwnd;
    rt->absRect = def.absRect;
    rt->isSpan = def.isSpan;
    rt->muted = st.muted;
    rt->lowEndMode.store(st.lowEndMode);
    rt->media = playMedia;
    rt->targetFps.store(st.targetFps);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
    st.panes.push_back(rt);



    IDXGIDevice* dxgiDevice = nullptr;
    g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = sw;
    scd.Height = sh;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    
    HRESULT hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &rt->swapChain);
    if (FAILED(hrSc)) {
        log::warn("createswapchain flip_discard faild (hr = 0x" + toHex(hrSc) + "), retry flip_seq");
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &rt->swapChain);
        if (FAILED(hrSc)) {
            log::warn("createswapchain flip_seq faild (hr = 0x" + toHex(hrSc) + "), retry blit discard");
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            scd.BufferCount = 1;
            hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &rt->swapChain);
            if (FAILED(hrSc)) {
                log::error("createswapchain faild cmpletely (hr = 0x" + toHex(hrSc) + ")");
            }
        }
    }
    factory->Release(); adapter->Release(); dxgiDevice->Release();

    IMFMediaEngineClassFactory* mfFactory = nullptr;
    CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mfFactory));
    
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 3);
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxgiManager);
    EngineNotify* notify = new EngineNotify(rt);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    HRESULT hrMf = mfFactory ? mfFactory->CreateInstance(0, attr, &rt->engine) : E_FAIL;
    if (FAILED(hrMf)) {
        log::error("mfmediaengine classfactry createinstnce faild (hr = 0x" + toHex(hrMf) + ")");
    }
    
    BSTR url = SysAllocString(rt->media.c_str());
    HRESULT hrSrc = (rt->engine) ? rt->engine->SetSource(url) : E_FAIL;
    SysFreeString(url);
    if (FAILED(hrSrc)) {
        log::error("mediaengine setsrc faild (hr = 0x" + toHex(hrSrc) + ")");
    }
    
    if (rt->engine) {
        rt->engine->SetLoop(TRUE);
        rt->engine->SetMuted(rt->muted);
        if (rt->muted) rt->engine->SetVolume(0.0);
        float rate = (float)st.playbackSpeed;
        if (rate < 0.25f || rate > 4.0f) rate = 1.0f;
        rt->engine->SetPlaybackRate((double)rate);
    }

    if (notify) notify->Release();
    if (attr) attr->Release();
    if (mfFactory) mfFactory->Release();

    rt->threadRunning = true;
    rt->renderThread = std::thread([rt, sw, sh]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        timeBeginPeriod(1);
        LONGLONG lastPts = -1;
        bool firstFrameLogged = false;
        DWORD firstFrameTick = 0;
        int trimStage = 0;
        bool wasPaused = false;

        LARGE_INTEGER qpcFreq{};
        QueryPerformanceFrequency(&qpcFreq);
        LARGE_INTEGER lastPresentQpc{};
        QueryPerformanceCounter(&lastPresentQpc);

        while (rt->threadRunning) {
            if (rt->paused || g_manualPaused || !rt->engine || !rt->swapChain) {
                wasPaused = true;
                Sleep(80);
                QueryPerformanceCounter(&lastPresentQpc);
                continue;
            }

            if (wasPaused) {
                wasPaused = false;
                lastPts = -1;
                QueryPerformanceCounter(&lastPresentQpc);
            }

            LONGLONG pts = 0;
            HRESULT hr = rt->engine->OnVideoStreamTick(&pts);
            if (hr == S_OK) {
                if (pts != lastPts) {
                    lastPts = pts;

                    int targetFps = rt->targetFps.load();
                    if (targetFps != 60) targetFps = 30;

                    LARGE_INTEGER nowQpc{};
                    QueryPerformanceCounter(&nowQpc);
                    LONGLONG minIntervalQpc = (qpcFreq.QuadPart / targetFps) - (qpcFreq.QuadPart * 3 / 1000);

                    if (nowQpc.QuadPart - lastPresentQpc.QuadPart >= minIntervalQpc) {
                        lastPresentQpc = nowQpc;

                        IDXGISurface* surf = nullptr;
                        if (SUCCEEDED(rt->swapChain->GetBuffer(0, IID_PPV_ARGS(&surf))) && surf) {
                            MFVideoNormalizedRect srcR = {0.0f, 0.0f, 1.0f, 1.0f};
                            RECT dstR = {0, 0, sw, sh};
                            MFARGB bg = {0, 0, 0, 255};
                            rt->engine->TransferVideoFrame(surf, &srcR, &dstR, &bg);
                            surf->Release();

                            rt->swapChain->Present(1, 0);

                            if (!firstFrameLogged) {
                                firstFrameLogged = true;
                                firstFrameTick = GetTickCount();
                            }
                        }
                    }
                } else {
                    Sleep(2);
                }
            } else {
                Sleep(4);
            }

            if (firstFrameLogged && trimStage < 3) {
                DWORD elapsed = GetTickCount() - firstFrameTick;
                if (trimStage == 0 && elapsed >= 1500) {
                    trimStage = 1;
                    trimMemory();
                } else if (trimStage == 1 && elapsed >= 4000) {
                    trimStage = 2;
                    trimWorkingSet();
                } else if (trimStage == 2 && elapsed >= 8000) {
                    trimStage = 3;
                    trimWorkingSet();
                }
            }
        }
        timeEndPeriod(1);
        CoUninitialize();
    });

    return true;
}

void applyConfigToState(HINSTANCE inst, HWND host, EngineState& st, const Config& cfg) {
    if (!host || !IsWindow(host)) {
        host = findWallpaperHost();
    }
    auto desiredPanes = buildPanes(cfg);
    st.muted = cfg.muteByDefault;
    st.lowEndMode = cfg.lowEndMode;
    st.playbackSpeed = (float)cfg.playbackSpeed;
    st.targetFps = cfg.targetFps == 60 ? 60 : 30;
    for (auto p : st.panes) {
        p->targetFps.store(st.targetFps);
        p->lowEndMode.store(st.lowEndMode);
    }

    bool needsFullRebuild = (st.panes.size() != desiredPanes.size());
    if (!needsFullRebuild) {
        for (size_t i = 0; i < desiredPanes.size(); ++i) {
            if (st.panes[i]->isSpan != desiredPanes[i].isSpan ||
                st.panes[i]->media != desiredPanes[i].media) {
                needsFullRebuild = true;
                break;
            }
        }
    }

    if (needsFullRebuild) {
        for (auto p : st.panes) delete p;
        st.panes.clear();
        for (const auto& p : desiredPanes) {
            startPane(inst, host, p, st);
        }
    } else {
        for (size_t i = 0; i < desiredPanes.size(); ++i) {
            if (st.panes[i]->engine) {
                st.panes[i]->engine->SetMuted(st.muted);
                st.panes[i]->engine->SetPlaybackRate((double)st.playbackSpeed);
            }
        }
    }
    trimMemory();
}

bool isDesktopWindow(HWND hwnd) {
    if (!hwnd || hwnd == GetDesktopWindow()) return true;
    if (hwnd == g_progman || hwnd == g_workerW || hwnd == g_defView || hwnd == g_listview) return true;
    char className[256] = {0};
    GetClassNameA(hwnd, className, sizeof(className));
    return (strcmp(className, "WorkerW") == 0 ||
            strcmp(className, "Progman") == 0 ||
            strcmp(className, "SHELLDLL_DefView") == 0 ||
            strcmp(className, "SysListView32") == 0 ||
            strcmp(className, "#32769") == 0 ||
            strcmp(className, "Shell_TrayWnd") == 0 ||
            strcmp(className, "Shell_SecondaryTrayWnd") == 0 ||
            strcmp(className, "Windows.UI.Core.CoreWindow") == 0 ||
            strcmp(className, "XamlExplorerHostIslandWindow") == 0 ||
            strcmp(className, "TopLevelWindowForOverflowXamlIsland") == 0);
}

bool isWindowCoveringScreen(HWND hwnd, bool checkMaximized, bool checkFullscreen) {
    if (!checkMaximized && !checkFullscreen) return false;
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_VISIBLE)) return false;
    if (!(style & WS_MAXIMIZE) && ((style & WS_CAPTION) == WS_CAPTION)) {
        return false;
    }

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) || (exStyle & WS_EX_TRANSPARENT)) return false;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!hMon) return false;

    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    if (checkMaximized) {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        if (GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
            return true;
        }
    }

    RECT fr{};
    if (GetWindowRect(hwnd, &fr)) {
        if (checkFullscreen) {
            if (fr.left <= mi.rcMonitor.left + 5 && fr.top <= mi.rcMonitor.top + 5 &&
                fr.right >= mi.rcMonitor.right - 5 && fr.bottom >= mi.rcMonitor.bottom - 5) {
                return true;
            }
        }
        if (checkMaximized) {
            if (fr.left <= mi.rcWork.left + 15 && fr.top <= mi.rcWork.top + 15 &&
                fr.right >= mi.rcWork.right - 15 && fr.bottom >= mi.rcWork.bottom - 15) {
                return true;
            }
        }
    }
    return false;
}

struct OcclusionEnumContext {
    DWORD currentPid = 0;
    bool checkMaximized = false;
    bool checkFullscreen = false;
    bool foundCovering = false;
};

BOOL CALLBACK EnumWindowsOcclusionCallback(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<OcclusionEnumContext*>(lParam);
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->currentPid) return TRUE;

    if (isDesktopWindow(hwnd)) {
        return TRUE;
    }

    if (isWindowCoveringScreen(hwnd, ctx->checkMaximized, ctx->checkFullscreen)) {
        ctx->foundCovering = true;
        return FALSE;
    }

    return TRUE;
}

}

int runEngineFromConfig() {
    log::init(false);
    log::info("Starting Motion CLI wallpaper engine background service...");

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kEngineMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 20 && GetLastError() == ERROR_ALREADY_EXISTS; ++i) {
            Sleep(50);
            CloseHandle(mutex);
            mutex = CreateMutexW(nullptr, FALSE, kEngineMutexName);
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            log::warn("Engine mutex already held by another process, exiting");
            CloseHandle(mutex);
            return 0;
        }
    }

    Config cfg = Config::load();
    g_currentCfg = cfg;

    log::info("Configuration loaded: mode=" + std::string(cfg.mode == WallpaperMode::PerMonitor ? "per-monitor" : "span") +
              ", targetFps=" + std::to_string(cfg.targetFps) +
              ", media=" + narrow(cfg.currentMediaPath));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        log::error("CoInitializeEx failed (hr = 0x" + toHex(hr) + ")");
        CloseHandle(mutex);
        return 1;
    }
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        log::error("MFStartup failed (hr = 0x" + toHex(hr) + ") - check if Media Feature Pack is installed");
    }
    if (!initD3D11()) {
        log::error("initD3D11 failed, aborting engine execution");
        CloseHandle(mutex);
        return 1;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wallpaperWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    HWND host = findWallpaperHost();
    if (!host) {
        log::error("findWallpaperHost failed to resolve desktop host");
    }

    EngineState st;
    g_currentEngineState = &st;

    applyConfigToState(inst, host, st, cfg);

    HWND trayHwnd = CreateWindowExW(0, kWindowClass, L"MotionCLI Tray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    HICON hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIcon) hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!hIcon) hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    nid.hIcon = hIcon;
    wcscpy_s(nid.szTip, L"Motion CLI · Live Wallpaper");
    Shell_NotifyIconW(NIM_ADD, &nid);

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    if (stopEvent) ResetEvent(stopEvent);
    HANDLE reloadEvent = CreateEventW(nullptr, FALSE, FALSE, kReloadEventName);
    if (reloadEvent) ResetEvent(reloadEvent);

    bool running = true;
    DWORD waitTimeout = (DWORD)cfg.occlusionPollMs;
    if (waitTimeout < 50) waitTimeout = 50;
    if (waitTimeout > 250) waitTimeout = 150;

    DWORD lastPauseTick = 0;
    DWORD lastTrimTick = GetTickCount();
    DWORD lastEnumTick = 0;
    bool wasOccluded = false;
    DWORD currentPid = GetCurrentProcessId();
    HWND lastFw = nullptr;

    HANDLE waitHandles[2] = { stopEvent, reloadEvent };

    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "CRASH! exception code 0x%08X at 0x%p",
                    ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        log::error(buf);
        return EXCEPTION_CONTINUE_SEARCH;
    });

    while (running) {
        DWORD waitRes = MsgWaitForMultipleObjects(2, waitHandles, FALSE, waitTimeout, QS_ALLINPUT);
        if (waitRes == WAIT_OBJECT_0) {
            log::info("stopEvent signaled, shutdwn engine");
            running = false;
            break;
        }
        if (waitRes == WAIT_OBJECT_0 + 1) {
            g_currentCfg = Config::load();
            applyConfigToState(inst, host, st, g_currentCfg);
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                log::info("WM_QUIT recved, shutdwn engine");
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        bool occluded = g_manualPaused.load();

        if (!occluded && g_currentCfg.pauseOnBattery) {
            SYSTEM_POWER_STATUS sps{};
            if (GetSystemPowerStatus(&sps) && sps.ACLineStatus == 0) {
                occluded = true;
            }
        }

        if (!occluded) {
            HWND fw = GetForegroundWindow();
            if (fw && !isDesktopWindow(fw)) {
                DWORD fgPid = 0;
                GetWindowThreadProcessId(fw, &fgPid);
                if (fgPid != currentPid) {
                    if (g_currentCfg.pauseUnlessDesktop) {
                        occluded = true;
                    } else if (isWindowCoveringScreen(fw, g_currentCfg.pauseWhenMaximized, g_currentCfg.pauseOnFullscreen)) {
                        occluded = true;
                    }
                }
            }
        }

        if (occluded != wasOccluded) {
            wasOccluded = occluded;
            if (occluded) {
                lastPauseTick = GetTickCount();
                trimWorkingSet();
            }
        }

        if (occluded && g_currentCfg.occlusionTimeoutSec > 0) {
            DWORD now = GetTickCount();
            if (now - lastPauseTick >= (DWORD)(g_currentCfg.occlusionTimeoutSec * 1000)) {
                trimMemory();
            }
        }

        for (auto p : st.panes) {
            if (p->paused != occluded) {
                p->paused = occluded;
                if (p->engine) {
                    if (occluded) p->engine->Pause();
                    else if (!g_manualPaused) p->engine->Play();
                }
            }
        }

        DWORD nowTrim = GetTickCount();
        if (nowTrim - lastTrimTick >= 30000) {
            lastTrimTick = nowTrim;
            trimWorkingSet();
        }
    }

    g_currentEngineState = nullptr;

    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (trayHwnd) DestroyWindow(trayHwnd);
    CloseHandle(stopEvent);
    CloseHandle(reloadEvent);
    for (auto p : st.panes) delete p;
    st.panes.clear();

    UnregisterClassW(kWindowClass, inst);
    cleanupD3D11();
    MFShutdown();
    CoUninitialize();
    CloseHandle(mutex);
    log::info("engine cleanly terminated");
    return 0;
}

bool EngineController::restart(std::string& err) {
    if (isRunning()) {
        if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kReloadEventName)) {
            SetEvent(ev);
            CloseHandle(ev);
            Sleep(20);
            return true;
        }
    }

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = exePath;
    sei.lpParameters = L"--render";
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            for (int i = 0; i < 30; ++i) {
                if (isRunning()) break;
                Sleep(50);
            }
            CloseHandle(sei.hProcess);
        }
        return true;
    }
    err = "Failed to start engine process.";
    return false;
}

void EngineController::stop() {
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName)) {
        SetEvent(ev);
        CloseHandle(ev);
        for (int i = 0; i < 40; ++i) {
            if (!isRunning()) break;
            Sleep(50);
        }
    }
}

bool EngineController::isRunning() const {
    HANDLE mutex = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, kEngineMutexName);
    if (mutex) {
        CloseHandle(mutex);
        return true;
    }
    return false;
}

}
