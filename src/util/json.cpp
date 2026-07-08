#include "util/json.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace motion {

thread_local const char* Json::s_lastError = nullptr;

void Json::setError(const char* msg) { s_lastError = msg; }
const char* Json::lastError() { return s_lastError; }

const Json& Json::operator[](const std::string& key) const {
    static const Json kNull;
    if (type != Type::Object) return kNull;
    for (const auto& kv : object)
        if (kv.first == key) return kv.second;
    return kNull;
}

const Json& Json::operator[](size_t index) const {
    static const Json kNull;
    if (type != Type::Array || index >= array.size()) return kNull;
    return array[index];
}

void Json::set(const std::string& key, Json value) {
    type = Type::Object;
    for (auto& kv : object) {
        if (kv.first == key) { kv.second = std::move(value); return; }
    }
    object.emplace_back(key, std::move(value));
}

void Json::sortObject() {
    std::sort(object.begin(), object.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
}

namespace {

struct Parser {
    const char* p;
    const char* end;
    bool failed = false;

    explicit Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void fail(const char* msg) {
        Json::setError(msg);
        failed = true;
        p = end;
    }

    bool ok() const { return !failed && p <= end; }

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    char peek() {
        if (p >= end) { fail("unexpected end of input"); return 0; }
        return *p;
    }

    void encodeUtf8(unsigned cp, std::string& out) {
        if (cp <= 0x7F) {
            out.push_back((char)cp);
        } else if (cp <= 0x7FF) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    std::string parseString() {
        if (peek() != '"') { fail("expected string"); return {}; }
        ++p;
        std::string out;
        while (p < end) {
            char c = *p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p >= end) { fail("bad escape"); return {}; }
                char e = *p++;
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) { fail("bad \\u escape"); return {}; }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else { fail("bad hex in \\u escape"); return {}; }
                        }
                        encodeUtf8(cp, out);
                        break;
                    }
                    default: fail("unknown escape character"); return {};
                }
            } else {
                out.push_back(c);
            }
        }
        fail("unterminated string");
        return {};
    }

    Json parseNumber() {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            ++p;
        }
        std::string token(start, p);
        char* endp = nullptr;
        double val = std::strtod(token.c_str(), &endp);
        if (endp == token.c_str() || *endp != '\0') { fail("invalid number"); return {}; }
        Json j;
        j.type = Json::Type::Number;
        j.number = val;
        return j;
    }

    void expectLiteral(const char* lit) {
        for (const char* q = lit; *q; ++q) {
            if (p >= end || *p != *q) { fail("invalid literal"); return; }
            ++p;
        }
    }

    Json parseValue(int depth) {
        if (depth >= Json::kMaxDepth) { fail("JSON nesting too deep"); return {}; }
        skipWs();
        char c = peek();
        if (p >= end) { fail("unexpected end"); return {}; }
        switch (c) {
            case '{': return parseObject(depth + 1);
            case '[': return parseArray(depth + 1);
            case '"': { Json j; j.type = Json::Type::String; j.str = parseString(); return j; }
            case 't': { expectLiteral("true");  return Json::makeBool(true); }
            case 'f': { expectLiteral("false"); return Json::makeBool(false); }
            case 'n': { expectLiteral("null");  return Json(); }
            default:  return parseNumber();
        }
    }

    Json parseObject(int depth) {
        Json obj = Json::makeObject();
        ++p;
        skipWs();
        if (p < end && *p == '}') { ++p; return obj; }
        while (ok()) {
            skipWs();
            std::string key = parseString();
            if (!ok()) break;
            skipWs();
            if (peek() != ':') { fail("expected ':'"); break; }
            ++p;
            obj.object.emplace_back(key, parseValue(depth));
            skipWs();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == '}') { ++p; break; }
            fail("expected ',' or '}'");
            break;
        }
        obj.sortObject();
        return obj;
    }

    Json parseArray(int depth) {
        Json arr = Json::makeArray();
        ++p;
        skipWs();
        if (p < end && *p == ']') { ++p; return arr; }
        while (true) {
            arr.array.push_back(parseValue(depth));
            skipWs();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == ']') { ++p; break; }
            fail("expected ',' or ']'");
            break;
        }
        return arr;
    }
};

void escapeTo(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void dumpTo(const Json& v, std::string& out, int indent, int depth) {
    auto pad = [&](int d) {
        if (indent > 0) { out.push_back('\n'); out.append((size_t)indent * d, ' '); }
    };

    switch (v.type) {
        case Json::Type::Null:   out += "null"; break;
        case Json::Type::Bool:   out += v.boolean ? "true" : "false"; break;
        case Json::Type::Number: {
            double n = v.number;
            char buf[32];
            if (n == (long long)n)
                std::snprintf(buf, sizeof(buf), "%lld", (long long)n);
            else
                std::snprintf(buf, sizeof(buf), "%g", n);
            out += buf;
            break;
        }
        case Json::Type::String: escapeTo(v.str, out); break;
        case Json::Type::Array: {
            if (v.array.empty()) { out += "[]"; break; }
            out.push_back('[');
            bool first = true;
            for (const auto& item : v.array) {
                if (!first) out.push_back(',');
                first = false;
                pad(depth + 1);
                dumpTo(item, out, indent, depth + 1);
            }
            pad(depth);
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            if (v.object.empty()) { out += "{}"; break; }
            out.push_back('{');
            bool first = true;
            for (const auto& kv : v.object) {
                if (!first) out.push_back(',');
                first = false;
                pad(depth + 1);
                escapeTo(kv.first, out);
                out += indent > 0 ? ": " : ":";
                dumpTo(kv.second, out, indent, depth + 1);
            }
            pad(depth);
            out.push_back('}');
            break;
        }
    }
}

}

Json Json::parse(const std::string& text) {
    s_lastError = nullptr;
    Parser parser(text);
    Json result = parser.parseValue(0);
    if (!parser.ok()) return {};
    parser.skipWs();
    if (parser.p != parser.end) {
        setError("JSON parse error: trailing characters after value");
        return {};
    }
    return result;
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(*this, out, indent, 0);
    return out;
}

}