#include "net/updater.h"
#include "net/http.h"
#include "util/json.h"
#include "util/str.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace motion::updater {

namespace {

std::vector<int> parseVersionNumbers(const std::string& ver) {
    std::vector<int> nums;
    size_t i = 0;
    while (i < ver.size()) {
        while (i < ver.size() && (ver[i] < '0' || ver[i] > '9')) ++i;
        if (i >= ver.size()) break;
        int val = 0;
        while (i < ver.size() && ver[i] >= '0' && ver[i] <= '9') {
            val = val * 10 + (ver[i] - '0');
            ++i;
        }
        nums.push_back(val);
    }
    while (nums.size() < 3) nums.push_back(0);
    return nums;
}

std::wstring currentExecutablePath() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::string extractChangelogFromReleaseHtml(const std::string& html) {
    size_t bodyStart = html.find("class=\"markdown-body");
    if (bodyStart != std::string::npos) {
        size_t contentStart = html.find('>', bodyStart);
        if (contentStart != std::string::npos) {
            ++contentStart;
            size_t contentEnd = html.find("</div>", contentStart);
            if (contentEnd == std::string::npos) contentEnd = html.size();
            std::string raw = html.substr(contentStart, contentEnd - contentStart);
            std::string text;
            bool inTag = false;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '<') {
                    inTag = true;
                    if (i + 4 <= raw.size() && raw.substr(i, 4) == "<li>") {
                        text += "\n  • ";
                    } else if (i + 5 <= raw.size() && (raw.substr(i, 5) == "</h2>" || raw.substr(i, 5) == "</h3>" || raw.substr(i, 4) == "<br>")) {
                        text += "\n";
                    }
                } else if (raw[i] == '>') {
                    inTag = false;
                } else if (!inTag) {
                    if (raw[i] == '\r' || raw[i] == '\n') {
                        if (!text.empty() && text.back() != '\n') text += '\n';
                    } else {
                        text += raw[i];
                    }
                }
            }
            std::string cleaned = motion::htmlDecode(trim(text));
            if (!cleaned.empty()) return cleaned;
        }
    }

    size_t descPos = html.find("property=\"og:description\" content=\"");
    if (descPos != std::string::npos) {
        size_t ds = descPos + 35;
        size_t de = html.find('"', ds);
        if (de != std::string::npos) {
            return motion::htmlDecode(html.substr(ds, de - ds));
        }
    }
    return "";
}

}

bool isVersionNewer(const std::string& currentVer, const std::string& latestVer) {
    if (currentVer.empty() || currentVer == "dev" || latestVer.empty()) return false;

    auto cur = parseVersionNumbers(currentVer);
    auto lat = parseVersionNumbers(latestVer);

    for (size_t i = 0; i < 3; ++i) {
        if (lat[i] > cur[i]) return true;
        if (lat[i] < cur[i]) return false;
    }
    return false;
}

bool checkUpdate(const std::string& repo,
                 const std::string& currentVersion,
                 UpdateInfo& outInfo,
                 std::string& err) {
    outInfo = UpdateInfo{};
    outInfo.currentVersion = currentVersion;

    std::string repoTarget = repo.empty() ? "RealTenzo/motioncli" : repo;
    std::wstring url = L"https://raw.githubusercontent.com/" + widen(repoTarget) + L"/refs/heads/main/version.json";

    std::string jsonStr;
    if (!http::getString(url, jsonStr, err)) {
        url = L"https://raw.githubusercontent.com/" + widen(repoTarget) + L"/main/version.json";
        if (!http::getString(url, jsonStr, err)) {
            return false;
        }
    }

    Json root = Json::parse(jsonStr);
    if (root.isNull()) {
        err = "Invalid version.json format";
        return false;
    }

    std::string ver = root["version"].asString();
    if (ver.empty()) {
        err = "version field missing in version.json";
        return false;
    }

    std::string cleanVer = ver;
    if (!cleanVer.empty() && (cleanVer[0] == 'v' || cleanVer[0] == 'V')) {
        cleanVer = cleanVer.substr(1);
    }

    outInfo.latestVersion = cleanVer;
    outInfo.downloadUrl = root["download_url"].asString();
    if (outInfo.downloadUrl.empty()) {
        outInfo.downloadUrl = "https://github.com/" + repoTarget + "/releases/download/v" + cleanVer + "/motioncli_portable.exe";
    }
    outInfo.htmlUrl = "https://github.com/" + repoTarget + "/releases/tag/v" + cleanVer;
    outInfo.title = "Motion CLI v" + cleanVer;

    std::string relHtml, relErr;
    if (http::getString(widen(outInfo.htmlUrl), relHtml, relErr)) {
        outInfo.changelog = extractChangelogFromReleaseHtml(relHtml);
    }

    outInfo.isNewer = isVersionNewer(currentVersion, cleanVer);

    return true;
}

bool downloadUpdate(const std::string& downloadUrl,
                    const std::wstring& destPath,
                    void(*onProgress)(int, void*),
                    void* progressCtx,
                    std::string& err) {
    if (downloadUrl.empty()) {
        err = "No download URL available";
        return false;
    }

    struct ProgressThunk {
        void(*cb)(int, void*);
        void* ctx;
        static void bridge(unsigned long long received, unsigned long long total, void* ctx) {
            auto* self = (ProgressThunk*)ctx;
            if (!self->cb) return;
            if (total > 0) {
                int p = (int)((received * 100) / total);
                if (p > 100) p = 100;
                self->cb(p, self->ctx);
            } else {
                self->cb(-1, self->ctx);
            }
        }
    };

    ProgressThunk thunk{ onProgress, progressCtx };
    bool ok = http::downloadFile(widen(downloadUrl), destPath, ProgressThunk::bridge, &thunk, err);
    if (!ok) {
        std::string fallbackUrl = downloadUrl;
        size_t lastSlash = fallbackUrl.find_last_of('/');
        if (lastSlash != std::string::npos) {
            fallbackUrl = fallbackUrl.substr(0, lastSlash + 1) + "motioncli.exe";
            ok = http::downloadFile(widen(fallbackUrl), destPath, ProgressThunk::bridge, &thunk, err);
        }
    }
    return ok;
}

bool applyUpdateAndRestart(const std::wstring& downloadedExePath,
                           std::string& err) {
    std::wstring currentExe = currentExecutablePath();
    if (currentExe.empty()) {
        err = "Could not locate current executable";
        return false;
    }

    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring scriptPath = std::wstring(tempDir) + L"motioncli_apply_update.cmd";

    std::ofstream out(scriptPath.c_str(), std::ios::trunc);
    if (!out) {
        err = "Could not create update script";
        return false;
    }

    out << "@echo off\r\n";
    out << "setlocal enabledelayedexpansion\r\n";
    out << "timeout /t 1 /nobreak >nul\r\n";
    out << ":wait_loop\r\n";
    out << "tasklist /fi \"imagename eq motioncli.exe\" 2>nul | find /i \"motioncli.exe\" >nul\r\n";
    out << "if %errorlevel%==0 (\r\n";
    out << "    timeout /t 1 /nobreak >nul\r\n";
    out << "    goto wait_loop\r\n";
    out << ")\r\n";
    out << "copy /y \"" << narrow(downloadedExePath) << "\" \"" << narrow(currentExe) << "\" >nul\r\n";
    out << "if exist \"" << narrow(downloadedExePath) << "\" del /f /q \"" << narrow(downloadedExePath) << "\" >nul\r\n";
    out << "start \"\" \"" << narrow(currentExe) << "\"\r\n";
    out << "del \"%~f0\" >nul\r\n";
    out.close();

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpFile = scriptPath.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_FLAG_NO_UI;

    if (!ShellExecuteExW(&sei)) {
        err = "Failed to launch update process";
        DeleteFileW(scriptPath.c_str());
        return false;
    }

    return true;
}

}
