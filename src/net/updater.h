#pragma once
#include <string>

namespace motion::updater {

struct UpdateInfo {
    std::string currentVersion;
    std::string latestVersion;
    std::string title;
    std::string changelog;
    std::string downloadUrl;
    std::string htmlUrl;
    bool isNewer = false;
};

bool isVersionNewer(const std::string& currentVer, const std::string& latestVer);

bool checkUpdate(const std::string& repo,
                 const std::string& currentVersion,
                 UpdateInfo& outInfo,
                 std::string& err);

bool downloadUpdate(const std::string& downloadUrl,
                    const std::wstring& destPath,
                    void(*onProgress)(int, void*),
                    void* progressCtx,
                    std::string& err);

bool applyUpdateAndRestart(const std::wstring& downloadedExePath,
                           std::string& err);

}
