#pragma once
/**
 * @file mini_json.hpp
 * @brief Minimális, függőségmentes JSON olvasó a GUI -> motor vezérlőüzenetekhez.
 */
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>
#include <cctype>

namespace ee_analyzer::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value* get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    double numberOr(double def) const {
        if (type == Type::Number) return num;
        if (type == Type::Bool) return b ? 1.0 : 0.0;
        if (type == Type::String) return std::atof(str.c_str());
        return def;
    }
    std::string stringOr(const std::string& def) const { return type == Type::String ? str : def; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Value& out) {
        pos_ = 0;
        if (!value(out, 0)) return false;
        ws();
        return pos_ == s_.size();
    }

private:
    void ws() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    bool lit(const char* l) {
        size_t n = std::char_traits<char>::length(l);
        if (s_.compare(pos_, n, l) != 0) return false;
        pos_ += n;
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > 32) return false;
        ws();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') { v.type = Value::Type::String; return string(v.str); }
        if (lit("true")) { v.type = Value::Type::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Value::Type::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Value::Type::Null; return true; }
        return number(v);
    }
    bool number(Value& v) {
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        v.num = std::strtod(begin, &end);
        if (end == begin) return false;
        pos_ += static_cast<size_t>(end - begin);
        v.type = Value::Type::Number;
        return true;
    }
    bool string(std::string& out) {
        ++pos_; // "
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char e = s_[pos_++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned cp = static_cast<unsigned>(std::strtoul(s_.substr(pos_, 4).c_str(), nullptr, 16));
                        pos_ += 4;
                        // UTF-8 kódolás (BMP)
                        if (cp < 0x80) out.push_back(static_cast<char>(cp));
                        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                        else { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }
    bool array(Value& v, int depth) {
        ++pos_;
        v.type = Value::Type::Array;
        ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            Value item;
            if (!value(item, depth + 1)) return false;
            v.arr.push_back(std::move(item));
            ws();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return false;
        }
    }
    bool object(Value& v, int depth) {
        ++pos_;
        v.type = Value::Type::Object;
        ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::string key;
            if (!string(key)) return false;
            ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            Value item;
            if (!value(item, depth + 1)) return false;
            v.obj[key] = std::move(item);
            ws();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
};

inline bool parse(const std::string& text, Value& out) {
    Parser p(text);
    return p.parse(out);
}

} // namespace ee_analyzer::json
