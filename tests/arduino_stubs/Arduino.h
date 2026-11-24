// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using std::min;

struct __FlashStringHelper;
#define F(text) reinterpret_cast<const __FlashStringHelper *>(text)

class String {
public:
    String() = default;
    String(const char *text) : _text(text != nullptr ? text : "") {}
    String(const __FlashStringHelper *text) : String(reinterpret_cast<const char *>(text)) {}

    const char *c_str() const { return _text.c_str(); }
    size_t length() const { return _text.size(); }
    bool startsWith(const char *prefix) const { return _text.find(prefix) == 0; }
    bool endsWith(const char *suffix) const
    {
        const size_t size = std::strlen(suffix);
        return size <= length() && _text.compare(length() - size, size, suffix) == 0;
    }
    int indexOf(char character) const
    {
        const size_t position = _text.find(character);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    void remove(size_t start, size_t count = std::string::npos) { _text.erase(start, count); }
    void trim()
    {
        const auto isSpace = [](unsigned char value) { return std::isspace(value); };
        _text.erase(_text.begin(), std::find_if_not(_text.begin(), _text.end(), isSpace));
        _text.erase(std::find_if_not(_text.rbegin(), _text.rend(), isSpace).base(), _text.end());
    }
    bool operator==(const String &other) const { return _text == other._text; }
    bool operator!=(const String &other) const { return !(*this == other); }
    String operator+(const String &other) const { return String((_text + other._text).c_str()); }
    String &operator+=(const String &other) { _text += other._text; return *this; }
    String &operator+=(char character) { _text += character; return *this; }

private:
    std::string _text;
};

inline uint32_t micros()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

inline void delay(unsigned long milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
