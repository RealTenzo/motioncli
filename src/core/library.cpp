#include "core/library.h"
#include "core/config.h"
#include "core/moewalls.h"
#include "core/steam.h"
#include "net/http.h"
#include "util/json.h"
#include "util/str.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace motion {

namespace {

std::wstring extensionFor(const std::string& url) {
    size_t q = url.find('?');
    std::string clean = (q == std::string::npos) ? url : url.substr(0, q);
    size_t dot = clean.find_last_of('.');
    size_t slash = clean.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        std::string ext = clean.substr(dot);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".mp4" || ext == ".webm" || ext == ".mov" ||
            ext == ".mkv" || ext == ".m4v"  || ext == ".avi")
            return widen(ext);
    }
    return L".mp4";
}

bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            out.push_back(c);
        else if (c == ' ')
            out.push_back('-');
    }
    if (out.empty()) out = "clip";
    return out;
}

bool parseCatalog(const std::string& body, std::vector<Wallpaper>& out, std::string& err) {
    Json root;
    try {
        root = Json::parse(body);
    } catch (const std::exception& e) {
        err = std::string("Invalid catalog JSON: ") + e.what();
        return false;
    }

    const Json& list = root["wallpapers"];
    if (!list.isArray()) {
        err = "Catalog has no \"wallpapers\" array";
        return false;
    }

    std::vector<Wallpaper> parsed;
    for (const Json& entry : list.array) {
        Wallpaper w;
        w.id    = entry["id"].asString();
        w.title = entry["title"].asString(w.id);
        w.url   = entry["url"].asString();
        w.isLocal   = entry["local"].asBool(false);
        w.localFile = entry["local_path"].asString();
        if (w.id.empty()) continue;
        if (!w.isLocal && w.url.empty()) continue;

        w.author     = entry["author"].asString();
        w.preview    = entry["preview"].asString();
        w.resolution = entry["resolution"].asString();
        w.sizeMb     = entry["size_mb"].asInt(0);

        const Json& tags = entry["tags"];
        if (tags.isArray())
            for (const Json& t : tags.array)
                if (t.isString()) w.tags.push_back(t.str);

        parsed.push_back(std::move(w));
    }

    out = std::move(parsed);
    return true;
}

const char* kBuiltinCatalog = R"JSON({
  "version": 1,
  "wallpapers": [
    { "id": "big-buck-bunny", "title": "Big Buck Bunny", "author": "Blender Foundation",
      "url": "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4",
      "tags": ["animation", "nature", "colorful"], "resolution": "1280x720", "size_mb": 158 },
    { "id": "elephants-dream", "title": "Elephants Dream", "author": "Blender Foundation",
      "url": "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ElephantsDream.mp4",
      "tags": ["animation", "surreal", "dark"], "resolution": "1280x720", "size_mb": 169 },
    { "id": "sintel", "title": "Sintel", "author": "Blender Foundation",
      "url": "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/Sintel.mp4",
      "tags": ["animation", "fantasy", "epic"], "resolution": "1280x720", "size_mb": 114 },
    { "id": "for-bigger-blazes", "title": "For Bigger Blazes", "author": "Google",
      "url": "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerBlazes.mp4",
      "tags": ["short", "bright"], "resolution": "1280x720", "size_mb": 2 },
    { "id": "for-bigger-joyrides", "title": "For Bigger Joyrides", "author": "Google",
      "url": "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/ForBiggerJoyrides.mp4",
      "tags": ["short", "drive"], "resolution": "1280x720", "size_mb": 3 }
  ]
})JSON";

}

void Library::rebuild() {
    m_merged.clear();
    m_merged.reserve(m_catalog.size() + m_local.size());
    for (const auto& w : m_catalog) m_merged.push_back(&w);
    for (const auto& w : m_local) m_merged.push_back(&w);
}

bool Library::fetch(const std::wstring& catalogUrl, std::string& err) {
    std::string body;
    if (!http::getString(catalogUrl, body, err))
        return false;

    std::vector<Wallpaper> parsed;
    if (!parseCatalog(body, parsed, err))
        return false;

    m_catalog = std::move(parsed);
    loadLocalLibrary();
    return true;
}

namespace {

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else if (c == ' ')
            out += "%20";
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
    }
    return out;
}

}

bool Library::fetchPexels(const std::wstring& apiKey, const std::string& query,
                          int maxWidth, std::string& err) {
    if (apiKey.empty()) { err = "No Pexels API key set"; return false; }

    std::wstring url = query.empty()
        ? L"https://api.pexels.com/videos/popular?per_page=24&min_width=1280"
        : L"https://api.pexels.com/videos/search?per_page=24&orientation=landscape&query=" +
              widen(urlEncode(query));

    std::wstring headers = L"Authorization: " + apiKey + L"\r\n";

    std::string body;
    if (!http::getString(url, body, err, headers)) return false;

    Json root;
    try { root = Json::parse(body); }
    catch (const std::exception& e) { err = std::string("Pexels JSON: ") + e.what(); return false; }

    const Json& videos = root["videos"];
    if (!videos.isArray()) { err = "Pexels returned no videos"; return false; }

    std::vector<Wallpaper> parsed;
    for (const Json& v : videos.array) {
        const Json& files = v["video_files"];
        if (!files.isArray() || files.array.empty()) continue;

        const Json* best = nullptr;
        int bestW = -1, smallestW = 1 << 30;
        const Json* smallest = nullptr;
        for (const Json& f : files.array) {
            if (f["file_type"].asString() != "video/mp4") continue;
            int w = f["width"].asInt(0);
            if (w < smallestW) { smallestW = w; smallest = &f; }
            if (w <= maxWidth && w > bestW) { bestW = w; best = &f; }
        }
        if (!best) best = smallest;
        if (!best) continue;

        Wallpaper wp;
        wp.id = "pexels-" + std::to_string(v["id"].asInt(0));
        wp.url = (*best)["link"].asString();
        if (wp.url.empty()) continue;
        wp.author = v["user"]["name"].asString("Pexels");
        wp.preview = v["image"].asString();
        int w = (*best)["width"].asInt(0), h = (*best)["height"].asInt(0);
        if (w && h) wp.resolution = std::to_string(w) + "x" + std::to_string(h);
        wp.title = (query.empty() ? "Pexels " : query + " ") + "#" + std::to_string(v["id"].asInt(0));
        wp.tags = { "pexels" };
        parsed.push_back(std::move(wp));
    }

    if (parsed.empty()) { err = "No usable videos in Pexels response"; return false; }

    m_catalog = std::move(parsed);
    loadLocalLibrary();
    return true;
}

bool Library::fetchSteamWorkshop(const std::string& sort, std::string& err) {
    std::vector<steam::Item> items;
    if (!steam::fetchTrending(sort, items, err)) return false;

    std::vector<Wallpaper> parsed;
    for (const steam::Item& it : items) {
        Wallpaper w;
        w.id = "steam-" + it.id;
        w.steamId = it.id;
        w.title = it.title;
        w.author = it.author.empty() ? "Steam Workshop" : it.author;
        w.preview = it.preview;
        w.tags = { "steam" };
        parsed.push_back(std::move(w));
    }

    m_catalog = std::move(parsed);
    loadLocalLibrary();
    return true;
}

bool Library::fetchMoeWalls(const std::string& query, const std::string& category,
                            int maxItems, std::string& err) {
    std::vector<moewalls::Item> items;
    if (!moewalls::fetchListing(query, category, maxItems, items, err)) return false;

    std::vector<Wallpaper> parsed;
    for (moewalls::Item& it : items) {
        Wallpaper w;
        w.id = "moe-" + it.id;
        w.title = it.title;
        w.author = "MoeWalls";
        w.sourceUrl = it.postUrl;
        w.tags = { "moewalls" };
        parsed.push_back(std::move(w));
    }

    m_catalog = std::move(parsed);
    loadLocalLibrary();
    return true;
}

bool Library::resolve(Wallpaper& w, std::string& err) {
    if (!w.url.empty() || w.sourceUrl.empty()) return true;
    moewalls::Item it;
    it.postUrl = w.sourceUrl;
    if (!moewalls::resolve(it, err)) return false;
    w.url = it.url;
    w.preview = it.preview;
    w.previewVideo = it.previewVideo;
    w.resolution = it.resolution;
    if (!it.title.empty()) w.title = it.title;
    if (!it.tags.empty())  w.tags = it.tags;
    return true;
}

const std::vector<Wallpaper>& Library::savedWallpapers() {
    loadLocalLibrary();
    scanFileSystem();
    return m_local;
}

void Library::scanFileSystem() {
    std::set<std::wstring> have;
    for (const auto& w : m_local) have.insert(widen(w.localFile));

    std::wstring dir = Config::wallpapersDir();
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (have.count(path)) continue;

            std::string name = narrow(fd.cFileName);
            size_t dot = name.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string ext = name.substr(dot);
            if (ext != ".mp4" && ext != ".webm" && ext != ".mov" && ext != ".mkv") continue;

            std::string stem = name.substr(0, dot);
            std::string title = stem;
            if (title.rfind("moe-", 0) == 0)        title = title.substr(4);
            else if (title.rfind("local-", 0) == 0) title = title.substr(6);
            for (char& c : title) if (c == '-' || c == '_') c = ' ';
            if (!title.empty()) title[0] = (char)toupper((unsigned char)title[0]);

            Wallpaper w;
            w.id = stem;
            w.title = title.empty() ? stem : title;
            w.author = "saved";
            w.isLocal = true;
            w.localFile = narrow(path);
            w.tags = { "saved" };
            m_local.push_back(std::move(w));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    rebuild();
}

void Library::loadBuiltin() {
    std::string err;
    std::vector<Wallpaper> parsed;
    parseCatalog(kBuiltinCatalog, parsed, err);
    m_catalog = std::move(parsed);
    loadLocalLibrary();
}

void Library::loadLocalLibrary() {
    m_local.clear();

    std::ifstream in(Config::userLibraryPath(), std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (!text.empty()) {
            std::string err;
            std::vector<Wallpaper> parsed;
            if (parseCatalog(text, parsed, err)) {
                for (auto& w : parsed) {
                    w.isLocal = true;
                    if (w.localFile.empty()) w.localFile = w.url;
                    m_local.push_back(std::move(w));
                }
            }
        }
    }

    rebuild();
}

void Library::saveLocalLibrary() {
    Json root = Json::makeObject();
    root.set("version", Json::makeNumber(1));
    Json arr = Json::makeArray();
    arr.array.reserve(m_local.size());
    for (const Wallpaper& w : m_local) {
        Json e = Json::makeObject();
        e.set("id", Json::makeString(w.id));
        e.set("title", Json::makeString(w.title));
        e.set("author", Json::makeString(w.author));
        e.set("local", Json::makeBool(true));
        e.set("local_path", Json::makeString(w.localFile));
        e.set("resolution", Json::makeString(w.resolution));
        Json tags = Json::makeArray();
        tags.array.reserve(w.tags.size());
        for (const auto& t : w.tags) tags.array.push_back(Json::makeString(t));
        e.set("tags", std::move(tags));
        arr.array.push_back(std::move(e));
    }
    root.set("wallpapers", std::move(arr));

    std::ofstream out(Config::userLibraryPath(), std::ios::binary | std::ios::trunc);
    if (out) out << root.dump(2);
}

bool Library::importFile(const std::wstring& srcPath, std::string& outId, std::string& err) {
    if (!fileExists(srcPath)) { err = "File not found"; return false; }

    std::wstring fileName = srcPath;
    size_t slash = fileName.find_last_of(L"\\/");
    if (slash != std::wstring::npos) fileName = fileName.substr(slash + 1);
    std::wstring ext = L".mp4";
    size_t dot = fileName.find_last_of(L'.');
    std::wstring stem = fileName;
    if (dot != std::wstring::npos) { ext = fileName.substr(dot); stem = fileName.substr(0, dot); }

    std::string title = narrow(stem);
    std::string baseId = "local-" + sanitize(narrow(stem));
    std::string id = baseId;
    int n = 1;
    auto idExists = [&](const std::string& candidate) {
        for (const auto& w : m_local) if (w.id == candidate) return true;
        return false;
    };
    while (idExists(id)) id = baseId + "-" + std::to_string(++n);

    std::wstring dest = Config::wallpapersDir() + L"\\" + widen(id) + ext;
    if (!CopyFileW(srcPath.c_str(), dest.c_str(), FALSE)) {
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Copy failed (%lu)", GetLastError());
        err = buf;
        return false;
    }

    Wallpaper w;
    w.id = id;
    w.title = title.empty() ? id : title;
    w.author = "imported";
    w.isLocal = true;
    w.localFile = narrow(dest);
    w.tags = { "local" };
    m_local.push_back(w);

    saveLocalLibrary();
    rebuild();

    outId = id;
    return true;
}

bool Library::removeLocal(const std::string& id) {
    for (auto it = m_local.begin(); it != m_local.end(); ++it) {
        if (it->id == id) {
            DeleteFileW(widen(it->localFile).c_str());
            m_local.erase(it);
            saveLocalLibrary();
            rebuild();
            return true;
        }
    }
    return false;
}

bool Library::removeSaved(const Wallpaper& w) {
    if (removeLocal(w.id)) return true;
    std::wstring path = (w.isLocal && !w.localFile.empty()) ? widen(w.localFile) : localPath(w);
    if (path.empty()) return false;
    BOOL ok = DeleteFileW(path.c_str());
    rebuild();
    return ok != 0;
}

std::wstring Library::localPath(const Wallpaper& w) {
    if (w.isLocal) return widen(w.localFile);
    return Config::wallpapersDir() + L"\\" + widen(w.id) + extensionFor(w.url);
}

bool Library::isDownloaded(const Wallpaper& w) {
    return fileExists(localPath(w));
}

bool Library::ensureDownloaded(const Wallpaper& w,
                               const std::function<void(int)>& onProgress,
                               std::wstring& outPath,
                               std::string& err) {
    std::wstring dest = localPath(w);
    outPath = dest;

    if (w.isLocal) {
        if (!fileExists(dest)) { err = "Imported file is missing"; return false; }
        if (onProgress) onProgress(100);
        return true;
    }

    if (isDownloaded(w)) {
        if (onProgress) onProgress(100);
        return true;
    }

    std::wstring mediaUrl = widen(w.url);
    if (mediaUrl.empty() && !w.sourceUrl.empty()) {
        moewalls::Item it; it.postUrl = w.sourceUrl;
        if (!moewalls::resolve(it, err)) return false;
        mediaUrl = widen(it.url);
    }
    if (mediaUrl.empty() && !w.steamId.empty()) {
        if (!steam::resolveVideoUrl(w.steamId, mediaUrl, err)) return false;
    }
    if (mediaUrl.empty()) { err = "No media URL for this wallpaper"; return false; }

    auto progress = [&](unsigned long long received, unsigned long long total) {
        if (!onProgress) return;
        if (total > 0) onProgress((int)((received * 100) / total));
        else           onProgress(-1);
    };

    return http::downloadFile(mediaUrl, dest, progress, err);
}

bool Library::exportMedia(const Wallpaper& w, const std::wstring& destPath, std::string& err) {
    std::wstring src;
    if (!ensureDownloaded(w, nullptr, src, err)) return false;
    if (!CopyFileW(src.c_str(), destPath.c_str(), FALSE)) {
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Export failed (%lu)", GetLastError());
        err = buf;
        return false;
    }
    return true;
}

}
