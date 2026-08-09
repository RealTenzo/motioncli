#include "core/wallpaper.h"
#include "core/config.h"
#include "core/monitors.h"
#include "util/str.h"

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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

void logLine(const wchar_t* msg) {
    wprintf(L"%s\n", msg);
}

template <typename... Args>
void logf(const wchar_t* fmt, Args... args) {
    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args...);
    wprintf(L"%s\n", buf);
}

HWND g_progman = nullptr;
HWND g_workerW = nullptr;
HWND g_listview = nullptr;

BOOL CALLBACK enumWorkerWCb(HWND top, LPARAM) {
    HWND shell = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (shell) {
        g_workerW = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        g_listview = FindWindowExW(shell, nullptr, L"SysListView32", nullptr);
    }
    return TRUE;
}

void makeIconsTransparent() {
    if (g_listview) {
        SendMessageW(g_listview, 0x1001, 0, (LPARAM)-1);
        SendMessageW(g_listview, 0x1026, 0, (LPARAM)-1);
        InvalidateRect(g_listview, nullptr, TRUE);
    }
}

HWND findWallpaperHost() {
    g_progman = FindWindowW(L"Progman", nullptr);
    SendMessageTimeoutW(g_progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

    for (int i = 0; i < 20; ++i) {
        g_workerW = nullptr;
        g_listview = nullptr;
        EnumWindows(enumWorkerWCb, 0);
        if (g_workerW && IsWindow(g_workerW)) {
            makeIconsTransparent();
            ShowWindow(g_workerW, SW_SHOW);
            SetWindowPos(g_workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            if (g_listview) {
                UpdateWindow(g_listview);
                InvalidateRect(g_listview, nullptr, TRUE);
            }
            logf(L"Host: WorkerW=0x%p", g_workerW);
            return g_workerW;
        }
        Sleep(50);
    }

    logLine(L"No empty WorkerW found - using Progman as host.");
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
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, featureLevels, 2, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
    if (FAILED(hr)) return false;

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

    ~PaneRT() {
        threadRunning = false;
        if (renderThread.joinable()) renderThread.join();
        if (engine) { engine->Shutdown(); engine->Release(); engine = nullptr; }
        if (swapChain) { swapChain->Release(); swapChain = nullptr; }
        if (hwnd) DestroyWindow(hwnd);
    }
};

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
        if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY) {
            if (m_pane && m_pane->engine) m_pane->engine->Play();
        }
        return S_OK;
    }
};

struct EngineState {
    std::vector<PaneRT*> panes;
    bool muted = true;
    bool lowEndMode = false;
    float playbackSpeed = 1.0f;
    ~EngineState() { for (auto p : panes) delete p; }
};

static LRESULT CALLBACK wallpaperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    PaneRT* p = reinterpret_cast<PaneRT*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_USER + 1:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, 1, L"Quit MotionCLI");
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == 1) PostQuitMessage(0);
            }
            return 0;
        case WM_SIZE:
        case WM_DISPLAYCHANGE:
            if (p) {
                if (msg == WM_DISPLAYCHANGE) {
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
            }
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

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED, kWindowClass, L"MotionCLI Wallpaper",
        WS_POPUP | WS_VISIBLE, ax, ay, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    if (host) {
        SetParent(hwnd, host);
        SetWindowLongW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        POINT pt = { ax, ay };
        ScreenToClient(host, &pt);
        SetWindowPos(hwnd, nullptr, pt.x, pt.y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    
    factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &rt->swapChain);
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
    float rate = st.lowEndMode ? 0.5f : st.playbackSpeed;
    if (rate < 0.25f || rate > 4.0f) rate = 1.0f;
    rt->engine->SetPlaybackRate((double)rate);

    notify->Release(); attr->Release(); mfFactory->Release();

    rt->threadRunning = true;
    bool lowEnd = st.lowEndMode;
    rt->renderThread = std::thread([rt, w, h, lowEnd]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (rt->threadRunning) {
            if (rt->paused || !rt->engine || !rt->swapChain) {
                Sleep(16);
                continue;
            }
            LONGLONG pts;
            if (rt->engine->OnVideoStreamTick(&pts) == S_OK) {
                IDXGISurface* surf = nullptr;
                if (SUCCEEDED(rt->swapChain->GetBuffer(0, IID_PPV_ARGS(&surf)))) {
                    MFVideoNormalizedRect srcR = {0.0f, 0.0f, 1.0f, 1.0f};
                    RECT dstR = {0, 0, w, h};
                    MFARGB bg = {0,0,0,255};
                    rt->engine->TransferVideoFrame(surf, &srcR, &dstR, &bg);
                    surf->Release();
                    rt->swapChain->Present(lowEnd ? 2 : 1, 0);
                }
            } else {
                Sleep(5);
            }
        }
        CoUninitialize();
    });

    return true;
}

}

int runEngineFromConfig() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"MotionCLI_EngineMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        logLine(L"Engine already running.");
        CloseHandle(mutex);
        return 0;
    }

    Config cfg = Config::load();
    auto panes = buildPanes(cfg);
    logf(L"Panes requested: %d", (int)panes.size());
    if (panes.empty()) { logLine(L"No playable panes - exiting."); return 0; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
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
    st.muted = cfg.muteByDefault;
    st.lowEndMode = cfg.lowEndMode;
    st.playbackSpeed = cfg.playbackSpeed;

    for (const auto& p : panes) {
        startPane(inst, host, p, st);
    }

    HWND trayHwnd = CreateWindowExW(0, kWindowClass, L"MotionCLI Tray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"MotionCLI");
    Shell_NotifyIconW(NIM_ADD, &nid);

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);

    bool running = true;
    while (running) {
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

        if (WaitForSingleObject(stopEvent, 16) == WAIT_OBJECT_0) {
            logLine(L"Stop event received.");
            running = false;
        }
        
        // Aggressive full-screen and focus occlusion (only pause on maximized/fullscreen)
        HWND fw = GetForegroundWindow();
        bool occluded = false;
        if (fw) {
            char className[256] = {0};
            GetClassNameA(fw, className, sizeof(className));
            if (strcmp(className, "WorkerW") != 0 && 
                strcmp(className, "Progman") != 0 && 
                strcmp(className, "#32769") != 0 &&
                strcmp(className, "Shell_TrayWnd") != 0 &&
                strcmp(className, "Shell_SecondaryTrayWnd") != 0 &&
                fw != GetDesktopWindow()) {
                
                WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
                if (GetWindowPlacement(fw, &wp)) {
                    if (wp.showCmd == SW_SHOWMAXIMIZED) {
                        occluded = true;
                    }
                }
                
                if (!occluded) {
                    RECT fr;
                    if (GetWindowRect(fw, &fr)) {
                        if (fr.left <= 0 && fr.top <= 0 && 
                            fr.right >= GetSystemMetrics(SM_CXSCREEN) && 
                            fr.bottom >= GetSystemMetrics(SM_CYSCREEN)) {
                            occluded = true;
                        }
                    } else {
                        // Fallback for elevated windows (like Task Manager) where GetWindowRect might fail
                        occluded = true;
                    }
                }
            }
        }
        for (auto p : st.panes) {
            if (p->paused != occluded) {
                p->paused = occluded;
                if (p->engine) {
                    if (occluded) p->engine->Pause();
                    else p->engine->Play();
                }
            }
        }
    }

    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (trayHwnd) DestroyWindow(trayHwnd);
    CloseHandle(stopEvent);
    st.panes.clear();

    UnregisterClassW(kWindowClass, inst);
    cleanupD3D11();
    MFShutdown();
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}

bool EngineController::restart(std::string& err) {
    stop();
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = L"motioncli.exe";
    sei.lpParameters = L"--render";
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        CloseHandle(sei.hProcess);
        return true;
    }
    err = "Failed to start engine process.";
    return false;
}

void EngineController::stop() {
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName)) {
        SetEvent(ev);
        CloseHandle(ev);
        Sleep(200);
    }
}

bool EngineController::isRunning() const {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"MotionCLI_EngineMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return true;
    }
    if (mutex) CloseHandle(mutex);
    return false;
}

}
