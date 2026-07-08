#pragma once
#include <map>
#include <string>

namespace motion {

enum class WallpaperMode { Span, PerMonitor };
enum class Quality { Auto, High, Medium, Low };

class Config {
public:
    std::wstring catalogUrl =
        L"https://raw.githubusercontent.com/tenzo/motioncli/main/assets/catalog.sample.json";
    std::wstring pexelsApiKey;
    std::map<std::string, std::wstring> monitorAssignments;

    std::string  currentWallpaperId;
    std::wstring currentMediaPath;
    std::string moeCategory;
    double playbackSpeed = 1.0;

    WallpaperMode mode = WallpaperMode::Span;
    Quality quality = Quality::Auto;
    int occlusionTimeoutSec = 0;
    int occlusionPollMs = 150;
    int occlusionGraceMs = 0;
    int libraryCount = 24;
    unsigned long enginePid = 0;

    bool muteByDefault = false;
    bool firstRun = true;
    bool pauseOnFullscreen = true;
    bool pauseWhenMaximized = true;
    bool pauseUnlessDesktop = false;
    bool pauseOnBattery = false;
    bool lowEndMode = false;

    static Config load();
    void save() const;

    static std::wstring dataDir();
    static std::wstring wallpapersDir();
    static std::wstring configPath();
    static std::wstring userLibraryPath();
};

}
