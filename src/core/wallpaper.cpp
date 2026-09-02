#include "core/wallpaper.h"
#include "core/config.h"
#include "core/monitors.h"
#include "util/str.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>

#include <vector>
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

Config g_currentCfg;
std::atomic<bool> g_manualPaused{false};

struct PaneRT;
struct EngineState {
    std::vector<PaneRT*> panes;
    bool muted = true;
    bool lowEndMode = false;
    float playbackSpeed = 1.0f;
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

HWND findWallpaperHost() {
    g_progman = FindWindowW(L"Progman", nullptr);
    if (!g_progman) return nullptr;

    SendMessageTimeoutW(g_progman, 0x052C, 0x0000000D, 0, SMTO_NORMAL, 1000, nullptr);
    SendMessageTimeoutW(g_progman, 0x052C, 0x0000000D, 1, SMTO_NORMAL, 1000, nullptr);
    SendMessageTimeoutW(g_progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

    for (int retry = 0; retry < 30; ++retry) {
        HWND shellView = nullptr;
        EnumWindows([](HWND top, LPARAM lp) -> BOOL {
            HWND shell = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
            if (shell) {
                *reinterpret_cast<HWND*>(lp) = top;
                g_listview = FindWindowExW(shell, nullptr, L"SysListView32", nullptr);
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&shellView));

        if (shellView) {
            g_workerW = FindWindowExW(nullptr, shellView, L"WorkerW", nullptr);
            if (g_workerW && IsWindow(g_workerW)) {
                makeIconsTransparent();
                ShowWindow(g_workerW, SW_SHOW);
                SetWindowPos(g_workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                if (g_listview) {
                    UpdateWindow(g_listview);
                    InvalidateRect(g_listview, nullptr, TRUE);
                }
                return g_workerW;
            }
        }
        Sleep(50);
    }

    makeIconsTransparent();
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
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels, 4, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
    if (FAILED(hr)) {
        creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels, 4, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
        if (FAILED(hr)) return false;
    }

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_d3dContext->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    hr = MFCreateDXGIDeviceManager(&g_resetToken, &g_dxgiManager);
    if (FAILED(hr)) return false;

    hr = g_dxgiManager->ResetDevice(g_d3dDevice, g_resetToken);
    return SUCCEEDED(hr);
}

void cleanupD3D11() {
    if (g_dxgiManager) { g_dxgiManager->Release(); g_dxgiManager = nullptr; }
    if (g_d3dContext) { g_d3dContext->Release(); g_d3dContext = nullptr; }
    if (g_d3dDevice) { g_d3dDevice->Release(); g_d3dDevice = nullptr; }
}

void trimMemory() {
    if (g_d3dDevice) {
        IDXGIDevice3* dxgi3 = nullptr;
        if (SUCCEEDED(g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice3), (void**)&dxgi3)) && dxgi3) {
            dxgi3->Trim();
            dxgi3->Release();
        }
    }
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
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
    std::thread renderThread;

    void setSource(const std::wstring& newMedia, bool isMuted, float speed) {
        media = newMedia;
        muted = isMuted;
        if (engine) {
            BSTR url = SysAllocString(media.c_str());
            engine->SetSource(url);
            SysFreeString(url);
            engine->SetLoop(TRUE);
            engine->SetMuted(muted);
            if (speed < 0.25f || speed > 4.0f) speed = 1.0f;
            engine->SetPlaybackRate((double)speed);
            if (!paused && !g_manualPaused) engine->Play();
        }
    }

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
                    SetWindowPos(hwnd, nullptr, pt.x, pt.y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
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

bool startPane(HINSTANCE inst, HWND host, const PaneDef& def, EngineState& st) {
    int ax = def.absRect.left, ay = def.absRect.top;
    int w = def.absRect.right - ax, h = def.absRect.bottom - ay;
    if (w <= 0 || h <= 0) return false;

    HWND parent = host ? host : nullptr;
    DWORD style = parent ? (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN) : (WS_POPUP | WS_VISIBLE);
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    POINT pt = { ax, ay };
    if (parent) ScreenToClient(parent, &pt);

    HWND hwnd = CreateWindowExW(
        exStyle, kWindowClass, L"MotionCLI Wallpaper",
        style, pt.x, pt.y, w, h, parent, nullptr, inst, nullptr);
    if (!hwnd) return false;

    if (parent) {
        SetWindowPos(hwnd, HWND_BOTTOM, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    PaneRT* rt = new PaneRT();
    rt->hwnd = hwnd;
    rt->absRect = def.absRect;
    rt->isSpan = def.isSpan;
    rt->muted = st.muted;
    rt->media = def.media;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
    st.panes.push_back(rt);

    IDXGIDevice* dxgiDevice = nullptr;
    g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    
    HRESULT hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &rt->swapChain);
    if (FAILED(hrSc)) {
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &rt->swapChain);
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

    mfFactory->CreateInstance(0, attr, &rt->engine);
    
    BSTR url = SysAllocString(rt->media.c_str());
    rt->engine->SetSource(url);
    SysFreeString(url);
    
    rt->engine->SetLoop(TRUE);
    rt->engine->SetMuted(rt->muted);
    float rate = (float)st.playbackSpeed;
    if (rate < 0.25f || rate > 4.0f) rate = 1.0f;
    rt->engine->SetPlaybackRate((double)rate);

    notify->Release(); attr->Release(); mfFactory->Release();

    rt->threadRunning = true;
    rt->renderThread = std::thread([rt, w, h]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LONGLONG lastPts = -1;

        while (rt->threadRunning) {
            if (rt->paused || g_manualPaused || !rt->engine || !rt->swapChain) {
                Sleep(100);
                continue;
            }

            LONGLONG pts = 0;
            HRESULT hr = rt->engine->OnVideoStreamTick(&pts);
            if (hr == S_OK) {
                if (pts != lastPts) {
                    lastPts = pts;
                    IDXGISurface* surf = nullptr;
                    if (SUCCEEDED(rt->swapChain->GetBuffer(0, IID_PPV_ARGS(&surf))) && surf) {
                        MFVideoNormalizedRect srcR = {0.0f, 0.0f, 1.0f, 1.0f};
                        RECT dstR = {0, 0, w, h};
                        MFARGB bg = {0, 0, 0, 255};
                        rt->engine->TransferVideoFrame(surf, &srcR, &dstR, &bg);
                        surf->Release();
                        rt->swapChain->Present(1, 0);
                    }
                } else {
                    Sleep(1);
                }
            } else {
                Sleep(2);
            }
        }
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
            st.panes[i]->setSource(desiredPanes[i].media, st.muted, st.playbackSpeed);
        }
    }
    trimMemory();
}

}

int runEngineFromConfig() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kEngineMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 20 && GetLastError() == ERROR_ALREADY_EXISTS; ++i) {
            Sleep(50);
            CloseHandle(mutex);
            mutex = CreateMutexW(nullptr, FALSE, kEngineMutexName);
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex);
            return 0;
        }
    }

    Config cfg = Config::load();
    g_currentCfg = cfg;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        CloseHandle(mutex);
        return 1;
    }
    MFStartup(MF_VERSION);
    initD3D11();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wallpaperWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    HWND host = findWallpaperHost();
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
    HANDLE reloadEvent = CreateEventW(nullptr, FALSE, FALSE, kReloadEventName);

    bool running = true;
    DWORD waitTimeout = (DWORD)cfg.occlusionPollMs;
    if (waitTimeout < 50) waitTimeout = 50;
    if (waitTimeout > 300) waitTimeout = 150;

    DWORD lastPauseTick = 0;
    bool wasOccluded = false;
    DWORD currentPid = GetCurrentProcessId();

    HANDLE waitHandles[2] = { stopEvent, reloadEvent };

    while (running) {
        DWORD waitRes = MsgWaitForMultipleObjects(2, waitHandles, FALSE, waitTimeout, QS_ALLINPUT);
        if (waitRes == WAIT_OBJECT_0) {
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
            if (fw) {
                DWORD fgPid = 0;
                GetWindowThreadProcessId(fw, &fgPid);
                if (fgPid != currentPid) {
                    char className[256] = {0};
                    GetClassNameA(fw, className, sizeof(className));
                    bool isDesktopWindow = (strcmp(className, "WorkerW") == 0 ||
                                            strcmp(className, "Progman") == 0 ||
                                            strcmp(className, "SysListView32") == 0 ||
                                            strcmp(className, "#32769") == 0 ||
                                            strcmp(className, "Shell_TrayWnd") == 0 ||
                                            strcmp(className, "Shell_SecondaryTrayWnd") == 0 ||
                                            strcmp(className, "Windows.UI.Core.CoreWindow") == 0 ||
                                            strcmp(className, "XamlExplorerHostIslandWindow") == 0 ||
                                            strcmp(className, "TopLevelWindowForOverflowXamlIsland") == 0 ||
                                            fw == GetDesktopWindow());

                    if (!isDesktopWindow) {
                        if (g_currentCfg.pauseUnlessDesktop) {
                            occluded = true;
                        } else if (g_currentCfg.pauseWhenMaximized || g_currentCfg.pauseOnFullscreen) {
                            WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
                            if (g_currentCfg.pauseWhenMaximized && GetWindowPlacement(fw, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
                                occluded = true;
                            } else {
                                RECT fr;
                                if (GetWindowRect(fw, &fr)) {
                                    HMONITOR hMon = MonitorFromWindow(fw, MONITOR_DEFAULTTONEAREST);
                                    MONITORINFO mi = { sizeof(MONITORINFO) };
                                    if (GetMonitorInfoW(hMon, &mi)) {
                                        int winW = fr.right - fr.left;
                                        int winH = fr.bottom - fr.top;
                                        int workW = mi.rcWork.right - mi.rcWork.left;
                                        int workH = mi.rcWork.bottom - mi.rcWork.top;
                                        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
                                        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

                                        if (g_currentCfg.pauseOnFullscreen && winW >= monW - 8 && winH >= monH - 8) {
                                            occluded = true;
                                        } else if (g_currentCfg.pauseWhenMaximized && winW >= workW - 20 && winH >= workH - 20) {
                                            occluded = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (occluded != wasOccluded) {
            wasOccluded = occluded;
            if (occluded) {
                lastPauseTick = GetTickCount();
                trimMemory();
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
    }

    g_currentEngineState = nullptr;

    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (trayHwnd) DestroyWindow(trayHwnd);
    CloseHandle(stopEvent);
    CloseHandle(reloadEvent);
    st.panes.clear();

    UnregisterClassW(kWindowClass, inst);
    cleanupD3D11();
    MFShutdown();
    CoUninitialize();
    CloseHandle(mutex);
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
