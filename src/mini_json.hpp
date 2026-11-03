#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fpkgi {

class Json {
public:
    enum Type { Null, Boolean, Number, String, Array, Object };

    Json() : type_(Null), boolean_(false), number_(0.0) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Null; }
    bool is_bool() const { return type_ == Boolean; }
    bool is_number() const { return type_ == Number; }
    bool is_string() const { return type_ == String; }
    bool is_array() const { return type_ == Array; }
    bool is_object() const { return type_ == Object; }

    bool as_bool() const { require(Boolean); return boolean_; }
    double as_number() const { require(Number); return number_; }
    const std::string& as_string() const { require(String); return string_; }
    const std::vector<Json>& as_array() const { require(Array); return array_; }
    const std::map<std::string, Json>& as_object() const { require(Object); return object_; }

    const Json* get(const std::string& key) const {
        if (type_ != Object) return 0;
        std::map<std::string, Json>::const_iterator it = object_.find(key);
        return it == object_.end() ? 0 : &it->second;
    }

    static Json make_bool(bool value) { Json j; j.type_ = Boolean; j.boolean_ = value; return j; }
    static Json make_number(double value) { Json j; j.type_ = Number; j.number_ = value; return j; }
    static Json make_string(const std::string& value) { Json j; j.type_ = String; j.string_ = value; return j; }
    static Json make_array(const std::vector<Json>& value) { Json j; j.type_ = Array; j.array_ = value; return j; }
    static Json make_object(const std::map<std::string, Json>& value) { Json j; j.type_ = Object; j.object_ = value; return j; }

private:
    void require(Type expected) const {
        if (type_ != expected) throw std::runtime_error("JSON value has unexpected type");
    }

    Type type_;
    bool boolean_;
    double number_;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input)
        : input_(input), pos_(0), depth_(0), nodes_(0) {}

    Json parse() {
        skip_ws();
        Json value = parse_value();
        skip_ws();
        if (pos_ != input_.size()) error("trailing data");
        return value;
    }

private:
    enum { kMaxDepth = 16, kMaxNodes = 65536 };

    Json parse_value() {
        if (pos_ >= input_.size()) error("unexpected end of input");
        if (++nodes_ > kMaxNodes) error("JSON node limit exceeded");
        const char c = input_[pos_];
        if (c == '{' || c == '[') {
            if (depth_ >= kMaxDepth) error("JSON nesting limit exceeded");
            ++depth_;
            Json value = c == '{' ? parse_object() : parse_array();
            --depth_;
            return value;
        }
        if (c == '"') return Json::make_string(parse_string());
        if (c == 't') { consume_literal("true"); return Json::make_bool(true); }
        if (c == 'f') { consume_literal("false"); return Json::make_bool(false); }
        if (c == 'n') { consume_literal("null"); return Json(); }
        if (c == '-' || (c >= '0' && c <= '9')) return Json::make_number(parse_number());
        error("unexpected character");
        return Json();
    }

    Json parse_object() {
        expect('{');
        skip_ws();
        std::map<std::string, Json> object;
        if (peek('}')) { ++pos_; return Json::make_object(object); }
        for (;;) {
            skip_ws();
            if (!peek('"')) error("object key must be a string");
            const std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            if (object.find(key) != object.end()) error("duplicate object key");
            object.insert(std::make_pair(key, parse_value()));
            skip_ws();
            if (peek('}')) { ++pos_; break; }
            expect(',');
        }
        return Json::make_object(object);
    }

    Json parse_array() {
        expect('[');
        skip_ws();
        std::vector<Json> array;
        if (peek(']')) { ++pos_; return Json::make_array(array); }
        for (;;) {
            array.push_back(parse_value());
            skip_ws();
            if (peek(']')) { ++pos_; break; }
            expect(',');
            skip_ws();
        }
        return Json::make_array(array);
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                if (!valid_utf8(out)) error("invalid UTF-8 in string");
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) error("control character in string");
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= input_.size()) error("incomplete escape sequence");
            const char esc = input_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': append_unicode(out, parse_unicode_escape()); break;
                default: error("invalid escape sequence");
            }
        }
        error("unterminated string");
        return out;
    }

    unsigned parse_hex4() {
        if (pos_ + 4 > input_.size()) error("incomplete unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else error("invalid unicode escape");
        }
        return value;
    }

    unsigned parse_unicode_escape() {
        const unsigned high = parse_hex4();
        if (high >= 0xD800 && high <= 0xDBFF) {
            if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u')
                error("high surrogate without a low surrogate");
            pos_ += 2;
            const unsigned low = parse_hex4();
            if (low < 0xDC00 || low > 0xDFFF) error("invalid low surrogate");
            return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
        }
        if (high >= 0xDC00 && high <= 0xDFFF) error("unexpected low surrogate");
        return high;
    }

    static bool valid_utf8(const std::string& value) {
        size_t i = 0;
        while (i < value.size()) {
            const unsigned char lead = static_cast<unsigned char>(value[i]);
            if (lead <= 0x7F) {
                ++i;
                continue;
            }

            size_t continuation = 0;
            if (lead >= 0xC2 && lead <= 0xDF) continuation = 1;
            else if (lead >= 0xE0 && lead <= 0xEF) continuation = 2;
            else if (lead >= 0xF0 && lead <= 0xF4) continuation = 3;
            else return false;
            if (i + continuation >= value.size()) return false;

            const unsigned char second = static_cast<unsigned char>(value[i + 1]);
            if ((lead == 0xE0 && second < 0xA0) ||
                (lead == 0xED && second > 0x9F) ||
                (lead == 0xF0 && second < 0x90) ||
                (lead == 0xF4 && second > 0x8F)) return false;
            for (size_t j = 1; j <= continuation; ++j)
                if ((static_cast<unsigned char>(value[i + j]) & 0xC0) != 0x80) return false;
            i += continuation + 1;
        }
        return true;
    }

    static void append_unicode(std::string& out, unsigned cp) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    double parse_number() {
        const size_t start = pos_;
        if (peek('-')) ++pos_;
        if (peek('0')) ++pos_;
        else {
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) error("invalid number");
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (peek('.')) {
            ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) error("invalid fraction");
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (peek('e') || peek('E')) {
            ++pos_;
            if (peek('+') || peek('-')) ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) error("invalid exponent");
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        const std::string text = input_.substr(start, pos_ - start);
        char* end = 0;
        const double value = std::strtod(text.c_str(), &end);
        if (!end || *end != '\0') error("invalid number");
        return value;
    }

    void consume_literal(const char* literal) {
        while (*literal) {
            if (pos_ >= input_.size() || input_[pos_] != *literal) error("invalid literal");
            ++pos_;
            ++literal;
        }
    }

    void skip_ws() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    bool peek(char c) const { return pos_ < input_.size() && input_[pos_] == c; }

    void expect(char c) {
        if (!peek(c)) {
            std::string message = "expected '";
            message.push_back(c);
            message.push_back('\'');
            error(message);
        }
        ++pos_;
    }

    void error(const std::string& message) const {
        std::ostringstream out;
        out << "JSON parse error at byte " << pos_ << ": " << message;
        throw std::runtime_error(out.str());
    }

    const std::string& input_;
    size_t pos_;
    size_t depth_;
    size_t nodes_;
};

inline Json parse_json(const std::string& input) {
    return JsonParser(input).parse();
}

} // namespace fpkgi
