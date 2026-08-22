#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include <iostream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

#define IDI_APPICON 101

namespace {

namespace color {
    constexpr const char* reset      = "\x1b[0m";
    constexpr const char* bold       = "\x1b[1m";
    constexpr const char* dim        = "\x1b[2m";
    constexpr const char* red        = "\x1b[31m";
    constexpr const char* green      = "\x1b[32m";
    constexpr const char* yellow     = "\x1b[33m";
    constexpr const char* blue       = "\x1b[34m";
    constexpr const char* magenta    = "\x1b[35m";
    constexpr const char* cyan       = "\x1b[36m";
    constexpr const char* brightCyan = "\x1b[96m";
    constexpr const char* gray       = "\x1b[90m";
    constexpr const char* invert     = "\x1b[7m";
}

void initTerminal() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Motion CLI - Terminal Setup");
}

void clearScreen() {
    std::cout << "\x1b[2J\x1b[H";
}

void hideCursor() {
    std::cout << "\x1b[?25l";
}

void showCursor() {
    std::cout << "\x1b[?25h";
}

void drawBanner() {
    static const char* art[] = {
        "  ███╗   ███╗ ██████╗ ████████╗██╗ ██████╗ ███╗   ██╗",
        "  ████╗ ████║██╔═══██╗╚══██╔══╝██║██╔═══██╗████╗  ██║",
        "  ██╔████╔██║██║   ██║   ██║   ██║██║   ██║██╔██╗ ██║",
        "  ██║╚██╔╝██║██║   ██║   ██║   ██║██║   ██║██║╚██╗██║",
        "  ██║ ╚═╝ ██║╚██████╔╝   ██║   ██║╚██████╔╝██║ ╚████║",
        "  ╚═╝     ╚═╝ ╚═════╝    ╚═╝   ╚═╝ ╚═════╝ ╚═╝  ╚═══╝",
    };
    std::cout << color::brightCyan;
    for (const char* l : art) std::cout << l << "\n";
    std::cout << color::reset;
    std::cout << color::gray << "        live wallpaper cli  ·  Interactive Installer\n" << color::reset << "\n";
}

void killRunningProcess(const wchar_t* processName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName) == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    WaitForSingleObject(hProc, 1000);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

std::wstring getSpecialFolder(int csidl) {
    wchar_t path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, path))) {
        return path;
    }
    return L"";
}

bool httpGetString(const std::wstring& host, const std::wstring& path, std::string& outBody) {
    HINTERNET hSession = WinHttpOpen(L"MotionCLI-Installer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) bResults = WinHttpReceiveResponse(hRequest, nullptr);

    if (bResults) {
        DWORD dwSize = 0;
        do {
            dwSize = 0;
            if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                std::vector<char> buf(dwSize);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                    outBody.append(buf.data(), dwDownloaded);
                }
            }
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bResults && !outBody.empty();
}

bool httpDownloadFileWithProgress(const std::wstring& fullUrl, const std::wstring& destPath, void(*onProgress)(int received, int total)) {
    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(fullUrl.c_str(), 0, 0, &urlComp)) return false;

    HINTERNET hSession = WinHttpOpen(L"MotionCLI-Installer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD reqFlags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) bResults = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD statusCode = 0;
    DWORD scSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode == 301 || statusCode == 302 || statusCode == 307 || statusCode == 308) {
        wchar_t loc[2048] = {0};
        DWORD locSize = sizeof(loc);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &locSize, WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return httpDownloadFileWithProgress(loc, destPath, onProgress);
        }
    }

    bool success = false;
    if (bResults && statusCode == 200) {
        DWORD contentLength = 0;
        DWORD clSize = sizeof(contentLength);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &clSize, WINHTTP_NO_HEADER_INDEX);

        HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD dwSize = 0;
            DWORD totalReceived = 0;
            success = true;
            do {
                dwSize = 0;
                if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                    std::vector<char> buf(dwSize);
                    DWORD dwDownloaded = 0;
                    if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                        DWORD dwWritten = 0;
                        WriteFile(hFile, buf.data(), dwDownloaded, &dwWritten, nullptr);
                        totalReceived += dwDownloaded;
                        if (onProgress) onProgress(totalReceived, contentLength);
                    }
                }
            } while (dwSize > 0);
            CloseHandle(hFile);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return success;
}

std::string extractJsonField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    p = json.find(':', p);
    if (p == std::string::npos) return "";
    p = json.find('"', p);
    if (p == std::string::npos) return "";
    size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return "";
    return json.substr(p + 1, e - (p + 1));
}

bool createShortcut(const std::wstring& targetPath, const std::wstring& shortcutPath, const std::wstring& description) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
    if (SUCCEEDED(hr)) {
        psl->SetPath(targetPath.c_str());
        psl->SetDescription(description.c_str());
        std::wstring workDir = targetPath.substr(0, targetPath.find_last_of(L"\\/"));
        psl->SetWorkingDirectory(workDir.c_str());
        psl->SetIconLocation(targetPath.c_str(), 0);

        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
        if (SUCCEEDED(hr)) {
            hr = ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    return SUCCEEDED(hr);
}

void addToUserPath(const std::wstring& dirToAdd) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        wchar_t currentPath[32768] = {0};
        DWORD size = sizeof(currentPath);
        DWORD type = REG_EXPAND_SZ;
        if (RegQueryValueExW(hKey, L"Path", nullptr, &type, (LPBYTE)currentPath, &size) == ERROR_SUCCESS) {
            std::wstring pathStr(currentPath);
            if (pathStr.find(dirToAdd) == std::string::npos) {
                if (!pathStr.empty() && pathStr.back() != L';') pathStr += L";";
                pathStr += dirToAdd;
                RegSetValueExW(hKey, L"Path", 0, type, (const BYTE*)pathStr.c_str(), (DWORD)((pathStr.size() + 1) * sizeof(wchar_t)));
                SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 1000, nullptr);
            }
        } else {
            RegSetValueExW(hKey, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)dirToAdd.c_str(), (DWORD)((dirToAdd.size() + 1) * sizeof(wchar_t)));
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 1000, nullptr);
        }
        RegCloseKey(hKey);
    }
}

void removeFromUserPath(const std::wstring& dirToRemove) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        wchar_t currentPath[32768] = {0};
        DWORD size = sizeof(currentPath);
        DWORD type = REG_EXPAND_SZ;
        if (RegQueryValueExW(hKey, L"Path", nullptr, &type, (LPBYTE)currentPath, &size) == ERROR_SUCCESS) {
            std::wstring pathStr(currentPath);
            size_t pos = pathStr.find(dirToRemove);
            if (pos != std::string::npos) {
                size_t len = dirToRemove.size();
                if (pos + len < pathStr.size() && pathStr[pos + len] == L';') len++;
                else if (pos > 0 && pathStr[pos - 1] == L';') { pos--; len++; }
                pathStr.erase(pos, len);
                RegSetValueExW(hKey, L"Path", 0, type, (const BYTE*)pathStr.c_str(), (DWORD)((pathStr.size() + 1) * sizeof(wchar_t)));
                SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 1000, nullptr);
            }
        }
        RegCloseKey(hKey);
    }
}

std::wstring narrowToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

std::string wideToNarrow(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

int readKey() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD rec;
    DWORD readCount = 0;
    while (true) {
        if (ReadConsoleInputW(hIn, &rec, 1, &readCount) && readCount == 1) {
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
                WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
                char ch = rec.Event.KeyEvent.uChar.AsciiChar;
                if (vk == VK_UP) return 1001;
                if (vk == VK_DOWN) return 1002;
                if (vk == VK_RETURN) return 1003;
                if (vk == VK_ESCAPE) return 1004;
                if (ch == 'w' || ch == 'W') return 1001;
                if (ch == 's' || ch == 'S') return 1002;
                if (ch == ' ') return 1003;
                return ch;
            }
        }
    }
}

int doUninstall() {
    clearScreen();
    drawBanner();
    std::cout << color::yellow << "  Motion CLI Uninstaller\n" << color::reset;
    std::cout << color::gray << "  ──────────────────────────────────────────────────────────\n\n" << color::reset;
    std::cout << "  Are you sure you want to completely uninstall Motion CLI? (Y/N): ";

    char c;
    std::cin >> c;
    if (c != 'y' && c != 'Y') {
        std::cout << "\n  " << color::gray << "Uninstallation aborted." << color::reset << "\n";
        return 0;
    }

    std::cout << "\n  " << color::cyan << "Uninstalling..." << color::reset << "\n";
    killRunningProcess(L"motioncli.exe");

    std::wstring localAppData = getSpecialFolder(CSIDL_LOCAL_APPDATA);
    std::wstring installDir = localAppData + L"\\Programs\\MotionCLI";

    // Read custom install dir from registry if present
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MotionCLI", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t regLoc[MAX_PATH] = {0};
        DWORD size = sizeof(regLoc);
        if (RegQueryValueExW(hKey, L"InstallLocation", nullptr, nullptr, (LPBYTE)regLoc, &size) == ERROR_SUCCESS) {
            if (wcslen(regLoc) > 0) installDir = regLoc;
        }
        RegCloseKey(hKey);
    }

    std::wstring startMenu = getSpecialFolder(CSIDL_PROGRAMS) + L"\\Motion CLI.lnk";
    std::wstring desktop = getSpecialFolder(CSIDL_DESKTOP) + L"\\Motion CLI.lnk";

    DeleteFileW(startMenu.c_str());
    DeleteFileW(desktop.c_str());
    removeFromUserPath(installDir);

    std::wstring exePath = installDir + L"\\motioncli.exe";
    std::wstring uninstPath = installDir + L"\\uninstall.exe";
    DeleteFileW(exePath.c_str());
    DeleteFileW(uninstPath.c_str());

    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MotionCLI");
    RemoveDirectoryW(installDir.c_str());

    std::cout << "  " << color::green << "✓ Motion CLI has been successfully removed from your system." << color::reset << "\n\n";
    std::cout << "  Press any key to exit...";
    readKey();
    return 0;
}

}

int main(int argc, char* argv[]) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    initTerminal();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--uninstall" || arg == "-u" || arg == "/uninstall" || arg == "/u") {
            int res = doUninstall();
            CoUninitialize();
            return res;
        }
    }

    std::wstring defaultInstallDir = getSpecialFolder(CSIDL_LOCAL_APPDATA) + L"\\Programs\\MotionCLI";
    std::string installPath = wideToNarrow(defaultInstallDir);
    bool createDesktop = true;
    bool createStartMenu = true;
    bool addToPath = true;
    bool launchAfter = true;

    int selected = 0;
    const int totalOptions = 7;

    while (true) {
        clearScreen();
        hideCursor();
        drawBanner();

        std::cout << color::bold << color::brightCyan << "  Setup Configuration\n" << color::reset;
        std::cout << color::gray << "  Customize your installation options below:\n";
        std::cout << "  ──────────────────────────────────────────────────────────\n\n" << color::reset;

        auto printItem = [&](int idx, const std::string& label, const std::string& value) {
            if (selected == idx) {
                std::cout << "  " << color::brightCyan << color::bold << "❯ " << color::reset
                          << color::invert << color::brightCyan << " " << label << " " << color::reset
                          << "  " << color::yellow << value << color::reset << "\n";
            } else {
                std::cout << "    " << label << "  " << color::gray << value << color::reset << "\n";
            }
        };

        printItem(0, "Install Location   ", "[" + installPath + "]");
        printItem(1, "Desktop Shortcut   ", createDesktop ? "[✓] Enabled" : "[ ] Disabled");
        printItem(2, "Start Menu Shortcut", createStartMenu ? "[✓] Enabled" : "[ ] Disabled");
        printItem(3, "Add to PATH        ", addToPath ? "[✓] Enabled (run 'motioncli' from any terminal)" : "[ ] Disabled");
        printItem(4, "Launch on finish   ", launchAfter ? "[✓] Yes" : "[ ] No");
        std::cout << "\n";
        printItem(5, "▶ START INSTALLATION", "");
        printItem(6, "✕ Cancel", "");

        std::cout << "\n" << color::gray << "  ↑/↓ move   ⏎ toggle/edit   esc cancel" << color::reset << "\n";

        int key = readKey();
        if (key == 1001) { // UP
            selected = (selected - 1 + totalOptions) % totalOptions;
        } else if (key == 1002) { // DOWN
            selected = (selected + 1) % totalOptions;
        } else if (key == 1004) { // ESC
            clearScreen();
            showCursor();
            std::cout << "\n  Installation cancelled.\n\n";
            CoUninitialize();
            return 0;
        } else if (key == 1003) { // ENTER
            if (selected == 0) {
                showCursor();
                clearScreen();
                drawBanner();
                std::cout << color::bold << color::brightCyan << "  Edit Install Location\n\n" << color::reset;
                std::cout << "  Current path: " << color::yellow << installPath << color::reset << "\n\n";
                std::cout << "  Enter new path (or press Enter to keep): ";
                std::string newPath;
                std::getline(std::cin, newPath);
                if (!newPath.empty()) {
                    while (!newPath.empty() && (newPath.back() == '\\' || newPath.back() == '/')) newPath.pop_back();
                    installPath = newPath;
                }
            } else if (selected == 1) {
                createDesktop = !createDesktop;
            } else if (selected == 2) {
                createStartMenu = !createStartMenu;
            } else if (selected == 3) {
                addToPath = !addToPath;
            } else if (selected == 4) {
                launchAfter = !launchAfter;
            } else if (selected == 5) {
                break; // Start installation!
            } else if (selected == 6) {
                clearScreen();
                showCursor();
                std::cout << "\n  Installation cancelled.\n\n";
                CoUninitialize();
                return 0;
            }
        }
    }

    // Begin installation phase
    clearScreen();
    drawBanner();
    std::cout << color::bold << color::brightCyan << "  Installing Motion CLI\n" << color::reset;
    std::cout << color::gray << "  ──────────────────────────────────────────────────────────\n\n" << color::reset;

    std::cout << "  " << color::cyan << "[1/4] Closing running instances..." << color::reset << "\n";
    killRunningProcess(L"motioncli.exe");

    std::wstring wInstallDir = narrowToWide(installPath);
    SHCreateDirectoryExW(nullptr, wInstallDir.c_str(), nullptr);

    std::wstring destExe = wInstallDir + L"\\motioncli.exe";
    std::wstring destUninst = wInstallDir + L"\\uninstall.exe";

    std::cout << "  " << color::cyan << "[2/4] Fetching latest release info from GitHub..." << color::reset << "\n";
    std::string jsonStr;
    std::string downloadUrl;
    std::string versionStr = "1.2.1";

    if (httpGetString(L"raw.githubusercontent.com", L"/RealTenzo/motioncli/refs/heads/main/version.json", jsonStr)) {
        downloadUrl = extractJsonField(jsonStr, "download_url");
        std::string v = extractJsonField(jsonStr, "version");
        if (!v.empty()) versionStr = v;
    }

    if (downloadUrl.empty()) {
        downloadUrl = "https://github.com/RealTenzo/motioncli/releases/latest/download/motioncli_portable.exe";
    }

    std::cout << "  " << color::cyan << "[3/4] Downloading Motion CLI v" << versionStr << "..." << color::reset << "\n";

    std::wstring wDownloadUrl = narrowToWide(downloadUrl);
    auto onProgress = +[](int received, int total) {
        int pct = (total > 0) ? (int)((received * 100) / total) : -1;
        int barWidth = 24;
        int filled = (pct >= 0) ? (pct * barWidth / 100) : 0;
        std::string bar(filled, '#');
        bar.append(barWidth - filled, '.');
        std::cout << "\r    [" << color::brightCyan << bar << color::reset << "] "
                  << ((pct >= 0) ? std::to_string(pct) : "...") << "% (" << (received / 1024) << " KB)   " << std::flush;
    };

    bool downloaded = httpDownloadFileWithProgress(wDownloadUrl, destExe, onProgress);
    std::cout << "\n";

    if (!downloaded) {
        wchar_t currentExe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, currentExe, MAX_PATH);
        std::wstring currentDir = currentExe;
        size_t lastSlash = currentDir.find_last_of(L"\\/");
        if (lastSlash != std::string::npos) {
            std::wstring neighbor = currentDir.substr(0, lastSlash + 1) + L"motioncli.exe";
            if (CopyFileW(neighbor.c_str(), destExe.c_str(), FALSE)) {
                downloaded = true;
            }
        }
    }

    if (!downloaded) {
        std::cout << "\n  " << color::red << "✕ Download failed. Please check your internet connection." << color::reset << "\n\n";
        std::cout << "  Press any key to exit...";
        showCursor();
        readKey();
        CoUninitialize();
        return 1;
    }

    std::cout << "  " << color::cyan << "[4/4] Setting up shortcuts and environment..." << color::reset << "\n";

    wchar_t thisInstallerPath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, thisInstallerPath, MAX_PATH);
    CopyFileW(thisInstallerPath, destUninst.c_str(), FALSE);

    if (createStartMenu) {
        std::wstring startMenuShortcut = getSpecialFolder(CSIDL_PROGRAMS) + L"\\Motion CLI.lnk";
        createShortcut(destExe, startMenuShortcut, L"Motion CLI - Live Wallpaper Engine");
    }

    if (createDesktop) {
        std::wstring desktopShortcut = getSpecialFolder(CSIDL_DESKTOP) + L"\\Motion CLI.lnk";
        createShortcut(destExe, desktopShortcut, L"Motion CLI - Live Wallpaper Engine");
    }

    if (addToPath) {
        addToUserPath(wInstallDir);
    }

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MotionCLI",
                        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        const wchar_t* dName = L"Motion CLI";
        std::wstring wVer = narrowToWide(versionStr);
        const wchar_t* pub = L"tenzo";
        std::wstring uninstCmd = L"\"" + destUninst + L"\" --uninstall";
        std::wstring iconPath = destExe + L",0";

        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)dName, (DWORD)((wcslen(dName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayVersion", 0, REG_SZ, (const BYTE*)wVer.c_str(), (DWORD)((wVer.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"Publisher", 0, REG_SZ, (const BYTE*)pub, (DWORD)((wcslen(pub) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ, (const BYTE*)iconPath.c_str(), (DWORD)((iconPath.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ, (const BYTE*)uninstCmd.c_str(), (DWORD)((uninstCmd.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ, (const BYTE*)wInstallDir.c_str(), (DWORD)((wInstallDir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }

    clearScreen();
    drawBanner();
    std::cout << color::bold << color::green << "  ✓ Installation Complete!\n" << color::reset;
    std::cout << color::gray << "  ──────────────────────────────────────────────────────────\n\n" << color::reset;
    std::cout << "  • Version     : " << color::brightCyan << "v" << versionStr << color::reset << "\n";
    std::cout << "  • Location    : " << color::yellow << installPath << color::reset << "\n";
    if (addToPath) {
        std::cout << "  • Command     : Run " << color::brightCyan << "motioncli" << color::reset << " in any terminal\n";
    }
    std::cout << "\n";

    if (launchAfter) {
        std::cout << "  " << color::green << "Launching Motion CLI now..." << color::reset << "\n\n";
        ShellExecuteW(nullptr, L"open", destExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Sleep(800);
    } else {
        std::cout << "  Press any key to finish...";
        readKey();
    }

    showCursor();
    CoUninitialize();
    return 0;
}
