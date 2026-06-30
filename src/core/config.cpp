#include "core/config.h"
#include "util/json.h"
#include "util/str.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

namespace motion {

namespace {

bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

std::wstring appDataRoot() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        result = path;
    }
    if (path) CoTaskMemFree(path);
    return result;
}

std::wstring oldDataDir() {
    std::wstring root = appDataRoot();
    return root.empty() ? std::wstring() : root + L"\\MotionCLI";
}

void copyFileIfMissing(const std::wstring& src, const std::wstring& dest) {
    if (src.empty() || dest.empty() || !fileExists(src) || fileExists(dest)) return;
    CopyFileW(src.c_str(), dest.c_str(), TRUE);
}

void copyWallpaperFilesIfNeeded(const std::wstring& oldDir, const std::wstring& newDir) {
    if (oldDir.empty()) return;
    CreateDirectoryW(newDir.c_str(), nullptr);

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((oldDir + L"\\wallpapers\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring src = oldDir + L"\\wallpapers\\" + fd.cFileName;
        std::wstring dest = newDir + L"\\" + fd.cFileName;
        copyFileIfMissing(src, dest);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void migratePortableData() {
    static bool done = false;
    if (done) return;
    done = true;

    std::wstring root = exeDir();
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\wallpapers").c_str(), nullptr);
    CreateDirectoryW((root + L"\\logs").c_str(), nullptr);

    std::wstring oldDir = oldDataDir();
    if (oldDir.empty()) return;

    copyFileIfMissing(oldDir + L"\\config.json", root + L"\\config.json");
    copyFileIfMissing(oldDir + L"\\library.json", root + L"\\library.json");
    copyWallpaperFilesIfNeeded(oldDir, root + L"\\wallpapers");
}

std::wstring normalizeMediaPath(const std::wstring& path) {
    if (path.empty() || !fileExists(path)) return path;

    std::wstring portableDir = Config::wallpapersDir();
    std::wstring lowerPath = path;
    std::wstring lowerDir = portableDir;
    CharLowerBuffW(lowerPath.data(), (DWORD)lowerPath.size());
    CharLowerBuffW(lowerDir.data(), (DWORD)lowerDir.size());
    if (lowerPath.rfind(lowerDir + L"\\", 0) == 0) return path;

    size_t slash = path.find_last_of(L"\\/");
    std::wstring fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::wstring dest = portableDir + L"\\" + fileName;
    copyFileIfMissing(path, dest);
    return fileExists(dest) ? dest : path;
}

}

std::wstring Config::dataDir() {
    migratePortableData();
    return exeDir();
}

std::wstring Config::wallpapersDir() {
    std::wstring dir = dataDir() + L"\\wallpapers";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Config::configPath() {
    return dataDir() + L"\\config.json";
}

std::wstring Config::userLibraryPath() {
    return dataDir() + L"\\library.json";
}

Config Config::load() {
    Config cfg;

    std::ifstream in(configPath(), std::ios::binary);
    if (!in) return cfg;

    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return cfg;

    auto qualityFrom = [](const std::string& s) {
        if (s == "high")   return Quality::High;
        if (s == "medium") return Quality::Medium;
        if (s == "low")    return Quality::Low;
        return Quality::Auto;
    };

    try {
        Json j = Json::parse(text);
        if (j["catalogUrl"].isString())   cfg.catalogUrl = widen(j["catalogUrl"].asString());
        if (j["pexelsApiKey"].isString()) cfg.pexelsApiKey = widen(j["pexelsApiKey"].asString());
        cfg.muteByDefault    = j["muteByDefault"].asBool(false);
        cfg.firstRun         = j["firstRun"].asBool(true);
        cfg.mode = (j["mode"].asString("span") == "per-monitor")
                       ? WallpaperMode::PerMonitor : WallpaperMode::Span;
        cfg.quality            = qualityFrom(j["quality"].asString("auto"));
        cfg.pauseOnFullscreen  = j["pauseOnFullscreen"].asBool(true);
        cfg.pauseWhenMaximized = j["pauseWhenMaximized"].asBool(true);
        cfg.pauseUnlessDesktop = j["pauseUnlessDesktop"].asBool(false);
        cfg.pauseOnBattery     = j["pauseOnBattery"].asBool(false);
        cfg.lowEndMode         = j["lowEndMode"].asBool(false);
        cfg.occlusionTimeoutSec = j["occlusionTimeoutSec"].asInt(0);
        if (cfg.occlusionTimeoutSec < 0 || cfg.occlusionTimeoutSec > 300) cfg.occlusionTimeoutSec = 0;
        cfg.occlusionPollMs    = j["occlusionPollMs"].asInt(150);
        if (cfg.occlusionPollMs < 50 || cfg.occlusionPollMs > 5000) cfg.occlusionPollMs = 150;
        cfg.occlusionGraceMs   = j["occlusionGraceMs"].asInt(0);
        if (cfg.occlusionGraceMs < 0 || cfg.occlusionGraceMs > 10000) cfg.occlusionGraceMs = 0;
        cfg.playbackSpeed      = j["playbackSpeed"].asNumber(1.0);
        if (cfg.playbackSpeed < 0.25 || cfg.playbackSpeed > 4.0) cfg.playbackSpeed = 1.0;
        cfg.moeCategory        = j["moeCategory"].asString("");
        cfg.libraryCount       = j["libraryCount"].asInt(24);
        if (cfg.libraryCount < 6 || cfg.libraryCount > 96) cfg.libraryCount = 24;
        cfg.currentWallpaperId = j["currentWallpaperId"].asString();
        if (j["currentMediaPath"].isString())
            cfg.currentMediaPath = widen(j["currentMediaPath"].asString());
        cfg.enginePid = (unsigned long)j["enginePid"].asNumber(0);

        const Json& mon = j["monitors"];
        if (mon.isObject())
            for (const auto& kv : mon.object)
                if (kv.second.isString())
                    cfg.monitorAssignments[kv.first] = widen(kv.second.asString());

        bool normalized = false;
        std::wstring media = normalizeMediaPath(cfg.currentMediaPath);
        if (media != cfg.currentMediaPath) {
            cfg.currentMediaPath = media;
            normalized = true;
        }
        for (auto& kv : cfg.monitorAssignments) {
            std::wstring monMedia = normalizeMediaPath(kv.second);
            if (monMedia != kv.second) {
                kv.second = monMedia;
                normalized = true;
            }
        }
        if (normalized) cfg.save();
    } catch (...) {
    }

    return cfg;
}

void Config::save() const {
    const char* qualityStr = quality == Quality::High ? "high"
                           : quality == Quality::Medium ? "medium"
                           : quality == Quality::Low ? "low" : "auto";

    Json j = Json::makeObject();
    j.set("catalogUrl",         Json::makeString(narrow(catalogUrl)));
    j.set("pexelsApiKey",       Json::makeString(narrow(pexelsApiKey)));
    j.set("muteByDefault",      Json::makeBool(muteByDefault));
    j.set("firstRun",           Json::makeBool(firstRun));
    j.set("mode",               Json::makeString(mode == WallpaperMode::PerMonitor
                                                     ? "per-monitor" : "span"));
    j.set("quality",            Json::makeString(qualityStr));
    j.set("pauseOnFullscreen",  Json::makeBool(pauseOnFullscreen));
    j.set("pauseWhenMaximized", Json::makeBool(pauseWhenMaximized));
    j.set("pauseUnlessDesktop", Json::makeBool(pauseUnlessDesktop));
    j.set("pauseOnBattery",     Json::makeBool(pauseOnBattery));
    j.set("lowEndMode",         Json::makeBool(lowEndMode));
    j.set("occlusionTimeoutSec", Json::makeNumber((double)occlusionTimeoutSec));
    j.set("occlusionPollMs",    Json::makeNumber((double)occlusionPollMs));
    j.set("occlusionGraceMs",   Json::makeNumber((double)occlusionGraceMs));
    j.set("playbackSpeed",      Json::makeNumber(playbackSpeed));
    j.set("moeCategory",        Json::makeString(moeCategory));
    j.set("libraryCount",       Json::makeNumber((double)libraryCount));
    j.set("currentWallpaperId", Json::makeString(currentWallpaperId));
    j.set("currentMediaPath",   Json::makeString(narrow(currentMediaPath)));
    j.set("enginePid",          Json::makeNumber((double)enginePid));

    Json monitors = Json::makeObject();
    for (const auto& kv : monitorAssignments)
        monitors.set(kv.first, Json::makeString(narrow(kv.second)));
    j.set("monitors", std::move(monitors));

    std::ofstream out(configPath(), std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

}
