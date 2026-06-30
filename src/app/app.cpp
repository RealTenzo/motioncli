#include "app/app.h"
#include "tui/menu.h"
#include "tui/dialogs.h"
#include "tui/image.h"
#include "core/autostart.h"
#include "net/http.h"
#include "util/str.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#ifndef MOTION_VERSION
#define MOTION_VERSION "dev"
#endif

namespace motion {

using namespace tui;

namespace {

std::string joinTags(const std::vector<std::string>& tags) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += ", ";
        out += tags[i];
    }
    return out;
}

std::vector<std::string> wrapText(const std::string& text, size_t width) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
        i = (sp == std::string::npos) ? text.size() : sp + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

}

App::App() : m_config(Config::load()), m_engine(m_config) {
    m_term.setTitle(L"Motion CLI — live wallpaper");
    m_term.hideCursor();
    m_hw = scanHardware();
}

int App::run() {
    if (m_config.firstRun) {
        autoTune(false);
        guide();
        m_config.firstRun = false;
        m_config.save();
    }
    bool haveWallpaper = !m_config.currentMediaPath.empty() ||
                         !m_config.monitorAssignments.empty();
    if (haveWallpaper && !m_engine.isRunning()) {
        std::string e;
        m_engine.restart(e);
    }
    mainMenu();
    m_term.clearScreen();
    m_term.showCursor();
    return 0;
}

void App::autoTune(bool announce) {
    m_hw = scanHardware();
    m_config.quality = recommendedQuality(m_hw);
    m_config.lowEndMode = recommendLowEnd(m_hw);
    m_config.save();
    m_catalogLoaded = false;

    if (!announce) return;

    auto qn = m_config.quality == Quality::High ? "High" :
              m_config.quality == Quality::Medium ? "Medium" : "Low";
    char hwline[128];
    _snprintf_s(hwline, sizeof(hwline), _TRUNCATE,
                "%d cores · %d GB RAM · %d MB VRAM", m_hw.cores, m_hw.ramGB, m_hw.vramMB);
    notify("Auto-tune", {
        { color::green, std::string("✓ Detected a ").append(tierName(m_hw)).append(" PC") },
        { color::gray,  hwline },
        { color::cyan,  std::string("Quality set to ").append(qn).append(m_config.lowEndMode ? " + Low-end mode on." : ".") },
    });
    std::string err;
    if (m_engine.isRunning()) m_engine.restart(err);
}

void App::notify(const std::string& title,
                 const std::vector<std::pair<const char*, std::string>>& lines) {
    Frame f;
    draw::banner(f);
    draw::title(f, title);
    for (const auto& [c, text] : lines)
        f.raw("  ").raw(c).raw(text).raw(color::reset).line();
    f.line();
    draw::footer(f, "Press any key to continue…");
    m_term.present(f);
    m_term.readKey();
}

void App::guide() {
    struct Beat { std::string who; std::string text; };
    const std::vector<Beat> beats = {
        { "tenzo", "Yo! Welcome to Motion CLI. I'm tenzo — I made this." },
        { "tenzo", "It runs living video wallpapers on your desktop, no heavy apps." },
        { "tenzo", "Open Browse Library to pull free clips from MoeWalls." },
        { "tenzo", "Move with W/S or the arrow keys. Press A/D to flip category tabs." },
        { "tenzo", "Hit / to search anything, R to refresh, and scroll for Load more." },
        { "tenzo", "Open one to Preview it right here, then Download & apply." },
        { "tenzo", "Everything you use is kept in My Wallpapers, and it resumes on reboot." },
        { "tenzo", "That's the tour. Go make your desktop move. - tenzo" },
    };

    std::string rule;
    rule.reserve(80);
    rule.append("  ").append(color::brightCyan)
        .append("════════════════════════════════════════════════════════")
        .append(color::reset);

    for (size_t i = 0; i < beats.size(); ++i) {
        Frame f;
        draw::banner(f);
        f.line(rule);
        f.raw("   ").raw(color::brightCyan).raw(color::bold).raw(beats[i].who).raw(color::reset).line();
        f.line();
        for (const std::string& ln : wrapText(beats[i].text, 52))
            f.raw("   ").raw(color::white).raw(ln).raw(color::reset).line();
        f.line();
        f.line(rule);
        f.line();
        char pos[16];
        _snprintf_s(pos, sizeof(pos), _TRUNCATE, "(%d/%d)", (int)i + 1, (int)beats.size());
        draw::footer(f, std::string("⏎ next   esc skip   ").append(pos));
        m_term.present(f);
        if (m_term.readKey().key == Key::Escape) return;
    }
}

void App::help() {
    Frame f;
    draw::banner(f);
    draw::title(f, "Help · Quick start");
    f.raw("  ").raw(color::brightCyan).raw("Apply a wallpaper").raw(color::reset).line();
    f.line("    Browse Library → pick one → Download & apply.");
    f.line("    First use downloads it once, then it's cached.");
    f.line();
    f.raw("  ").raw(color::brightCyan).raw("Browse & preview").raw(color::reset).line();
    f.line("    In Browse Library use ⇅ Category to switch (anime, games,");
    f.line("    landscape, fantasy…). Open one, then 'Preview (open in");
    f.line("    browser)' to see it before you download.");
    f.line();
    f.raw("  ").raw(color::brightCyan).raw("Use your own video").raw(color::reset).line();
    f.line("    Import a video… → choose a .mp4/.wmv/.mov from your PC.");
    f.line();
    f.raw("  ").raw(color::brightCyan).raw("Multiple monitors").raw(color::reset).line();
    f.line("    Per-monitor setup → assign a different wallpaper per screen.");
    f.line();
    f.raw("  ").raw(color::brightCyan).raw("Smooth & light").raw(color::reset).line();
    f.line("    Performance settings auto-pause a screen when an app goes");
    f.line("    fullscreen on it (other screens keep playing), and Low-end");
    f.line("    mode keeps playback at 1080p (skips 4K) for weaker PCs.");
    f.line();
    f.raw("  ").raw(color::brightCyan).raw("Controls").raw(color::reset).line();
    f.line("    Tray icon: mute / stop / reopen. Settings: start on login.");
    f.line();
    f.raw("  ").raw(color::magenta).raw("Motion CLI v" MOTION_VERSION)
     .raw(" · Dev by tenzo").raw(color::reset)
     .raw(color::gray).raw(" · Open source · MIT").raw(color::reset).line();
    f.line();
    draw::footer(f, "Press any key to continue…");
    m_term.present(f);
    m_term.readKey();
}

void App::mainMenu() {
    while (true) {
        const bool running = m_engine.isRunning();
        std::string activeHint;
        if (running) {
            activeHint = "● ";
            activeHint += m_config.mode == WallpaperMode::PerMonitor
                ? "per-monitor"
                : (m_config.currentWallpaperId.empty() ? "running" : m_config.currentWallpaperId);
        } else {
            activeHint = "none";
        }

        Menu menu(m_term, "Main menu", "A super-lightweight live wallpaper engine.");
        menu.setItems({
            { "Browse Library",    "search · tabs · preview" },
            { "My Wallpapers",     "saved & imported" },
            { "Per-monitor setup", "one wallpaper per screen" },
            { "Active Wallpaper",  activeHint },
            { "Settings",          "" },
            { "Quit",              "" },
        });
        menu.setFooter("↑/↓ move   ⏎ select   q/esc quit");

        switch (menu.run()) {
            case 0: browseLibrary();    break;
            case 1: myWallpapers();     break;
            case 2: perMonitorSetup();  break;
            case 3: activeWallpaper();  break;
            case 4: settings();         break;
            case 5: case -1:            return;
            default: break;
        }
    }
}

bool App::ensureCatalog(bool forceRefresh) {
    if (m_catalogLoaded && !forceRefresh) return true;

    std::string where;
    if (!m_moeSearch.empty()) {
        where.reserve(8 + m_moeSearch.size());
        where.append("search: ").append(m_moeSearch);
    } else {
        where = m_config.moeCategory.empty() ? "latest" : m_config.moeCategory;
    }

    Frame f;
    draw::banner(f);
    draw::title(f, "Library");
    f.raw(color::gray).raw("  Loading wallpapers (").raw(where).raw(")…").raw(color::reset).line();
    m_term.present(f);

    int limit = m_moeLimit > 0 ? m_moeLimit : m_config.libraryCount;
    std::string err;
    if (m_library.fetchMoeWalls(m_moeSearch, m_config.moeCategory, limit, err)) {
        m_catalogLoaded = true;
        return true;
    }

    m_library.loadBuiltin();
    m_catalogLoaded = true;
    notify("Library", {
        { color::yellow, "Couldn't reach the online library:" },
        { color::gray,   std::string("  ").append(err) },
        { color::cyan,   "Showing the built-in clips + your imports instead." },
        { color::gray,   "Check your connection, then choose ↻ Refresh." },
    });
    return true;
}

int App::pickWallpaper(const std::string& title, const std::string& subtitle) {
    if (!ensureCatalog()) return -1;

    Menu menu(m_term, title, subtitle);
    std::vector<MenuItem> items;
    for (const Wallpaper* w : m_library.items()) {
        std::string hint;
        if (w->isLocal) hint = "local";
        if (Library::isDownloaded(*w)) {
            if (!hint.empty()) hint += "  ";
            hint += "✓";
        }
        if (!w->resolution.empty()) {
            if (!hint.empty()) hint += "  ";
            hint += w->resolution;
        }
        items.push_back({ w->title, hint });
    }
    menu.setItems(items);
    menu.setFooter("↑/↓ move   ⏎ select   esc back");
    return menu.run();
}

static const char* kMoeCategories[] = {
    "anime", "games", "landscape", "fantasy",
    "sci-fi", "abstract", "animal", "vehicle", ""
};
static const int kMoeCategoryCount =
    (int)(sizeof(kMoeCategories) / sizeof(kMoeCategories[0]));

void App::browseLibrary() {
    if (!ensureCatalog()) return;

    int selected = 0;
    while (true) {
        std::string sub;
        if (!m_moeSearch.empty()) {
            sub.clear();
            sub.reserve(80 + m_moeSearch.size());
            sub.append(color::brightCyan).append("search: \"").append(m_moeSearch).append("\"")
                .append(color::reset).append(color::gray).append("  (a/d → categories)").append(color::reset);
        } else {
            for (int i = 0; i < kMoeCategoryCount; ++i) {
                std::string name = kMoeCategories[i][0] ? kMoeCategories[i] : "latest";
                bool cur = m_config.moeCategory == kMoeCategories[i];
                if (cur) {
                    sub.append(color::brightCyan).append("[").append(name).append("]").append(color::reset).append(" ");
                } else {
                    sub.append(color::gray).append(name).append(color::reset).append(" ");
                }
            }
        }

        Menu menu(m_term, "Browse Library", sub);
        std::vector<MenuItem> items;
        for (const Wallpaper* w : m_library.items()) {
            std::string hint;
            if (w->isLocal) hint = "local";
            else if (Library::isDownloaded(*w)) hint = "✓ cached";
            items.push_back({ w->title, hint });
        }
        const int loadMoreIdx = (int)items.size();
        items.push_back({ "▼ Load more", "" });
        menu.setItems(items);
        menu.setHotkeys("ad/r");
        menu.setFooter("w/s or ↑/↓ move   ⏎ open   a/d tabs   / search   r refresh   esc back");

        int choice = menu.run(selected);
        if (choice == -1) return;

        if (choice == Menu::kHotkey) {
            char k = menu.hotkey();
            if (k == 'r') { ensureCatalog(true); selected = 0; continue; }
            if (k == '/') {
                Frame f;
                draw::banner(f);
                draw::title(f, "Search MoeWalls");
                f.raw(color::gray).raw("  Search (e.g. naruto, sunset, cyberpunk); blank clears:").raw(color::reset).line();
                f.line();
                f.raw("  ");
                m_term.present(f);
                m_moeSearch = m_term.readLineAt(13, 3, m_moeSearch);
                m_moeLimit = 0;
                ensureCatalog(true);
                selected = 0;
                continue;
            }
            m_moeSearch.clear();
            m_moeLimit = 0;
            int cur = 0;
            for (int i = 0; i < kMoeCategoryCount; ++i)
                if (m_config.moeCategory == kMoeCategories[i]) { cur = i; break; }
            cur = (k == 'd') ? (cur + 1) % kMoeCategoryCount
                             : (cur - 1 + kMoeCategoryCount) % kMoeCategoryCount;
            m_config.moeCategory = kMoeCategories[cur];
            m_config.save();
            ensureCatalog(true);
            selected = 0;
            continue;
        }

        if (choice == loadMoreIdx) {
            m_moeLimit = (m_moeLimit > 0 ? m_moeLimit : m_config.libraryCount) + 24;
            ensureCatalog(true);
            selected = loadMoreIdx;
            continue;
        }

        selected = choice;
        wallpaperDetail(*m_library.items()[choice]);
    }
}

void App::myWallpapers() {
    while (true) {
        std::vector<Wallpaper> saved = m_library.savedWallpapers();

        std::string mySubtitle;
        if (saved.empty()) {
            mySubtitle = "Nothing yet — import a video or download one from Browse.";
        } else {
            mySubtitle = std::to_string(saved.size());
            mySubtitle += " on this PC";
        }
        Menu menu(m_term, "My Wallpapers", mySubtitle);
        std::vector<MenuItem> items;
        for (const Wallpaper& w : saved)
            items.push_back({ w.title, w.author == "imported" ? "import" : "saved" });
        const int importIdx = (int)saved.size();
        items.push_back({ "＋ Import a video…", "use your own .mp4" });
        items.push_back({ "Back", "" });
        menu.setItems(items);
        menu.setHotkeys("x");
        menu.setFooter("w/s move   ⏎ open   x delete   esc back");

        int choice = menu.run();
        if (choice == -1) return;
        if (choice == Menu::kHotkey) {
            int idx = menu.selectedIndex();
            if (idx >= 0 && idx < (int)saved.size()) deleteWallpaper(saved[idx]);
            continue;
        }
        if (choice == importIdx + 1) return;
        if (choice == importIdx) { importWallpaper(); continue; }
        wallpaperDetail(saved[choice]);
    }
}

void App::deleteWallpaper(const Wallpaper& w) {
    bool active = !w.localFile.empty() && widen(w.localFile) == m_config.currentMediaPath;
    if (!m_library.removeSaved(w)) {
        notify("My Wallpapers", { { color::red, "Couldn't delete that file." } });
        return;
    }
    if (active) {
        m_engine.stop();
        m_config.currentWallpaperId.clear();
        m_config.currentMediaPath.clear();
        m_config.save();
    }
    notify("My Wallpapers", { { color::yellow, std::string("Deleted \"").append(w.title).append("\".") } });
}

void App::wallpaperDetail(const Wallpaper& w) {
    Wallpaper wr = w;
    if (wr.url.empty() && !wr.sourceUrl.empty()) {
        Frame f;
        draw::banner(f);
        draw::title(f, "Loading…");
        f.raw(color::gray).raw("  Fetching ").raw(wr.title).raw("…").raw(color::reset).line();
        m_term.present(f);
        std::string err;
        if (!m_library.resolve(wr, err)) {
            notify("Library", { { color::red, "Couldn't open this wallpaper:" },
                                { color::gray, std::string("  ").append(err) } });
            return;
        }
    }

    const bool cached = Library::isDownloaded(wr);

    auto line = [](const std::string& label, const std::string& value) -> std::string {
        if (value.empty()) return {};
        std::string r;
        r.reserve(4 + label.size() + 2 + value.size());
        r.append("\r\n  ").append(label).append(": ").append(value);
        return r;
    };
    std::string detail;
    detail += line("Author", wr.author);
    detail += line("Resolution", wr.resolution);
    detail += line("Tags", joinTags(wr.tags));
    if (wr.sizeMb > 0) detail += line("Size", std::to_string(wr.sizeMb).append(" MB"));
    detail += line("Status", cached ? "✓ ready" : "not downloaded");

    const bool canPreview = !wr.previewVideo.empty() || !wr.preview.empty();

    std::string wt;
    wt.reserve(14 + wr.title.size());
    wt.append("Wallpaper · ").append(wr.title);
    Menu actions(m_term, wt, detail);
    std::vector<MenuItem> items = {
        { cached ? "Apply (whole desktop)" : "Download & apply", "" },
    };
    if (canPreview) items.push_back({ "Preview", "in console · b for browser" });
    items.push_back({ "Assign to a monitor…", "" });
    items.push_back({ "Export a copy…", "" });
    if (wr.isLocal) items.push_back({ "Delete", "" });
    items.push_back({ "Back", "" });
    actions.setItems(items);
    actions.setFooter("↑/↓ move   ⏎ select   esc back");

    const int previewIndex = canPreview ? 1 : -1;
    const int assignIndex  = canPreview ? 2 : 1;
    const int exportIndex  = canPreview ? 3 : 2;
    const int deleteIndex  = wr.isLocal ? exportIndex + 1 : -1;

    int choice = actions.run();
    if (choice == -1) return;
    if (choice == 0)                  applyWallpaper(wr);
    else if (choice == previewIndex)  previewWallpaper(wr);
    else if (choice == assignIndex)   assignToMonitor(wr);
    else if (choice == exportIndex)   exportWallpaper(wr);
    else if (choice == deleteIndex)   deleteWallpaper(wr);
}

void App::previewWallpaper(const Wallpaper& w) {
    std::string img;
    if (!w.preview.empty()) {
        int cols = 96, rows = 30;
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            cols = csbi.srWindow.Right - csbi.srWindow.Left + 1 - 4;
            rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1 - 13;
            if (cols < 24) cols = 24;
            if (rows < 8)  rows = 8;
        }
        wchar_t tp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tp);
        std::wstring tmp(tp);
        tmp.append(L"motioncli_preview.jpg");
        std::string err;
        if (http::downloadFile(widen(w.preview), tmp, nullptr, err))
            tui::renderImage(tmp, cols, rows, img);
    }

    const std::string& browseTarget = !w.previewVideo.empty() ? w.previewVideo : w.preview;

    Frame f;
    draw::banner(f);
    std::string pt;
    pt.reserve(12 + w.title.size());
    pt.append("Preview · ").append(w.title);
    draw::title(f, pt);
    if (!img.empty()) f.raw(img);
    else f.raw(color::gray).raw("  (no in-console preview for this one)").raw(color::reset).line();
    f.line();
    draw::footer(f, browseTarget.empty()
        ? "any key: back"
        : "b: open the moving preview in your browser    any key: back");
    m_term.present(f);

    KeyEvent ev = m_term.readKey();
    if (ev.key == Key::Char && (ev.ch == 'b' || ev.ch == 'B') && !browseTarget.empty())
        ShellExecuteW(nullptr, L"open", widen(browseTarget).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool App::prepareMedia(const Wallpaper& w, std::wstring& outPath) {
    auto progressFrame = [&](const std::string& statusLine) {
        Frame f;
        draw::banner(f);
        std::string pt2;
        pt2.reserve(14 + w.title.size());
        pt2.append("Preparing · ").append(w.title);
        draw::title(f, pt2);
        f.line();
        f.raw("  ").raw(statusLine).line();
        m_term.present(f);
    };

    int lastPct = -2;
    auto onProgress = [&](int pct) {
        if (pct == lastPct) return;
        lastPct = pct;
        if (pct < 0) {
            progressFrame(std::string(color::cyan).append("Downloading…").append(color::reset));
        } else {
            int filled = pct / 5;
            std::string bar(filled, '#');
            bar.append(20 - filled, '.');
            char buf[96];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Downloading  [%s] %3d%%", bar.c_str(), pct);
            progressFrame(std::string(color::cyan).append(buf).append(color::reset));
        }
    };

    if (!Library::isDownloaded(w))
        progressFrame(std::string(color::gray).append("Starting…").append(color::reset));

    std::string err;
    if (!m_library.ensureDownloaded(w, onProgress, outPath, err)) {
        notify("Download failed", { { color::red, err } });
        return false;
    }
    return true;
}

bool App::applyWallpaper(const Wallpaper& w) {
    std::wstring path;
    if (!prepareMedia(w, path)) return false;

    m_config.mode = WallpaperMode::Span;
    m_config.currentWallpaperId = w.id;
    m_config.currentMediaPath   = path;
    m_config.save();

    std::string err;
    if (!m_engine.restart(err)) {
        notify("Could not start wallpaper", { { color::red, err } });
        return false;
    }

    notify("Wallpaper applied", {
        { color::green, std::string("✓ ").append(w.title).append(" is now live across your desktop.") },
        { color::gray,  "It keeps running in the background (see the tray icon)." },
        { color::gray,  autostart::isEnabled()
                            ? "It will also resume automatically on login."
                            : "Enable 'Start on login' in Settings to auto-resume." },
    });
    return true;
}

void App::assignToMonitor(const Wallpaper& w) {
    std::vector<MonitorInfo> monitors = enumerateMonitors();
    if (monitors.empty()) {
        notify("Per-monitor", { { color::red, "No monitors detected." } });
        return;
    }

    std::string menuTitle;
    menuTitle.reserve(14 + w.title.size());
    menuTitle.append("Assign \u201c").append(w.title).append("\u201d to\u2026");
    Menu menu(m_term, menuTitle, "Pick a monitor");
    std::vector<MenuItem> items;
    for (const MonitorInfo& m : monitors) {
        char dims[48];
        _snprintf_s(dims, sizeof(dims), _TRUNCATE, "%dx%d", m.width, m.height);
        std::string label;
        label.reserve(16);
        label.append("Monitor ").append(std::to_string(m.index));
        if (m.primary) label.append(" (primary)");
        std::string hint = std::string(dims);
        auto it = m_config.monitorAssignments.find(m.device);
        if (it != m_config.monitorAssignments.end()) hint += "  · assigned";
        items.push_back({ label, hint });
    }
    items.push_back({ "Cancel", "" });
    menu.setItems(items);
    menu.setFooter("↑/↓ move   ⏎ assign   esc cancel");

    int choice = menu.run();
    if (choice < 0 || choice >= (int)monitors.size()) return;

    std::wstring path;
    if (!prepareMedia(w, path)) return;

    m_config.monitorAssignments[monitors[choice].device] = path;
    m_config.mode = WallpaperMode::PerMonitor;
    m_config.save();

    std::string err;
    if (!m_engine.restart(err)) {
        notify("Per-monitor", { { color::red, err } });
        return;
    }
    notify("Per-monitor", {
        { color::green, std::string("✓ ").append(w.title).append(" assigned to Monitor ")
                            .append(std::to_string(monitors[choice].index)).append(".") },
        { color::gray,  "Switched to per-monitor mode." },
    });
}

void App::exportWallpaper(const Wallpaper& w) {
    std::wstring dest = dialogs::saveVideoFile(std::wstring(widen(w.id)).append(L".mp4"));
    if (dest.empty()) return;

    Frame f;
    draw::banner(f);
    std::string et;
    et.reserve(14 + w.title.size());
    et.append("Exporting · ").append(w.title);
    draw::title(f, et);
    f.line();
    f.raw(color::cyan).raw("  Exporting (downloading first if needed)…").raw(color::reset).line();
    m_term.present(f);

    std::string err;
    if (m_library.exportMedia(w, dest, err))
        notify("Export", { { color::green, "✓ Saved to:" }, { color::gray, narrow(dest) } });
    else
        notify("Export failed", { { color::red, err } });
}

void App::importWallpaper() {
    std::wstring src = dialogs::openVideoFile();
    if (src.empty()) return;

    Frame f;
    draw::banner(f);
    draw::title(f, "Import");
    f.raw(color::cyan).raw("  Importing video…").raw(color::reset).line();
    m_term.present(f);

    std::string id, err;
    if (!m_library.importFile(src, id, err)) {
        notify("Import failed", { { color::red, err } });
        return;
    }

    const Wallpaper* added = nullptr;
    for (const Wallpaper* w : m_library.items())
        if (w->id == id) { added = w; break; }
    if (!added) {
        notify("Import", { { color::green, "✓ Added to your library." } });
        return;
    }

    Menu menu(m_term, "Imported ✓", added->title);
    menu.setItems({
        { "Apply now (whole desktop)", "" },
        { "Assign to a monitor…", "" },
        { "Just add to library", "" },
    });
    Wallpaper copy = *added;
    switch (menu.run()) {
        case 0: applyWallpaper(copy);   break;
        case 1: assignToMonitor(copy);  break;
        default: break;
    }
}

void App::perMonitorSetup() {
    while (true) {
        std::vector<MonitorInfo> monitors = enumerateMonitors();

        Menu menu(m_term, "Per-monitor setup",
                  m_config.mode == WallpaperMode::PerMonitor
                      ? "Mode: per-monitor (active)"
                      : "Mode: span — assign a screen below to switch");

        std::vector<MenuItem> items;
        for (const MonitorInfo& m : monitors) {
            std::string label;
            label.reserve(16);
            label.append("Monitor ").append(std::to_string(m.index));
            if (m.primary) label.append(" (primary)");
            auto it = m_config.monitorAssignments.find(m.device);
            std::string hint;
            if (it != m_config.monitorAssignments.end()) {
                std::wstring p = it->second;
                size_t s = p.find_last_of(L"\\/");
                hint = narrow(s == std::wstring::npos ? p : p.substr(s + 1));
            } else {
                hint = "— not set —";
            }
            items.push_back({ label, hint });
        }
        items.push_back({ "Switch to single (span) wallpaper", "" });
        items.push_back({ "Back", "" });
        menu.setItems(items);
        menu.setFooter("↑/↓ move   ⏎ select   esc back");

        int choice = menu.run();
        const int spanIndex = (int)monitors.size();
        const int backIndex = spanIndex + 1;

        if (choice == -1 || choice == backIndex) return;

        if (choice == spanIndex) {
            m_config.mode = WallpaperMode::Span;
            m_config.save();
            std::string err;
            if (m_engine.isRunning()) m_engine.restart(err);
            notify("Per-monitor setup", { { color::yellow, "Switched to single-wallpaper (span) mode." } });
            continue;
        }

        std::string pickPrompt;
        pickPrompt.reserve(36);
        pickPrompt.append("Choose a wallpaper for Monitor ").append(std::to_string(monitors[choice].index));
        int wi = pickWallpaper(pickPrompt, "It will be downloaded if needed");
        if (wi < 0 || wi >= (int)m_library.items().size()) continue;

        Wallpaper w = *m_library.items()[wi];
        std::wstring path;
        if (!prepareMedia(w, path)) continue;

        m_config.monitorAssignments[monitors[choice].device] = path;
        m_config.mode = WallpaperMode::PerMonitor;
        m_config.save();
        std::string err;
        m_engine.restart(err);
        notify("Per-monitor setup",
               { { color::green, std::string("✓ ").append(w.title).append(" set on Monitor ")
                                     .append(std::to_string(monitors[choice].index)).append(".") } });
    }
}

void App::activeWallpaper() {
    while (true) {
        const bool running = m_engine.isRunning();
        const bool haveLast = !m_config.currentMediaPath.empty() ||
                              !m_config.monitorAssignments.empty();

        std::string subtitle;
        if (running) {
            if (m_config.mode == WallpaperMode::PerMonitor) {
                subtitle = "Running: per-monitor wallpapers";
            } else {
                subtitle = "Running: ";
                subtitle += m_config.currentWallpaperId.empty() ? "custom" : m_config.currentWallpaperId;
            }
        } else {
            subtitle = "No wallpaper is running.";
        }

        Menu menu(m_term, "Active Wallpaper", subtitle);
        menu.setItems({
            { "Stop wallpaper",   "", running },
            { running ? "Restart wallpaper" : "Resume last wallpaper", "", haveLast },
            { "Default audio", m_config.muteByDefault ? "muted" : "on" },
            { "Back", "" },
        });
        menu.setFooter("↑/↓ move   ⏎ select   esc back");

        std::string err;
        switch (menu.run()) {
            case 0:
                m_engine.stop();
                notify("Active Wallpaper", { { color::yellow, "Wallpaper stopped." } });
                break;
            case 1:
                if (haveLast && m_engine.restart(err))
                    notify("Active Wallpaper",
                           { { color::green, running ? "Wallpaper restarted." : "Wallpaper resumed." } });
                else if (haveLast)
                    notify("Active Wallpaper", { { color::red, err } });
                break;
            case 2:
                m_config.muteByDefault = !m_config.muteByDefault;
                m_config.save();
                if (m_engine.isRunning()) m_engine.restart(err);
                break;
            case 3: case -1:
                return;
            default:
                break;
        }
    }
}

void App::settings() {
    while (true) {
        const bool autoOn = autostart::isEnabled();

        Menu menu(m_term, "Settings", "");
        menu.setItems({
            { "Performance",        "smoothness & battery" },
            { "Wallpapers per load", std::to_string(m_config.libraryCount) },
            { "Connect Pexels",     m_config.pexelsApiKey.empty() ? "not set" : "connected" },
            { "Start on login",     autoOn ? "enabled" : "disabled" },
            { "Default mute",       m_config.muteByDefault ? "on" : "off" },
            { "Help & about",       "" },
            { "Back", "" },
        });
        menu.setFooter("↑/↓ move   ⏎ edit   esc back");

        switch (menu.run()) {
            case 0:
                performanceSettings();
                break;
            case 1: {
                int c = m_config.libraryCount;
                m_config.libraryCount = c < 24 ? 24 : c < 48 ? 48 : 12;
                m_config.save();
                m_catalogLoaded = false;
                break;
            }
            case 2:
                connectPexels();
                break;
            case 3: {
                bool ok = autoOn ? autostart::disable() : autostart::enable();
                if (!ok)
                    notify("Settings", { { color::red, "Could not update the login setting." } });
                break;
            }
            case 4: {
                m_config.muteByDefault = !m_config.muteByDefault;
                m_config.save();
                std::string err;
                if (m_engine.isRunning()) m_engine.restart(err);
                break;
            }
            case 5:
                help();
                break;
            case 6: case -1:
                return;
            default:
                break;
        }
    }
}

void App::connectPexels() {
    Frame f;
    draw::banner(f);
    draw::title(f, "Connect Pexels");
    f.line("  Pexels gives you a huge, searchable library of free videos.");
    f.raw("  ").raw(color::gray).raw("Get a free key at pexels.com/api (takes a minute).").raw(color::reset).line();
    f.raw("  ").raw(color::gray).raw("Paste it below, or leave blank to disconnect.").raw(color::reset).line();
    f.line();
    f.raw("  ");
    m_term.present(f);

    std::string key = m_term.readLineAt(14, 3, narrow(m_config.pexelsApiKey));
    m_config.pexelsApiKey = widen(key);
    m_config.save();
    m_catalogLoaded = false;
    m_pexelsQuery.clear();
    notify("Pexels", { { key.empty() ? color::yellow : color::green,
                         key.empty() ? "Disconnected from Pexels."
                                     : "✓ Pexels key saved." } });
}

void App::performanceSettings() {
    while (true) {
        auto qName = [&] {
            switch (m_config.quality) {
                case Quality::High:   return "High (up to 4K)";
                case Quality::Medium: return "Medium (1080p)";
                case Quality::Low:    return "Low (720p)";
                default:              return "Auto (match screen)";
            }
        };

        char hw[96];
        _snprintf_s(hw, sizeof(hw), _TRUNCATE, "This PC: %s · %d cores · %d GB · %d MB VRAM",
                    tierName(m_hw), m_hw.cores, m_hw.ramGB, m_hw.vramMB);

        char speed[16];
        _snprintf_s(speed, sizeof(speed), _TRUNCATE, "%.2gx", m_config.playbackSpeed);

        char timeout[32];
        if (m_config.occlusionTimeoutSec <= 0)
            _snprintf_s(timeout, sizeof(timeout), _TRUNCATE, "never");
        else
            _snprintf_s(timeout, sizeof(timeout), _TRUNCATE, "%d sec", m_config.occlusionTimeoutSec);

        Menu menu(m_term, "Performance", hw);
        menu.setItems({
            { "Detect my PC (auto-tune)", "" },
            { "Quality",                 qName() },
            { "Pause when fullscreen",   m_config.pauseOnFullscreen ? "on" : "off" },
            { "Pause when maximized",    m_config.pauseWhenMaximized ? "on" : "off" },
            { "Pause when app focused",  m_config.pauseUnlessDesktop ? "on" : "off" },
            { "Pause on battery",        m_config.pauseOnBattery ? "on" : "off" },
            { "Low-end mode",            m_config.lowEndMode ? "on" : "off" },
            { "Playback speed",          speed },
            { "Deep sleep after",        timeout },
            { "Back", "" },
        });
        menu.setFooter("↑/↓ move   ⏎ toggle   esc back");

        std::string err;
        auto restartIfLive = [&] { if (m_engine.isRunning()) m_engine.restart(err); };

        switch (menu.run()) {
            case 0:
                autoTune(true);
                break;
            case 1:
                m_config.quality = (Quality)(((int)m_config.quality + 1) % 4);
                m_config.save();
                m_catalogLoaded = false;
                break;
            case 2:
                m_config.pauseOnFullscreen = !m_config.pauseOnFullscreen;
                m_config.save(); restartIfLive();
                break;
            case 3:
                m_config.pauseWhenMaximized = !m_config.pauseWhenMaximized;
                m_config.save(); restartIfLive();
                break;
            case 4:
                m_config.pauseUnlessDesktop = !m_config.pauseUnlessDesktop;
                m_config.save(); restartIfLive();
                break;
            case 5:
                m_config.pauseOnBattery = !m_config.pauseOnBattery;
                m_config.save(); restartIfLive();
                break;
            case 6:
                m_config.lowEndMode = !m_config.lowEndMode;
                m_config.save(); m_catalogLoaded = false; restartIfLive();
                break;
            case 7: {
                static const double steps[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
                int idx = 2;
                for (int i = 0; i < 6; ++i) if (steps[i] == m_config.playbackSpeed) idx = i;
                m_config.playbackSpeed = steps[(idx + 1) % 6];
                m_config.save(); restartIfLive();
                break;
            }
            case 8: {
                static const int steps[] = { 0, 10, 30, 60, 120, 300 };
                int idx = 0;
                for (int i = 0; i < 6; ++i) if (steps[i] == m_config.occlusionTimeoutSec) idx = i;
                m_config.occlusionTimeoutSec = steps[(idx + 1) % 6];
                m_config.save(); restartIfLive();
                break;
            }
            case 9: case -1:
                return;
            default:
                break;
        }
    }
}

}
