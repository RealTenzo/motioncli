#pragma once
#include <string>
#include <vector>

namespace motion {

struct Wallpaper {
    std::string id;
    std::string title;
    std::string author;
    std::string preview;
    std::string previewVideo;
    std::string url;
    std::string sourceUrl;
    std::vector<std::string> tags;
    std::string resolution;
    int sizeMb = 0;

    bool isLocal = false;
    std::string localFile;
    std::string steamId;
};

class Library {
public:
    bool fetch(const std::wstring& catalogUrl, std::string& err);

    bool fetchPexels(const std::wstring& apiKey, const std::string& query,
                     int maxWidth, std::string& err);
    bool fetchSteamWorkshop(const std::string& sort, std::string& err);

    bool fetchMoeWalls(const std::string& query, const std::string& category,
                       int maxItems, std::string& err);
    bool resolve(Wallpaper& w, std::string& err);

    const std::vector<Wallpaper>& savedWallpapers();

    void loadBuiltin();
    void loadLocalLibrary();

    const std::vector<const Wallpaper*>& items() const { return m_merged; }
    bool empty() const { return m_catalog.empty() && m_local.empty(); }

    bool importFile(const std::wstring& srcPath, std::string& outId, std::string& err);
    bool removeLocal(const std::string& id);
    bool removeSaved(const Wallpaper& w);

    static std::wstring localPath(const Wallpaper& w);
    static bool isDownloaded(const Wallpaper& w);

    bool ensureDownloaded(const Wallpaper& w,
                          void(*onProgress)(int percent, void* ctx),
                          void* progressCtx,
                          std::wstring& outPath,
                          std::string& err);
    bool exportMedia(const Wallpaper& w, const std::wstring& destPath, std::string& err);

private:
    void rebuild();
    void scanFileSystem();
    void saveLocalLibrary();

    std::vector<Wallpaper> m_catalog;
    std::vector<Wallpaper> m_local;
    std::vector<const Wallpaper*> m_merged;
};

}
