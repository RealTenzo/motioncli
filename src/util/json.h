#pragma once
#include <string>
#include <vector>
#include <map>

namespace motion {

class Json {
public:
    static constexpr int kMaxDepth = 256;
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool   boolean = false;
    double number  = 0.0;
    std::string str;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    Json() = default;

    static Json makeObject() { Json j; j.type = Type::Object; return j; }
    static Json makeArray()  { Json j; j.type = Type::Array;  return j; }
    static Json makeString(std::string s) { Json j; j.type = Type::String; j.str = std::move(s); return j; }
    static Json makeNumber(double n) { Json j; j.type = Type::Number; j.number = n; return j; }
    static Json makeBool(bool b) { Json j; j.type = Type::Bool; j.boolean = b; return j; }

    bool isNull()   const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray()  const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }

    const Json& operator[](const std::string& key) const;
    const Json& operator[](size_t index) const;

    std::string asString(const std::string& def = "") const { return type == Type::String ? str : def; }
    double      asNumber(double def = 0.0)            const { return type == Type::Number ? number : def; }
    int         asInt(int def = 0)                    const { return type == Type::Number ? (int)number : def; }
    bool        asBool(bool def = false)              const { return type == Type::Bool ? boolean : def; }

    void set(const std::string& key, Json value) { type = Type::Object; object[key] = std::move(value); }

    static Json parse(const std::string& text);
    std::string dump(int indent = 2) const;
};

}
