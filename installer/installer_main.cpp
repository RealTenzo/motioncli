#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

#define IDI_APPICON 101

namespace {

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

bool httpDownloadFile(const std::wstring& fullUrl, const std::wstring& destPath) {
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

    // Handle 301/302 Redirect
    if (statusCode == 301 || statusCode == 302 || statusCode == 307 || statusCode == 308) {
        wchar_t loc[2048] = {0};
        DWORD locSize = sizeof(loc);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &locSize, WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return httpDownloadFile(loc, destPath);
        }
    }

    bool success = false;
    if (bResults && statusCode == 200) {
        HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD dwSize = 0;
            success = true;
            do {
                dwSize = 0;
                if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                    std::vector<char> buf(dwSize);
                    DWORD dwDownloaded = 0;
                    if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                        DWORD dwWritten = 0;
                        WriteFile(hFile, buf.data(), dwDownloaded, &dwWritten, nullptr);
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

int doUninstall(bool silent) {
    killRunningProcess(L"motioncli.exe");

    std::wstring localAppData = getSpecialFolder(CSIDL_LOCAL_APPDATA);
    std::wstring installDir = localAppData + L"\\Programs\\MotionCLI";
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

    if (!silent) {
        MessageBoxW(nullptr, L"Motion CLI has been successfully uninstalled from your system.", L"Motion CLI Uninstall", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR pCmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring cmd = pCmdLine ? pCmdLine : L"";
    bool silent = (cmd.find(L"/S") != std::string::npos || cmd.find(L"/s") != std::string::npos);
    bool uninstall = (cmd.find(L"/uninstall") != std::string::npos || cmd.find(L"/u") != std::string::npos || cmd.find(L"-u") != std::string::npos);

    if (uninstall) {
        int res = doUninstall(silent);
        CoUninitialize();
        return res;
    }

    killRunningProcess(L"motioncli.exe");

    std::wstring localAppData = getSpecialFolder(CSIDL_LOCAL_APPDATA);
    if (localAppData.empty()) {
        if (!silent) MessageBoxW(nullptr, L"Failed to determine local app data path.", L"Motion CLI Setup", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    std::wstring installDir = localAppData + L"\\Programs\\MotionCLI";
    SHCreateDirectoryExW(nullptr, installDir.c_str(), nullptr);

    std::wstring destExe = installDir + L"\\motioncli.exe";
    std::wstring destUninst = installDir + L"\\uninstall.exe";

    // 1. Fetch version.json from GitHub
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

    // Convert downloadUrl to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, downloadUrl.c_str(), -1, nullptr, 0);
    std::wstring wDownloadUrl(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, downloadUrl.c_str(), -1, &wDownloadUrl[0], wlen);
    if (!wDownloadUrl.empty() && wDownloadUrl.back() == 0) wDownloadUrl.pop_back();

    // 2. Download executable
    bool downloaded = httpDownloadFile(wDownloadUrl, destExe);
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
        if (!silent) MessageBoxW(nullptr, L"Failed to download Motion CLI from GitHub. Please check your internet connection.", L"Motion CLI Setup", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    wchar_t thisInstallerPath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, thisInstallerPath, MAX_PATH);
    CopyFileW(thisInstallerPath, destUninst.c_str(), FALSE);

    std::wstring startMenuShortcut = getSpecialFolder(CSIDL_PROGRAMS) + L"\\Motion CLI.lnk";
    std::wstring desktopShortcut = getSpecialFolder(CSIDL_DESKTOP) + L"\\Motion CLI.lnk";

    createShortcut(destExe, startMenuShortcut, L"Motion CLI - Live Wallpaper Engine");
    createShortcut(destExe, desktopShortcut, L"Motion CLI - Live Wallpaper Engine");
    addToUserPath(installDir);

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MotionCLI",
                        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        const wchar_t* dName = L"Motion CLI";
        int vwlen = MultiByteToWideChar(CP_UTF8, 0, versionStr.c_str(), -1, nullptr, 0);
        std::wstring wVer(vwlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, versionStr.c_str(), -1, &wVer[0], vwlen);
        if (!wVer.empty() && wVer.back() == 0) wVer.pop_back();

        const wchar_t* pub = L"tenzo";
        std::wstring uninstCmd = L"\"" + destUninst + L"\" /uninstall";
        std::wstring iconPath = destExe + L",0";

        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)dName, (DWORD)((wcslen(dName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayVersion", 0, REG_SZ, (const BYTE*)wVer.c_str(), (DWORD)((wVer.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"Publisher", 0, REG_SZ, (const BYTE*)pub, (DWORD)((wcslen(pub) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ, (const BYTE*)iconPath.c_str(), (DWORD)((iconPath.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ, (const BYTE*)uninstCmd.c_str(), (DWORD)((uninstCmd.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ, (const BYTE*)installDir.c_str(), (DWORD)((installDir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }

    if (!silent) {
        int ans = MessageBoxW(nullptr,
            L"Motion CLI has been successfully installed!\n\n"
            L"• Added to Start Menu & Desktop\n"
            L"• Added 'motioncli' command to PATH\n\n"
            L"Would you like to launch Motion CLI now?",
            L"Motion CLI Setup",
            MB_YESNO | MB_ICONINFORMATION);
        if (ans == IDYES) {
            ShellExecuteW(nullptr, L"open", destExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    CoUninitialize();
    return 0;
}
