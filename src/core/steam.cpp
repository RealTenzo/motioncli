#include "core/steam.h"
#include "net/http.h"
#include "util/str.h"

#include <windows.h>
#include <cstring>
#include <string_view>

namespace motion::steam {

namespace {

using motion::htmlDecode;
using motion::trim;

std::string_view attrAfter(const std::string& html, size_t from, const char* key) {
    size_t k = html.find(key, from);
    if (k == std::string::npos) return {};
    size_t q = html.find('"', k + strlen(key) - 1);
    if (q == std::string::npos) return {};
    size_t e = html.find('"', q + 1);
    if (e == std::string::npos) return {};
    return std::string_view(html).substr(q + 1, e - q - 1);
}

std::string digitsAfter(const std::string& html, size_t pos) {
    std::string out;
    while (pos < html.size() && html[pos] >= '0' && html[pos] <= '9') out.push_back(html[pos++]);
    return out;
}

}

bool fetchTrending(const std::string& sort, std::vector<Item>& out, std::string& err) {
    std::string s = sort.empty() ? "trend" : sort;
    std::wstring url = L"https://steamcommunity.com/workshop/browse/?appid=431960"
                       L"&browsesort=" + widen(s) + L"&section=readytouseitems&actualsort=" + widen(s);

    std::string html;
    if (!http::getString(url, html, err)) return false;

    out.clear();
    const std::string key = "filedetails/?id=";
    size_t p = 0;

    auto seen = [&](const std::string& id) {
        for (const Item& it : out) if (it.id == id) return true;
        return false;
    };
    auto quoted = [&](size_t q) -> std::string_view {
        q += 5;
        size_t e = html.find('"', q);
        return e == std::string::npos ? std::string_view() : std::string_view(html).substr(q, e - q);
    };

    while ((p = html.find(key, p)) != std::string::npos) {
        size_t idStart = p + key.size();
        std::string id = digitsAfter(html, idStart);
        p = idStart;
        if (id.empty() || seen(id)) continue;

        size_t winEnd = idStart + 720 < html.size() ? idStart + 720 : html.size();
        size_t img = html.find("<img", idStart);
        if (img == std::string::npos || img > winEnd) continue;

        size_t sp = html.find("src=\"", img);
        size_t ap = html.find("alt=\"", img);
        if (sp == std::string::npos || ap == std::string::npos || sp > winEnd || ap > winEnd)
            continue;

        std::string title = trim(htmlDecode(std::string(quoted(ap))));
        if (title.empty()) continue;

        Item item;
        item.id = id;
        item.title = title;
        item.preview = std::string(quoted(sp));
        item.author = "Steam Workshop";
        out.push_back(std::move(item));
        if (out.size() >= 60) break;
    }

    if (out.empty()) { err = "No workshop items found (Steam may have changed its page)"; return false; }
    return true;
}

bool resolveVideoUrl(const std::string& id, std::wstring& outUrl, std::string& err) {
    std::wstring url = L"https://steamcommunity.com/sharedfiles/filedetails/?id=" + widen(id);
    std::string html;
    if (!http::getString(url, html, err)) return false;

    const char* keys[] = { "data-mp4-hd-source=\"", "data-mp4-source=\"",
                           "og:video:secure_url\" content=\"", "og:video\" content=\"" };
    for (const char* k : keys) {
        std::string_view v = attrAfter(html, 0, k);
        if (v.find(".mp4") != std::string::npos) {
            std::string clean;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '/') { clean.push_back('/'); ++i; }
                else clean.push_back(v[i]);
            }
            size_t amp;
            while ((amp = clean.find("&amp;")) != std::string::npos) clean.replace(amp, 5, "&");
            outUrl = widen(clean);
            return true;
        }
    }

    size_t h = html.find("https://");
    while (h != std::string::npos) {
        size_t e = html.find_first_of("\"'\\ ", h);
        std::string cand = html.substr(h, (e == std::string::npos ? html.size() : e) - h);
        if (cand.find(".mp4") != std::string::npos) { outUrl = widen(cand); return true; }
        h = html.find("https://", h + 8);
    }

    err = "This wallpaper has no downloadable video (needs Wallpaper Engine).";
    return false;
}

}
