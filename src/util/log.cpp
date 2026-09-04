#include "util/log.h"
#include "core/config.h"
#include "core/hardware.h"
#include "util/str.h"

#include <windows.h>
#include <fstream>
#include <mutex>
#include <cstdio>

namespace motion::log {

namespace {

std::mutex s_mutex;
bool s_initialized = false;

std::string getRegString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
    wchar_t buf[256] = {0};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    std::string res;
    if (RegQueryValueExW(hKey, value, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
        if (type == REG_SZ) {
            res = narrow(buf);
        }
    }
    RegCloseKey(hKey);
    return res;
}

DWORD getRegDword(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return 0;
    DWORD val = 0;
    DWORD sz = sizeof(val);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, value, nullptr, &type, (LPBYTE)&val, &sz) != ERROR_SUCCESS) {
        val = 0;
    }
    RegCloseKey(hKey);
    return val;
}

void appendToFile(const std::wstring& path, const std::string& line) {
    if (path.empty()) return;
    std::ofstream out(path, std::ios::out | std::ios::app | std::ios::binary);
    if (out) {
        out << line;
        out.flush();
    }
}

void writeFormatted(const char* level, const std::string& msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char timeBuf[64];
    _snprintf_s(timeBuf, sizeof(timeBuf), _TRUNCATE,
                "[%02d:%02d:%02d.%03d] [%s] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                level ? level : "info");

    std::string full = std::string(timeBuf) + msg + "\r\n";
    OutputDebugStringA(full.c_str());

    std::wstring p1 = primaryLogPath();
    std::wstring p2 = appDataLogPath();
    appendToFile(p1, full);
    if (!p2.empty() && p2 != p1) {
        appendToFile(p2, full);
    }
}

}

std::wstring primaryLogPath() {
    return Config::logPath();
}

std::wstring appDataLogPath() {
    return Config::appDataLogPath();
}

void write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_initialized) {
        init();
    }
    writeFormatted(level, msg);
}

void info(const std::string& msg) {
    write("info", msg);
}

void warn(const std::string& msg) {
    write("warn", msg);
}

void error(const std::string& msg) {
    write("err", msg);
}

void init(bool truncate) {
    if (s_initialized) return;
    s_initialized = true;

    std::wstring p1 = primaryLogPath();
    std::wstring p2 = appDataLogPath();
    if (truncate) {
        if (!p1.empty()) {
            std::ofstream out(p1, std::ios::out | std::ios::trunc | std::ios::binary);
        }
        if (!p2.empty() && p2 != p1) {
            std::ofstream out(p2, std::ios::out | std::ios::trunc | std::ios::binary);
        }
    }

    std::string prod = getRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    std::string disp = getRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    std::string build = getRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
    if (build.empty()) build = getRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    DWORD ubr = getRegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR");

    std::string os = prod;
    if (!disp.empty()) os += " " + disp;
    if (!build.empty()) {
        os += " build " + build;
        if (ubr > 0) os += "." + std::to_string(ubr);
    }

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    HwInfo hw = scanHardware();

    writeFormatted("init", truncate ? "=== motioncli v" MOTION_VERSION " debg log ===" : "=== motioncli engne attacht ===");
    writeFormatted("init", "winver: " + os);
    writeFormatted("init", "exe: " + narrow(exePath));
    writeFormatted("init", "datadir: " + narrow(Config::dataDir()));
    char hwBuf[256];
    _snprintf_s(hwBuf, sizeof(hwBuf), _TRUNCATE,
                "hw: %s, cores=%d, ram=%dgb, vram=%dmb, dedigpu=%s",
                tierName(hw), hw.cores, hw.ramGB, hw.vramMB, hw.dedicatedGpu ? "true" : "flase");
    writeFormatted("init", hwBuf);
}

}
