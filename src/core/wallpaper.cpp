#include "core/wallpaper.h"
#include "core/config.h"
#include "core/monitors.h"
#include "util/str.h"
#include "util/log.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>

#include <timeapi.h>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
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
HWND g_activeHost = nullptr;

Config g_currentCfg;
std::atomic<bool> g_manualPaused{false};

std::string toHex(uintptr_t val) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%08llX", (unsigned long long)val);
    return buf;
}

std::wstring mediaSourceUrl(const std::wstring& media) {
    if (media.empty()) return media;
    size_t scheme = media.find(L"://");
    if (scheme != std::wstring::npos && scheme <= 8) return media;
    if (media.size() >= 5 && _wcsnicmp(media.c_str(), L"file:", 5) == 0) return media;
    std::wstring fwd = media;
    for (auto& c : fwd) if (c == L'\\') c = L'/';
    bool unc = fwd.size() >= 2 && fwd[0] == L'/' && fwd[1] == L'/';
    int ulen = WideCharToMultiByte(CP_UTF8, 0, fwd.c_str(), (int)fwd.size(), nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return media;
    std::string utf8((size_t)ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, fwd.c_str(), (int)fwd.size(), utf8.data(), ulen, nullptr, nullptr);
    static const char* hexd = "0123456789ABCDEF";
    std::string enc;
    enc.reserve(utf8.size() + 16);
    for (unsigned char b : utf8) {
        bool plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
                     (b >= '0' && b <= '9') || b == '-' || b == '_' ||
                     b == '.' || b == '~' || b == '/' || b == ':';
        if (plain) enc.push_back((char)b);
        else { enc.push_back('%'); enc.push_back(hexd[b >> 4]); enc.push_back(hexd[b & 15]); }
    }
    std::wstring wenc;
    wenc.reserve(enc.size());
    for (char c : enc) wenc.push_back((wchar_t)(unsigned char)c);
    if (unc) return L"file:" + wenc;
    if (wenc.size() >= 2 && wenc[1] == L':') return L"file:///" + wenc;
    if (!wenc.empty() && wenc[0] == L'/') return L"file://" + wenc;
    return L"file:///" + wenc;
}

bool localMediaMissing(const std::wstring& media) {
    if (media.empty()) return true;
    size_t scheme = media.find(L"://");
    if (scheme != std::wstring::npos && scheme <= 8) return false;
    if (media.size() >= 5 && _wcsnicmp(media.c_str(), L"file:", 5) == 0) return false;
    DWORD attr = GetFileAttributesW(media.c_str());
    return attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool systemCanDecodeH264() {
    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                           &input, nullptr, &acts, &count);
    if (FAILED(hr)) return true;
    bool ok = count > 0 && acts != nullptr;
    if (acts) {
        for (UINT32 i = 0; i < count; ++i) if (acts[i]) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return ok;
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
        SendMessageW(g_listview, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_TRANSPARENTBKGND, LVS_EX_TRANSPARENTBKGND);
        InvalidateRect(g_listview, nullptr, TRUE);
    }
}

HWND findWallpaperHost(int maxRetries = 100, int delayMs = 100) {
    log::info("locating desktop wallpaper host...");
    g_progman = FindWindowW(L"Progman", nullptr);
    if (!g_progman) {
        g_progman = GetShellWindow();
    }
    if (!g_progman) {
        log::error("progman window not found");
        return nullptr;
    }
    log::info("found Progman: 0x" + toHex(reinterpret_cast<uintptr_t>(g_progman)));

    DWORD_PTR dummy = 0;
    SendMessageTimeoutW(g_progman, 0x052C, 0x0000000D, 0, SMTO_NORMAL, 1000, &dummy);
    SendMessageTimeoutW(g_progman, 0x052C, 0x0000000D, 1, SMTO_NORMAL, 1000, &dummy);
    SendMessageTimeoutW(g_progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &dummy);

    struct TopScan {
        HWND iconTop = nullptr;
        HWND emptyTop = nullptr;
        int workerCount = 0;
    };
    struct ChildScan {
        HWND emptyWorker = nullptr;
        int workerCount = 0;
    };

    int topSeen = 0;
    int childSeen = 0;

    for (int retry = 0; retry < maxRetries; ++retry) {
        TopScan top{};
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<TopScan*>(lp);
            if (!s->iconTop && FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr))
                s->iconTop = hwnd;
            wchar_t cls[32] = {0};
            if (GetClassNameW(hwnd, cls, 32) && wcscmp(cls, L"WorkerW") == 0) {
                s->workerCount++;
                if (!s->emptyTop && !FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr))
                    s->emptyTop = hwnd;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&top));

        ChildScan child{};
        EnumChildWindows(g_progman, [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<ChildScan*>(lp);
            wchar_t cls[32] = {0};
            if (!GetClassNameW(hwnd, cls, 32) || wcscmp(cls, L"WorkerW") != 0) return TRUE;
            s->workerCount++;
            if (!s->emptyWorker && !FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr))
                s->emptyWorker = hwnd;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&child));

        topSeen = top.workerCount;
        childSeen = child.workerCount;

        HWND iconDefView = nullptr;
        bool iconsInTopWorker = false;
        if (top.iconTop) {
            iconDefView = FindWindowExW(top.iconTop, nullptr, L"SHELLDLL_DefView", nullptr);
            if (iconDefView) {
                wchar_t icls[32] = {0};
                iconsInTopWorker = GetClassNameW(top.iconTop, icls, 32) && wcscmp(icls, L"WorkerW") == 0;
            }
        }
        if (!iconDefView)
            iconDefView = FindWindowExW(g_progman, nullptr, L"SHELLDLL_DefView", nullptr);
        g_defView = iconDefView;
        g_listview = iconDefView ? FindWindowExW(iconDefView, nullptr, L"SysListView32", nullptr) : nullptr;

        HWND host = nullptr;
        bool nested = false;
        if (iconsInTopWorker) {
            HWND after = FindWindowExW(nullptr, top.iconTop, L"WorkerW", nullptr);
            while (after) {
                if (!FindWindowExW(after, nullptr, L"SHELLDLL_DefView", nullptr)) {
                    host = after;
                    break;
                }
                after = FindWindowExW(nullptr, after, L"WorkerW", nullptr);
            }
            if (!host) host = top.emptyTop;
        } else if (child.emptyWorker) {
            host = child.emptyWorker;
            nested = true;
        }

        if (host && IsWindow(host)) {
            g_workerW = host;
            makeIconsTransparent();
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (vw <= 0) vw = GetSystemMetrics(SM_CXSCREEN);
            if (vh <= 0) vh = GetSystemMetrics(SM_CYSCREEN);
            ShowWindow(g_workerW, SW_SHOW);
            SetWindowPos(g_workerW, HWND_BOTTOM, vx, vy, vw, vh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (g_listview) {
                UpdateWindow(g_listview);
                InvalidateRect(g_listview, nullptr, TRUE);
            }
            if (nested)
                log::info("attached to Progman child WorkerW host: 0x" + toHex(reinterpret_cast<uintptr_t>(g_workerW)));
            else
                log::info("attached to WorkerW host: 0x" + toHex(reinterpret_cast<uintptr_t>(g_workerW)));
            return host;
        }
        Sleep(delayMs);
    }

    log::warn("no empty WorkerW found (top=" + std::to_string(topSeen) + ", progman children=" + std::to_string(childSeen) + ")");
    return nullptr;
}

void hideStrayWorkerWs() {
    int hidden = 0;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* n = reinterpret_cast<int*>(lp);
        wchar_t cls[32] = {0};
        if (!GetClassNameW(hwnd, cls, 32) || wcscmp(cls, L"WorkerW") != 0) return TRUE;
        if (hwnd == g_workerW) return TRUE;
        if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) return TRUE;
        ShowWindow(hwnd, SW_HIDE);
        (*n)++;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hidden));
    if (hidden > 0)
        log::info("hidden stray WorkerW windows: " + std::to_string(hidden));
}

struct PaneDef {
    RECT absRect;
    std::wstring media;
    bool isSpan;
};

std::vector<PaneDef> buildPanes(const Config& cfg) {
    std::vector<PaneDef> panes;
    if (cfg.mode == WallpaperMode::Span) {
        if (!cfg.currentMediaPath.empty() && !localMediaMissing(cfg.currentMediaPath)) {
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
            if (localMediaMissing(kv.second)) continue;
            for (const auto& m : monitors) {
                if (m.device == kv.first && !kv.second.empty()) {
                    panes.push_back(PaneDef{ { m.x, m.y, m.x + m.width, m.y + m.height }, kv.second, false });
                    break;
                }
            }
        }
        if (panes.empty() && !cfg.currentMediaPath.empty() && !localMediaMissing(cfg.currentMediaPath)) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (vw <= 0) vw = GetSystemMetrics(SM_CXSCREEN);
            if (vh <= 0) vh = GetSystemMetrics(SM_CYSCREEN);
            panes.push_back(PaneDef{ { vx, vy, vx + vw, vy + vh }, cfg.currentMediaPath, true });
        }
    }
    return panes;
}

ID3D11Device* g_d3dDevice = nullptr;
ID3D11DeviceContext* g_d3dContext = nullptr;
IMFDXGIDeviceManager* g_dxgiManager = nullptr;
UINT g_resetToken = 0;

bool initD3D11() {
    log::info("initializing D3D11 device and context...");
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL highLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL lowLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, highLevels, 2, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, lowLevels, 4, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
    }
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, creationFlags, lowLevels, 4, D3D11_SDK_VERSION, &g_d3dDevice, &fl, &g_d3dContext);
        if (SUCCEEDED(hr)) {
            log::warn("hardware D3D11 unavailable, using WARP software rasterizer");
        }
    }
    if (FAILED(hr)) {
        log::error("D3D11 device creation failed completely (hr = 0x" + toHex(hr) + ")");
        return false;
    }

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_d3dContext->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    hr = MFCreateDXGIDeviceManager(&g_resetToken, &g_dxgiManager);
    if (FAILED(hr)) {
        log::error("MFCreateDXGIDeviceManager failed (hr = 0x" + toHex(hr) + ")");
        return false;
    }

    hr = g_dxgiManager->ResetDevice(g_d3dDevice, g_resetToken);
    if (FAILED(hr)) {
        log::error("DXGIManager ResetDevice failed (hr = 0x" + toHex(hr) + ")");
        return false;
    }

    log::info("DXGI device manager registered ok");
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
    trimWorkingSet();
}

struct PaneRT {
    HWND hwnd = nullptr;
    RECT absRect = {};
    bool isSpan = false;
    bool paused = false;
    bool muted = false;
    std::wstring media;
    int paneW = 0;
    int paneH = 0;

    IDXGISwapChain1* swapChain = nullptr;
    IMFMediaEngine* engine = nullptr;

    std::atomic<bool> threadRunning{false};
    std::atomic<int> targetFps{30};
    std::atomic<bool> wantSleep{false};
    std::atomic<bool> asleep{false};
    DWORD lastLoadAttempt = 0;
    std::mutex mediaMutex;
    std::thread renderThread;

    void setSource(const std::wstring& newMedia, bool isMuted, float speed) {
        media = newMedia;
        muted = isMuted;
        if (wantSleep.load() || asleep.load()) return;
        IMFMediaEngine* eng = nullptr;
        {
            std::lock_guard<std::mutex> guard(mediaMutex);
            eng = engine;
            if (eng) eng->AddRef();
        }
        if (eng) {
            std::wstring src = mediaSourceUrl(media);
            BSTR url = SysAllocString(src.c_str());
            HRESULT hr = eng->SetSource(url);
            SysFreeString(url);
            if (FAILED(hr)) {
                log::error("MediaEngine SetSource failed (hr = 0x" + toHex(hr) + ")");
                eng->Release();
                return;
            }
            eng->SetLoop(TRUE);
            eng->SetMuted(muted);
            if (muted) eng->SetVolume(0.0);
            if (speed < 0.25f || speed > 4.0f) speed = 1.0f;
            eng->SetPlaybackRate((double)speed);
            if (!paused && !g_manualPaused) eng->Play();
            eng->Release();
        }
    }

    void unloadMedia() {
        if (asleep.load()) return;
        wantSleep.store(true);
        IMFMediaEngine* eng = nullptr;
        IDXGISwapChain1* sw = nullptr;
        {
            std::lock_guard<std::mutex> guard(mediaMutex);
            eng = engine; engine = nullptr;
            sw = swapChain; swapChain = nullptr;
        }
        if (eng) { eng->Pause(); eng->Shutdown(); eng->Release(); }
        if (sw) sw->Release();
        asleep.store(true);
        trimWorkingSet();
        log::info("media unloaded to save power");
    }

    bool loadMedia(EngineState& st);

    ~PaneRT() {
        threadRunning = false;
        if (renderThread.joinable()) renderThread.join();
        std::lock_guard<std::mutex> guard(mediaMutex);
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
                trimWorkingSet();
            }
        }
        return S_OK;
    }
};

IDXGISwapChain1* createPaneSwapChain(HWND hwnd, int w, int h) {
    IDXGIDevice* dxgiDevice = nullptr;
    if (!g_d3dDevice || FAILED(g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice) {
        log::error("QueryInterface IDXGIDevice failed");
        return nullptr;
    }
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) {
        log::error("GetAdapter failed");
        dxgiDevice->Release();
        return nullptr;
    }
    IDXGIFactory2* factory = nullptr;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory)) || !factory) {
        log::error("GetParent IDXGIFactory2 failed");
        adapter->Release();
        dxgiDevice->Release();
        return nullptr;
    }

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

    IDXGISwapChain1* chain = nullptr;
    HRESULT hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &chain);
    if (FAILED(hrSc)) {
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &chain);
        if (FAILED(hrSc)) {
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            scd.BufferCount = 1;
            scd.Scaling = DXGI_SCALING_STRETCH;
            hrSc = factory->CreateSwapChainForHwnd(g_d3dDevice, hwnd, &scd, nullptr, nullptr, &chain);
        }
    }
    factory->Release(); adapter->Release(); dxgiDevice->Release();

    if (!chain) {
        log::error("swapchain creation failed (hr = 0x" + toHex(hrSc) + ")");
        return nullptr;
    }
    return chain;
}

IMFMediaEngine* createPaneEngine(PaneRT* rt) {
    IMFMediaEngineClassFactory* mfFactory = nullptr;
    HRESULT hrFactory = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mfFactory));
    if (FAILED(hrFactory) || !mfFactory) {
        log::error("Media Foundation unavailable (hr = 0x" + toHex(hrFactory) + ")");
        return nullptr;
    }

    IMFAttributes* attr = nullptr;
    HRESULT hrAttr = MFCreateAttributes(&attr, 3);
    EngineNotify* notify = new EngineNotify(rt);
    if (FAILED(hrAttr) || !attr) {
        log::error("MFCreateAttributes failed (hr = 0x" + toHex(hrAttr) + ")");
        notify->Release();
        mfFactory->Release();
        return nullptr;
    }
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxgiManager);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    IMFMediaEngine* eng = nullptr;
    HRESULT hrMf = mfFactory->CreateInstance(0, attr, &eng);
    notify->Release();
    attr->Release();
    mfFactory->Release();
    if (FAILED(hrMf) || !eng) {
        log::error("MFMediaEngine CreateInstance failed (hr = 0x" + toHex(hrMf) + ")");
        return nullptr;
    }
    return eng;
}

void dropPane(EngineState& st, HWND hwnd, PaneRT* rt) {
    for (auto it = st.panes.begin(); it != st.panes.end(); ++it) {
        if (*it == rt) { st.panes.erase(it); break; }
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    delete rt;
}

bool PaneRT::loadMedia(EngineState& st) {
    if (!asleep.load()) return true;
    if (localMediaMissing(media)) {
        if (lastLoadAttempt == 0)
            log::error("wallpaper media not found, skipping reload: " + narrow(media));
        return false;
    }
    IDXGISwapChain1* sw = createPaneSwapChain(hwnd, paneW, paneH);
    if (!sw) return false;
    {
        std::lock_guard<std::mutex> guard(mediaMutex);
        if (swapChain) swapChain->Release();
        swapChain = sw;
    }
    IMFMediaEngine* eng = createPaneEngine(this);
    if (!eng) {
        std::lock_guard<std::mutex> guard(mediaMutex);
        if (swapChain) { swapChain->Release(); swapChain = nullptr; }
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(mediaMutex);
        if (engine) { engine->Shutdown(); engine->Release(); }
        engine = eng;
    }
    asleep.store(false);
    wantSleep.store(false);
    muted = st.muted;
    setSource(media, st.muted, (float)st.playbackSpeed);
    log::info("media reloaded on resume");
    return true;
}

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
    if (!host || !IsWindow(host)) {
        log::error("wallpaper host window invalid, skipping pane");
        return false;
    }
    if (localMediaMissing(def.media)) {
        log::error("wallpaper media not found, skipping pane: " + narrow(def.media));
        return false;
    }

    log::info("starting pane: bounds [" + std::to_string(ax) + ", " +
              std::to_string(ay) + ", " + std::to_string(w) + "x" + std::to_string(h) +
              "], media: " + narrow(def.media));

    HWND parent = host;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    POINT pt = { ax, ay };
    ScreenToClient(parent, &pt);

    HWND hwnd = CreateWindowExW(
        exStyle, kWindowClass, L"MotionCLI Wallpaper",
        style, pt.x, pt.y, w, h, parent, nullptr, inst, nullptr);
    if (!hwnd) {
        log::error("CreateWindowExW failed (err = " + std::to_string(GetLastError()) + ")");
        return false;
    }

    SetWindowPos(hwnd, HWND_BOTTOM, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    if (g_listview) {
        InvalidateRect(g_listview, nullptr, TRUE);
        UpdateWindow(g_listview);
    }

    PaneRT* rt = new PaneRT();
    rt->hwnd = hwnd;
    rt->absRect = def.absRect;
    rt->isSpan = def.isSpan;
    rt->muted = st.muted;
    rt->media = def.media;
    rt->paneW = w;
    rt->paneH = h;
    rt->targetFps.store(st.targetFps);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(rt));
    st.panes.push_back(rt);

    rt->swapChain = createPaneSwapChain(hwnd, w, h);
    if (!rt->swapChain) {
        dropPane(st, hwnd, rt);
        return false;
    }

    rt->engine = createPaneEngine(rt);
    if (!rt->engine) {
        dropPane(st, hwnd, rt);
        return false;
    }

    std::wstring srcUrl = mediaSourceUrl(rt->media);
    BSTR url = SysAllocString(srcUrl.c_str());
    HRESULT hrSrc = rt->engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hrSrc)) {
        log::error("MediaEngine SetSource failed (hr = 0x" + toHex(hrSrc) + ")");
        rt->engine->Shutdown();
        rt->engine->Release();
        rt->engine = nullptr;
        dropPane(st, hwnd, rt);
        return false;
    }

    if (rt->engine) {
        rt->engine->SetLoop(TRUE);
        rt->engine->SetMuted(rt->muted);
        if (rt->muted) rt->engine->SetVolume(0.0);
        float rate = (float)st.playbackSpeed;
        if (rate < 0.25f || rate > 4.0f) rate = 1.0f;
        rt->engine->SetPlaybackRate((double)rate);
    }

    rt->threadRunning = true;
    rt->renderThread = std::thread([rt, w, h]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        timeBeginPeriod(1);
        LONGLONG lastPts = -1;
        bool firstFrameLogged = false;
        bool tickWarned = false;
        bool bufWarned = false;
        bool transferWarned = false;
        bool presentWarned = false;
        int quietTicks = 0;

        LARGE_INTEGER qpcFreq{};
        QueryPerformanceFrequency(&qpcFreq);
        LARGE_INTEGER lastPresentQpc{};
        QueryPerformanceCounter(&lastPresentQpc);

        while (rt->threadRunning) {
            IMFMediaEngine* eng = nullptr;
            IDXGISwapChain1* sw = nullptr;
            {
                std::lock_guard<std::mutex> guard(rt->mediaMutex);
                eng = rt->engine;
                sw = rt->swapChain;
                if (eng) eng->AddRef();
                if (sw) sw->AddRef();
            }
            if (rt->paused || g_manualPaused || !eng || !sw) {
                if (eng) eng->Release();
                if (sw) sw->Release();
                Sleep(80);
                QueryPerformanceCounter(&lastPresentQpc);
                continue;
            }

            LONGLONG pts = 0;
            HRESULT hr = eng->OnVideoStreamTick(&pts);
            if (hr == S_OK) {
                quietTicks = 0;
                if (pts != lastPts) {
                    int fps = rt->targetFps.load();
                    if (fps != 60) fps = 30;
                    LONGLONG intervalQpc = qpcFreq.QuadPart / fps;

                    LARGE_INTEGER nowQpc{};
                    QueryPerformanceCounter(&nowQpc);

                    if (nowQpc.QuadPart - lastPresentQpc.QuadPart >= intervalQpc) {
                        lastPts = pts;
                        lastPresentQpc = nowQpc;

                        IDXGISurface* surf = nullptr;
                        HRESULT hrBuf = sw->GetBuffer(0, IID_PPV_ARGS(&surf));
                        if (FAILED(hrBuf) || !surf) {
                            if (!bufWarned) {
                                bufWarned = true;
                                log::error("swapchain GetBuffer failed (hr = 0x" + toHex(hrBuf) + ")");
                            }
                            if (surf) surf->Release();
                            eng->Release();
                            sw->Release();
                            Sleep(8);
                            continue;
                        }
                        MFVideoNormalizedRect srcR = {0.0f, 0.0f, 1.0f, 1.0f};
                        RECT dstR = {0, 0, w, h};
                        MFARGB bg = {0, 0, 0, 255};
                        HRESULT hrFrame = eng->TransferVideoFrame(surf, &srcR, &dstR, &bg);
                        surf->Release();
                        if (FAILED(hrFrame)) {
                            if (!transferWarned) {
                                transferWarned = true;
                                log::error("TransferVideoFrame failed (hr = 0x" + toHex(hrFrame) + ")");
                            }
                            eng->Release();
                            sw->Release();
                            Sleep(8);
                            continue;
                        }
                        HRESULT hrPresent = sw->Present(1, 0);
                        if (FAILED(hrPresent)) {
                            if (!presentWarned) {
                                presentWarned = true;
                                log::error("swapchain Present failed (hr = 0x" + toHex(hrPresent) + ")");
                            }
                            eng->Release();
                            sw->Release();
                            Sleep(8);
                            continue;
                        }

                        if (!firstFrameLogged) {
                            firstFrameLogged = true;
                            log::info("first video frame presented on desktop");
                            trimWorkingSet();
                        }
                        eng->Release();
                        sw->Release();
                    } else {
                        eng->Release();
                        sw->Release();
                        Sleep(1);
                    }
                } else {
                    eng->Release();
                    sw->Release();
                    Sleep(1);
                }
            } else {
                eng->Release();
                sw->Release();
                quietTicks++;
                if (quietTicks == 900 && !tickWarned) {
                    tickWarned = true;
                    log::warn("video stream idle, no fresh frame after 900 ticks");
                }
                Sleep(2);
            }
        }
        timeEndPeriod(1);
        CoUninitialize();
    });

    return true;
}

void applyConfigToState(HINSTANCE inst, HWND host, EngineState& st, const Config& cfg) {
    if (!host || !IsWindow(host)) {
        host = findWallpaperHost(40, 50);
        if (!host) {
            g_workerW = nullptr;
            hideStrayWorkerWs();
            host = g_progman;
        }
    }
    bool hostChanged = g_activeHost && host != g_activeHost;
    auto desiredPanes = buildPanes(cfg);
    if (desiredPanes.empty()) {
        log::error("no playable wallpaper configured (file missing or monitor unplugged)");
    }
    st.muted = cfg.muteByDefault;
    st.lowEndMode = cfg.lowEndMode;
    st.playbackSpeed = (float)cfg.playbackSpeed;
    st.targetFps = cfg.targetFps == 60 ? 60 : 30;
    for (auto p : st.panes) p->targetFps.store(st.targetFps);

    bool needsFullRebuild = (st.panes.size() != desiredPanes.size()) || hostChanged;
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
    g_activeHost = host;
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
        log::error("MFStartup failed (hr = 0x" + toHex(hr) + ")");
        CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }
    if (!systemCanDecodeH264()) {
        log::error("no H264 video decoder found (install Media Feature Pack, GPU driver, or HEVC extension)");
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
        makeIconsTransparent();
        g_workerW = nullptr;
        hideStrayWorkerWs();
        log::warn("fallback to Progman host: 0x" + toHex(reinterpret_cast<uintptr_t>(g_progman)));
        host = g_progman;
    }
    EngineState st;
    g_currentEngineState = &st;
    g_activeHost = nullptr;

    applyConfigToState(inst, host, st, cfg);
    if (st.panes.empty()) {
        log::error("engine started with no video panes (missing file, bad codec, or missing GPU driver)");
    } else {
        log::info("panes active: " + std::to_string(st.panes.size()));
    }

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
    if (waitTimeout > 300) waitTimeout = 150;

    DWORD lastPauseTick = 0;
    bool wasOccluded = false;
    DWORD currentPid = GetCurrentProcessId();
    DWORD pollCount = 0;
    HWND lastFg = nullptr;
    bool cachedCovering = false;

    HANDLE waitHandles[2] = { stopEvent, reloadEvent };

    while (running) {
        DWORD waitRes = MsgWaitForMultipleObjects(2, waitHandles, FALSE, waitTimeout, QS_ALLINPUT);
        if (waitRes == WAIT_OBJECT_0) {
            log::info("stopEvent signaled, shutting down engine");
            running = false;
            break;
        }
        if (waitRes == WAIT_OBJECT_0 + 1) {
            log::info("reloadEvent signaled, reloading config");
            g_currentCfg = Config::load();
            if (!g_workerW) {
                HWND upgrade = findWallpaperHost(20, 50);
                if (upgrade) host = upgrade;
            }
            applyConfigToState(inst, host, st, g_currentCfg);
            log::info("panes active: " + std::to_string(st.panes.size()) +
                      ", host valid: " + std::string(IsWindow(host) ? "yes" : "no"));
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                log::info("WM_QUIT received, shutting down engine");
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
                    if (!isDesktopWindow(fw)) {
                        if (g_currentCfg.pauseUnlessDesktop) {
                            occluded = true;
                        } else if (isWindowCoveringScreen(fw, g_currentCfg.pauseWhenMaximized, g_currentCfg.pauseOnFullscreen)) {
                            occluded = true;
                        }
                    }
                }
            }

            if (!occluded && (g_currentCfg.pauseWhenMaximized || g_currentCfg.pauseOnFullscreen)) {
                if (fw != lastFg || (pollCount % 4) == 0) {
                    OcclusionEnumContext ctx;
                    ctx.currentPid = currentPid;
                    ctx.checkMaximized = g_currentCfg.pauseWhenMaximized;
                    ctx.checkFullscreen = g_currentCfg.pauseOnFullscreen;
                    EnumWindows(EnumWindowsOcclusionCallback, reinterpret_cast<LPARAM>(&ctx));
                    cachedCovering = ctx.foundCovering;
                }
                if (cachedCovering) {
                    occluded = true;
                }
            }
            lastFg = fw;
        }
        pollCount++;

        if (occluded != wasOccluded) {
            wasOccluded = occluded;
            if (occluded) {
                lastPauseTick = GetTickCount();
                log::info(g_manualPaused.load() ? "wallpaper paused by user" : "wallpaper auto-paused (covering window or battery)");
                trimMemory();
            } else {
                log::info("wallpaper resumed on desktop");
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

        DWORD nowTick = GetTickCount();
        DWORD deepMs = g_currentCfg.occlusionTimeoutSec > 0 ? (DWORD)g_currentCfg.occlusionTimeoutSec * 1000 : 8000;
        for (auto p : st.panes) {
            if (occluded && !p->asleep.load() && nowTick - lastPauseTick >= deepMs) {
                p->unloadMedia();
            } else if (!occluded && p->asleep.load() && nowTick - p->lastLoadAttempt >= 3000) {
                p->lastLoadAttempt = nowTick;
                if (p->loadMedia(st)) trimMemory();
            }
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
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kEngineMutexName);
    if (mutex) {
        CloseHandle(mutex);
        return true;
    }
    return false;
}

}
