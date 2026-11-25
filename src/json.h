// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jsn {

struct Node {
    enum Type { Null, Bool, Num, Str, Arr, Obj };

    Type type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Node> arr;
    std::vector<std::pair<std::string, Node>> obj;

    const Node* Get(std::string_view key) const {
        if (type != Obj) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    bool IsStr() const { return type == Str; }
    bool IsNum() const { return type == Num; }
    bool IsArr() const { return type == Arr; }
    bool IsObj() const { return type == Obj; }

    std::string AsStr(std::string_view fallback = "") const {
        return type == Str ? str : std::string(fallback);
    }

    double AsNum(double fallback = 0.0) const {
        return type == Num ? num : fallback;
    }

    uint64_t AsU64(uint64_t fallback = 0) const {
        if (type != Num || num < 0) return fallback;
        return static_cast<uint64_t>(num);
    }

    int64_t AsI64(int64_t fallback = 0) const {
        if (type != Num) return fallback;
        return static_cast<int64_t>(num);
    }

    bool AsBool(bool fallback = false) const {
        return type == Bool ? b : fallback;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text), n_(text.size()) {}

    bool Parse(Node& out) {
        SkipWs();
        if (!Value(out)) return false;
        SkipWs();
        return true;
    }

private:
    bool Value(Node& out) {
        if (i_ >= n_) return false;
        char c = s_[i_];
        switch (c) {
        case '{': return Object(out);
        case '[': return Array(out);
        case '"': out.type = Node::Str; return String(out.str);
        case 't':
            if (Literal("true")) { out.type = Node::Bool; out.b = true; return true; }
            return false;
        case 'f':
            if (Literal("false")) { out.type = Node::Bool; out.b = false; return true; }
            return false;
        case 'n':
            if (Literal("null")) { out.type = Node::Null; return true; }
            return false;
        default: return Number(out);
        }
    }

    bool Object(Node& out) {
        out.type = Node::Obj;
        ++i_;
        SkipWs();
        if (i_ < n_ && s_[i_] == '}') { ++i_; return true; }
        while (i_ < n_) {
            SkipWs();
            std::string key;
            if (i_ >= n_ || s_[i_] != '"' || !String(key)) return false;
            SkipWs();
            if (i_ >= n_ || s_[i_] != ':') return false;
            ++i_;
            SkipWs();
            Node val;
            if (!Value(val)) return false;
            out.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (i_ >= n_) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return false;
        }
        return false;
    }

    bool Array(Node& out) {
        out.type = Node::Arr;
        ++i_;
        SkipWs();
        if (i_ < n_ && s_[i_] == ']') { ++i_; return true; }
        while (i_ < n_) {
            SkipWs();
            Node val;
            if (!Value(val)) return false;
            out.arr.emplace_back(std::move(val));
            SkipWs();
            if (i_ >= n_) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return false;
        }
        return false;
    }

    bool String(std::string& out) {
        if (s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < n_) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i_ >= n_) return false;
            char e = s_[i_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i_ + 4 > n_) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = s_[i_ + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                    else return false;
                }
                i_ += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= n_ && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    unsigned lo = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s_[i_ + 2 + k];
                        if (h >= '0' && h <= '9') lo |= unsigned(h - '0') << (4 * (3 - k));
                        else if (h >= 'a' && h <= 'f') lo |= unsigned(h - 'a' + 10) << (4 * (3 - k));
                        else if (h >= 'A' && h <= 'F') lo |= unsigned(h - 'A' + 10) << (4 * (3 - k));
                        else { lo = 0; break; }
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i_ += 6;
                    }
                }
                Utf8(cp, out);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    static void Utf8(unsigned cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool Number(Node& out) {
        size_t start = i_;
        if (i_ < n_ && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool digits = false;
        while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; digits = true; }
        if (i_ < n_ && s_[i_] == '.') {
            ++i_;
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; digits = true; }
        }
        if (!digits) return false;
        if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < n_ && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < n_ && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        out.type = Node::Num;
        out.num = std::strtod(std::string(s_.substr(start, i_ - start)).c_str(), nullptr);
        return true;
    }

    bool Literal(const char* lit) {
        size_t len = std::strlen(lit);
        if (i_ + len > n_ || s_.compare(i_, len, lit) != 0) return false;
        i_ += len;
        return true;
    }

    void SkipWs() {
        while (i_ < n_) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    std::string_view s_;
    size_t n_;
    size_t i_ = 0;
};

class Writer {
public:
    Writer& BeginObject() { Sep(); out_.push_back('{'); stack_.push_back(1); return *this; }
    Writer& EndObject() { if (!stack_.empty()) stack_.pop_back(); out_.push_back('}'); return *this; }
    Writer& BeginArray() { Sep(); out_.push_back('['); stack_.push_back(1); return *this; }
    Writer& EndArray() { if (!stack_.empty()) stack_.pop_back(); out_.push_back(']'); return *this; }

    Writer& Key(std::string_view k) {
        Sep();
        Quote(k);
        out_.push_back(':');
        pending_ = true;
        return *this;
    }

    Writer& Str(std::string_view v) { Sep(); Quote(v); return *this; }
    Writer& Bool(bool v) { Sep(); out_ += v ? "true" : "false"; return *this; }

    Writer& Num(double v) {
        Sep();
        if (v == static_cast<double>(static_cast<int64_t>(v))) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            out_ += buf;
        } else {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            out_ += buf;
        }
        return *this;
    }

    Writer& Raw(std::string_view v) { Sep(); out_ += v; return *this; }
    Writer& U64(uint64_t v) { Sep(); out_ += std::to_string(v); return *this; }
    Writer& I64(int64_t v) { Sep(); out_ += std::to_string(v); return *this; }
    Writer& Null() { Sep(); out_ += "null"; return *this; }

    const std::string& Str() const { return out_; }
    std::string Take() { return std::move(out_); }

private:
    void Sep() {
        if (pending_) { pending_ = false; return; }
        if (stack_.empty()) return;
        if (stack_.back()) stack_.back() = 0;
        else out_.push_back(',');
    }

    void Quote(std::string_view v) {
        out_.push_back('"');
        for (char c : v) {
            switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\b': out_ += "\\b"; break;
            case '\f': out_ += "\\f"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out_ += buf;
                } else {
                    out_.push_back(c);
                }
            }
        }
        out_.push_back('"');
    }

    std::string out_;
    std::vector<char> stack_;
    bool pending_ = false;
};

} // namespace jsn
