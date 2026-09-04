#include "app/app.h"
#include "core/wallpaper.h"
#include "util/log.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include <string>
#include <vector>

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        LocalFree(argv);
    }

    bool isEngine = false;
    for (const auto& a : args) {
        if (a == L"--render" || a == L"--startup") {
            isEngine = true;
            break;
        }
    }

    motion::log::init(!isEngine);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t selfExe[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, selfExe, MAX_PATH)) {
        std::wstring oldExe = std::wstring(selfExe) + L".old";
        DeleteFileW(oldExe.c_str());
    }

    if (isEngine) {
        return motion::runEngineFromConfig();
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$", "r", stdin);

    motion::App app;
    int ret = app.run();

    FreeConsole();
    return ret;
}
