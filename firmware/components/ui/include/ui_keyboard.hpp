#pragma once
#include <array>
#include <string>
#include <cstddef>

namespace dh::ui {
// Bounded ASCII editor. No heap buffer keeps a second editable copy of a password.
class Keyboard {
    std::array<char, 65> bytes_{};
    size_t length_ = 0;
    bool password_ = true, revealed_ = false;
    bool hex() const {
        for (size_t i = 0; i < length_; ++i) {
            const char c = bytes_[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
public:
    ~Keyboard() { clear(); }
    void clear() {
        volatile char *p = bytes_.data();
        for (size_t i = 0; i < bytes_.size(); ++i) p[i] = 0;
        length_ = 0; revealed_ = false;
    }
    void reset(bool password) { clear(); password_ = password; }
    bool append(char c) {
        if (c < 32 || c > 126 || length_ >= (password_ ? 64u : 32u)) return false;
        if (password_ && length_ == 63 && (!hex() || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))) return false;
        bytes_[length_++] = c; bytes_[length_] = 0; return true;
    }
    void erase() { if (length_) bytes_[--length_] = 0; }
    bool valid() const { return password_ ? (length_ >= 8 && (length_ <= 63 || (length_ == 64 && hex()))) : length_ >= 1; }
    bool password() const { return password_; }
    size_t size() const { return length_; }
    size_t missing() const { return password_ && length_ < 8 ? 8 - length_ : 0; }   // how many until a password is long enough
    size_t limit() const { return password_ ? (length_ == 64 ? 64 : 63) : 32; }      // what the count says: 64 only for a full hex key
    bool full() const { return password_ ? length_ == 64 || (length_ == 63 && !hex()) : length_ == 32; }
    void reveal(bool on) { revealed_ = password_ && on; }
    std::string display() const { return password_ && !revealed_ ? std::string(length_, '*') : value(); }
    std::string value() const { return std::string(bytes_.data(), length_); }
    static std::array<std::string, 4> rows(int page) {
        switch (page) {
        case 1: return {"ABCDEFG", "HIJKLMN", "OPQRSTU", "VWXYZ"};
        case 2: return {"1234567", "890-_.@", "!#$%&*+", "?=/:;"};
        case 3: return {"()[]{}<", ">\"',`~\\", "|^", ""};
        default: return {"abcdefg", "hijklmn", "opqrstu", "vwxyz"};
        }
    }
};
}
