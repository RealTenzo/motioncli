#include "core/moewalls.h"
#include "net/http.h"
#include "util/str.h"

#include <windows.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string_view>

namespace motion::moewalls {

namespace {

std::string htmlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)   { out.push_back('&');  i += 5; continue; }
            if (s.compare(i, 6, "&#038;") == 0)  { out.push_back('&');  i += 6; continue; }
            if (s.compare(i, 4, "&lt;") == 0)    { out.push_back('<');  i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0)    { out.push_back('>');  i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0)  { out.push_back('"');  i += 6; continue; }
            if (s.compare(i, 5, "&#39;") == 0)   { out.push_back('\''); i += 5; continue; }
            if (s.compare(i, 7, "&#8217;") == 0) { out.push_back('\''); i += 7; continue; }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else if (c == ' ') out += "%20";
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
    }
    return out;
}

std::string_view quotedAfter(const std::string& html, size_t from, const char* key) {
    size_t k = html.find(key, from);
    if (k == std::string::npos) return {};
    size_t q = html.find('"', k + std::strlen(key) - 1);
    if (q == std::string::npos) return {};
    size_t e = html.find('"', q + 1);
    if (e == std::string::npos) return {};
    return std::string_view(html).substr(q + 1, e - q - 1);
}

std::string_view metaContent(const std::string& html, const char* prop) {
    std::string needle = std::string("property=\"") + prop + "\"";
    size_t p = html.find(needle);
    if (p == std::string::npos) return {};
    return quotedAfter(html, p, "content=\"");
}

std::string slugFromUrl(const std::string& url) {
    std::string u = url;
    if (!u.empty() && u.back() == '/') u.pop_back();
    size_t s = u.find_last_of('/');
    std::string slug = (s == std::string::npos) ? u : u.substr(s + 1);
    if (endsWith(slug, "-live-wallpaper")) slug.resize(slug.size() - 15);
    return slug;
}

std::string prettify(const std::string& slug) {
    std::string out;
    bool start = true;
    for (char c : slug) {
        if (c == '-') { out.push_back(' '); start = true; }
        else { out.push_back(start ? (char)toupper((unsigned char)c) : c); start = false; }
    }
    return out;
}

void collectPostUrls(const std::string& html, const std::string& category,
                     std::vector<std::string>& urls, std::set<std::string>& seen) {
    const std::string open = "href=\"https://moewalls.com/";
    const std::string need = category.empty()
        ? std::string() : "https://moewalls.com/" + category + "/";

    size_t p = 0;
    while ((p = html.find(open, p)) != std::string::npos) {
        size_t urlStart = p + std::string("href=\"").size();
        size_t e = html.find('"', urlStart);
        if (e == std::string::npos) { p = urlStart; continue; }
        std::string url = html.substr(urlStart, e - urlStart);
        p = e + 1;
        if (!endsWith(url, "-live-wallpaper/")) continue;
        if (!need.empty() && url.compare(0, need.size(), need) != 0) continue;
        if (seen.insert(url).second) urls.push_back(url);
    }
}

}

bool fetchListing(const std::string& query, const std::string& category,
                  int maxItems, std::vector<Item>& out, std::string& err) {
    out.clear();
    if (maxItems <= 0) maxItems = 24;

    std::set<std::string> seen;
    std::vector<std::string> postUrls;
    const std::string q = urlEncode(trim(query));
    const int maxPages = 40;

    for (int page = 1; page <= maxPages && (int)postUrls.size() < maxItems; ++page) {
        std::string path;
        if (!q.empty())
            path = (page == 1) ? "https://moewalls.com/?s=" + q
                               : "https://moewalls.com/page/" + std::to_string(page) + "/?s=" + q;
        else if (category.empty())
            path = (page == 1) ? "https://moewalls.com/"
                               : "https://moewalls.com/page/" + std::to_string(page) + "/";
        else
            path = (page == 1) ? "https://moewalls.com/category/" + category + "/"
                               : "https://moewalls.com/category/" + category +
                                 "/page/" + std::to_string(page) + "/";

        std::string html, e2;
        if (!http::getString(widen(path), html, e2)) {
            if (page == 1) { err = e2; return false; }
            break;
        }
        collectPostUrls(html, q.empty() ? category : std::string(), postUrls, seen);
        if ((int)postUrls.size() >= maxItems) break;
    }

    if (postUrls.empty()) {
        err = query.empty() ? "No wallpapers found" : "No results for \"" + query + "\"";
        return false;
    }

    for (const std::string& purl : postUrls) {
        if ((int)out.size() >= maxItems) break;
        Item it;
        it.id = slugFromUrl(purl);
        it.title = prettify(it.id);
        it.postUrl = purl;
        out.push_back(std::move(it));
    }
    return true;
}

bool resolve(Item& it, std::string& err) {
    if (!it.url.empty()) return true;
    if (it.postUrl.empty()) { err = "No source page"; return false; }

    std::string html;
    if (!http::getString(widen(it.postUrl), html, err)) return false;

    size_t btn = html.find("id=\"moe-download\"");
    if (btn == std::string::npos) { err = "No download link on the page"; return false; }
    std::string token = std::string(quotedAfter(html, btn, "data-url=\""));
    if (token.empty()) { err = "No download link on the page"; return false; }
    it.url = "https://go.moewalls.com/download.php?video=" + token;

    std::string title = trim(htmlDecode(std::string(metaContent(html, "og:title"))));
    const std::string brand = " - MoeWalls";
    if (endsWith(title, brand)) title.resize(title.size() - brand.size());
    const std::string lw = " Live Wallpaper";
    if (endsWith(title, lw)) title.resize(title.size() - lw.size());
    title = trim(title);
    if (!title.empty()) it.title = title;

    it.preview = std::string(metaContent(html, "og:image"));

    std::string w = std::string(metaContent(html, "og:image:width"));
    std::string h = std::string(metaContent(html, "og:image:height"));
    if (!w.empty() && !h.empty()) { it.resolution = w + "x" + h; it.width = std::atoi(w.c_str()); }

    size_t pv = html.find("/wp-content/uploads/preview/");
    if (pv != std::string::npos) {
        size_t qo = html.rfind('"', pv);
        size_t e = html.find('"', pv);
        if (qo != std::string::npos && e != std::string::npos && e > pv) {
            std::string_view rel = std::string_view(html).substr(qo + 1, e - qo - 1);
            if (!rel.empty())
                it.previewVideo = rel.compare(0, 4, "http") == 0
                                      ? std::string(rel) : "https://moewalls.com" + std::string(rel);
        }
    }

    size_t ts = html.find("class=\"tag-items\"");
    if (ts != std::string::npos) {
        size_t te = html.find("</div>", ts);
        std::string_view block = std::string_view(html).substr(ts, (te == std::string::npos ? html.size() : te) - ts);
        const char* mark = "rel=\"tag\">";
        size_t p = 0;
        while ((p = block.find(mark, p)) != std::string::npos) {
            p += std::strlen(mark);
            size_t e = block.find("</a>", p);
            if (e == std::string::npos) break;
            std::string tag = trim(htmlDecode(std::string(block.substr(p, e - p))));
            if (!tag.empty()) it.tags.push_back(tag);
            p = e + 4;
            if (it.tags.size() >= 6) break;
        }
    }

    return true;
}

}
