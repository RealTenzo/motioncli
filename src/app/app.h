#pragma once
#include "tui/terminal.h"
#include "core/config.h"
#include "core/library.h"
#include "core/wallpaper.h"
#include "core/monitors.h"
#include "core/hardware.h"

#include <string>
#include <utility>
#include <vector>

namespace motion {

class App {
public:
    App();
    int run();

private:
    void mainMenu();
    void guide();
    void help();
    void browseLibrary();
    void myWallpapers();
    void wallpaperDetail(const Wallpaper& w);
    void previewWallpaper(const Wallpaper& w);
    void deleteWallpaper(const Wallpaper& w);
    void importWallpaper();
    void perMonitorSetup();
    void activeWallpaper();
    void settings();
    void performanceSettings();
    void connectPexels();
    void autoTune(bool announce);

    int  pickWallpaper(const std::string& title, const std::string& subtitle);
    bool prepareMedia(const Wallpaper& w, std::wstring& outPath);
    bool applyWallpaper(const Wallpaper& w);
    void assignToMonitor(const Wallpaper& w);
    void exportWallpaper(const Wallpaper& w);
    bool ensureCatalog(bool forceRefresh = false);
    void notify(const std::string& title,
                const std::vector<std::pair<const char*, std::string>>& lines);

    tui::Terminal     m_term;
    Config            m_config;
    Library           m_library;
    EngineController  m_engine;
    bool              m_catalogLoaded = false;
    std::string       m_pexelsQuery;
    std::string       m_moeSearch;
    int               m_moeLimit = 0;
    HwInfo            m_hw;
};

}
