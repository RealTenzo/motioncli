#include "util/json.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace motion {

const Json& Json::operator[](const std::string& key) const {
    static const Json kNull;
    if (type != Type::Object) return kNull;
    auto it = object.find(key);
    return it == object.end() ? kNull : it->second;
}

const Json& Json::operator[](size_t index) const {
    static const Json kNull;
    if (type != Type::Array || index >= array.size()) return kNull;
    return array[index];
}

namespace {

struct Parser {
    const char* p;
    const char* end;

    explicit Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    [[noreturn]] void fail(const char* msg) {
        throw std::runtime_error(std::string("JSON parse error: ") + msg);
    }

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    char peek() {
        if (p >= end) fail("unexpected end of input");
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
        if (peek() != '"') fail("expected string");
        ++p;
        std::string out;
        while (p < end) {
            char c = *p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p >= end) fail("bad escape");
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
                        if (end - p < 4) fail("bad \\u escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else fail("bad hex in \\u escape");
                        }
                        encodeUtf8(cp, out);
                        break;
                    }
                    default: fail("unknown escape character");
                }
            } else {
                out.push_back(c);
            }
        }
        fail("unterminated string");
    }

    Json parseNumber() {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            ++p;
        }
        std::string token(start, p);
        try {
            Json j;
            j.type = Json::Type::Number;
            j.number = std::stod(token);
            return j;
        } catch (...) {
            fail("invalid number");
        }
    }

    void expectLiteral(const char* lit) {
        for (const char* q = lit; *q; ++q) {
            if (p >= end || *p != *q) fail("invalid literal");
            ++p;
        }
    }

    Json parseValue(int depth) {
        if (depth >= Json::kMaxDepth) fail("JSON nesting too deep");
        skipWs();
        char c = peek();
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
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            if (peek() != ':') fail("expected ':'");
            ++p;
            obj.object[key] = parseValue(depth);
            skipWs();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == '}') { ++p; break; }
            fail("expected ',' or '}'");
        }
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
    if (depth >= Json::kMaxDepth) throw std::runtime_error("JSON nesting too deep");
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
    Parser parser(text);
    Json result = parser.parseValue(0);
    parser.skipWs();
    if (parser.p != parser.end)
        throw std::runtime_error("JSON parse error: trailing characters after value");
    return result;
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(*this, out, indent, 0);
    return out;
}

}
