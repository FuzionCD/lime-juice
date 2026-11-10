//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "charset.h"

#include <cstring>
#include <stdexcept>

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

Charset::Charset() {}

Charset::~Charset() {
#ifdef HAVE_ICONV
    close_iconv();
#endif
}

void Charset::reset() {
    sjis_to_char_map_.clear();
    char_to_sjis_map_.clear();
    char_space_    = U'\u3000';
    char_newline_  = U'\uFF05';
    char_continue_ = U'\uFF20';
    char_break_    = U'\uFF03';
    fontwidth_ = 2;
}

void Charset::load(const std::string& name) {
    reset();

    if (name == "pc98") {
        register_charset_pc98(*this);
    } else if (name == "english") {
        register_charset_english(*this);
    } else if (name == "europe") {
        register_charset_europe(*this);
    } else if (name == "korean") {
        register_charset_korean(*this);
    } else if (name == "korean-gamebox") {
        register_charset_korean_gamebox(*this);
    } else if (name == "korean-hannuri") {
        register_charset_korean_hannuri(*this);
    } else if (name == "korean-kk") {
        register_charset_korean_kk(*this);
    } else if (name == "korean-parangsae") {
        register_charset_korean_parangsae(*this);
    } else if (name == "chinese") {
        register_charset_chinese(*this);
    } else {
        throw std::runtime_error("unsupported charset: " + name);
    }
}

// --- custom mapping registration ---

void Charset::register_mapping(const std::vector<int>& sjis_pair, char32_t unicode_char) {
    int j1 = sjis_pair[0];
    int j2 = sjis_pair.size() > 1 ? sjis_pair[1] : 0;
    uint32_t key = sjis_key(j1, j2);
    sjis_to_char_map_[key] = unicode_char;
    char_to_sjis_map_[unicode_char] = key;
}

void Charset::register_kuten_range(int ku, int ten, const std::vector<char32_t>& chars) {
    // translated from racket's (charset* k t char1 char2 ...)
    // for each char at position i:
    //   row = ku + (ten-1+i) / 94
    //   col = 1 + (ten-1+i) % 94
    for (size_t i = 0; i < chars.size(); i++) {
        int adjusted = (ten - 1) + static_cast<int>(i);
        int row = ku + adjusted / 94;
        int col = 1 + adjusted % 94;
        auto sjis = kuten_to_sjis(row, col);
        register_mapping(sjis, chars[i]);
    }
}

void Charset::register_sjis_mapping(int sjis_int, char32_t c) {
    auto sjis_pair = integer_to_sjis(sjis_int);
    register_mapping(sjis_pair, c);
}

void Charset::register_kuten_fill(int ku, int ten, int count, char32_t fill) {
    for (int i = 0; i < count; i++) {
        int adjusted = (ten - 1) + i;
        int row = ku + adjusted / 94;
        int col = 1 + adjusted % 94;
        auto sjis = kuten_to_sjis(row, col);
        register_mapping(sjis, fill);
    }
}

void Charset::register_kuten_range_str(int ku, int ten, const char* utf8_str) {
    // decode UTF-8 string to codepoints, then delegate to register_kuten_range
    std::vector<char32_t> chars;
    const auto* p = reinterpret_cast<const uint8_t*>(utf8_str);

    while (*p) {
        char32_t cp;

        if (*p < 0x80) {
            cp = *p++;
        } else if (*p < 0xE0) {
            cp = static_cast<char32_t>(*p++ & 0x1F) << 6;
            cp |= (*p++ & 0x3F);
        } else if (*p < 0xF0) {
            cp = static_cast<char32_t>(*p++ & 0x0F) << 12;
            cp |= static_cast<char32_t>(*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        } else {
            cp = static_cast<char32_t>(*p++ & 0x07) << 18;
            cp |= static_cast<char32_t>(*p++ & 0x3F) << 12;
            cp |= static_cast<char32_t>(*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        }

        chars.push_back(cp);
    }

    register_kuten_range(ku, ten, chars);
}

// --- conversion functions ---

std::optional<char32_t> Charset::sjis_to_char(const std::vector<int>& sjis_pair) const {
    int j1 = sjis_pair[0];
    int j2 = sjis_pair.size() > 1 ? sjis_pair[1] : 0;
    uint32_t key = sjis_key(j1, j2);

    // check custom mapping first
    auto it = sjis_to_char_map_.find(key);

    if (it != sjis_to_char_map_.end()) {
        return it->second;
    }

    // check for irregular or nonstandard codes
    if (!sjis_is_regular(j1, j2) || sjis_is_nonstandard(j1, j2)) {
        return std::nullopt;
    }

    // fall back to iconv if available
    int sjis_int = sjis_to_integer(sjis_pair);
    auto decoded = sjis_to_char_iconv(sjis_int);

    // CP932 has a few duplicate chars. Most will never appear in printable text,
    // but if we run into one that does just check it to make sure the encoding will
    // stay consistent.
    if (decoded.has_value() && j1 >= 0xEB && j1 <= 0xEF) {
        auto re = char_to_sjis(*decoded);

        if (!re.has_value() || sjis_to_integer(*re) != sjis_int) {
            return std::nullopt;
        }
    }

    return decoded;
}

std::optional<std::vector<int>> Charset::char_to_sjis(char32_t ch) const {
    // check custom mapping first
    auto it = char_to_sjis_map_.find(ch);

    if (it != char_to_sjis_map_.end()) {
        uint32_t key = it->second;
        int j1 = (key >> 8) & 0xFF;
        int j2 = key & 0xFF;
        return std::vector<int>{j1, j2};
    }

    // fall back to iconv if available
    return char_to_sjis_iconv(ch);
}

// --- arithmetic conversions ---

std::vector<int> Charset::kuten_to_sjis(int ku, int ten) {
    int j1, j2;

    if (ku == 0) {
        j1 = 0;
        j2 = 0;
    } else if (ku >= 1 && ku <= 62) {
        j1 = (ku + 257) / 2;
    } else {
        // ku >= 63 && ku <= 94
        j1 = (ku + 385) / 2;
    }

    if (ku == 0) {
        j2 = 0;
    } else if (ku % 2 == 0) {
        j2 = ten + 158;
    } else if (ten >= 1 && ten <= 63) {
        j2 = ten + 63;
    } else {
        // ten >= 64 && ten <= 94
        j2 = ten + 64;
    }

    return {j1, j2};
}

std::pair<int, int> Charset::sjis_to_kuten(int j1, int j2) {
    int i = (j2 <= 0x9E) ? 0 : 1;
    int k, t;

    if (j1 >= 0x81 && j1 <= 0x9F) {
        k = i + (j1 * 2 - 257);
    } else if (j1 >= 0xE0 && j1 <= 0xEF) {
        k = i + (j1 * 2 - 385);
    } else {
        k = 0;
    }

    if (k == 0) {
        t = 0;
    } else if ((k % 2 == 1) && (j2 >= 0x40 && j2 <= 0x7E)) {
        t = j2 - 63;
    } else if ((k % 2 == 1) && (j2 >= 0x80 && j2 <= 0x9E)) {
        t = j2 - 64;
    } else if ((k % 2 == 0) && (j2 >= 0x9F && j2 <= 0xFC)) {
        t = j2 - 158;
    } else {
        t = 0;
    }

    return {k, t};
}

std::pair<int, int> Charset::kuten_to_jis(int ku, int ten) {
    return {ku + 0x20, ten + 0x20};
}

std::pair<int, int> Charset::jis_to_kuten(int j1, int j2) {
    return {j1 - 0x20, j2 - 0x20};
}

int Charset::sjis_to_integer(const std::vector<int>& sjis_pair) {
    if (sjis_pair.size() < 2 || sjis_pair[1] == 0) {
        return sjis_pair[0];
    }
    return sjis_pair[0] * 0x100 + sjis_pair[1];
}

std::vector<int> Charset::integer_to_sjis(int i) {
    if (i <= 0xFF) {
        return {i, 0};
    }
    int c1 = (i >> 8) & 0xFF;
    int c2 = i & 0xFF;
    return {c1, c2};
}

// --- SJIS validation ---

bool Charset::sjis_is_regular(int j1, int j2) {
    auto [k, t] = sjis_to_kuten(j1, j2);

    // ASCII (single byte)
    if (j1 >= 0x20 && j1 <= 0x7E && j2 == 0) {
        return true;
    }

    // half-width katakana (single byte)
    if (j1 >= 0xA1 && j1 <= 0xDF && j2 == 0) {
        return true;
    }

    // double-byte SJIS
    if ((j1 >= 0x81 && j1 <= 0x9F) || (j1 >= 0xE0 && j1 <= 0xEF)) {

        if (k % 2 == 1) {
            // odd ku
            return (j2 >= 0x40 && j2 <= 0x7E) || (j2 >= 0x80 && j2 <= 0x9E);
        } else {
            // even ku
            return j2 >= 0x9F && j2 <= 0xFC;
        }
    }

    return false;
}

bool Charset::sjis_is_nonstandard(int j1, int j2) {
    auto [k, t] = sjis_to_kuten(j1, j2);
    return k >= 9 && k <= 15;
}

// --- special characters ---

void Charset::set_newline_char(char32_t c) {
    char_newline_ = c;

    // register mapping: the SJIS code for this char maps to newline
    auto sjis = char_to_sjis(c);

    if (sjis.has_value()) {
        register_mapping(*sjis, U'\n');
    }
}

void Charset::set_continue_char(char32_t c) {
    char_continue_ = c;
    auto sjis = char_to_sjis(c);

    if (sjis.has_value()) {
        register_mapping(*sjis, U'\t');
    }
}

void Charset::set_break_char(char32_t c) {
    char_break_ = c;
    auto sjis = char_to_sjis(c);

    if (sjis.has_value()) {
        register_mapping(*sjis, U'\b');
    }
}

#ifdef HAVE_ICONV
// --- iconv ---

void Charset::ensure_iconv() const {
    // use CP932 (Windows-31J), not bare SHIFT_JIS: PC-98 games target the
    // Microsoft SJIS dialect with the NEC/IBM extension rows. glibc's SHIFT_JIS
    // is only JIS X 0208 and would disagree with the Windows build's win_iconv
    // SHIFT_JIS (which is CP932). explicitly specifying CP932 gets rid of this issue.
    if (iconv_sjis_to_utf8_ == nullptr) {
        iconv_sjis_to_utf8_ = iconv_open("UTF-8", "CP932");

        if (iconv_sjis_to_utf8_ == (iconv_t)-1) {
            throw std::runtime_error("failed to open iconv CP932 -> UTF-8");
        }
    }

    if (iconv_utf8_to_sjis_ == nullptr) {
        iconv_utf8_to_sjis_ = iconv_open("CP932", "UTF-8");

        if (iconv_utf8_to_sjis_ == (iconv_t)-1) {
            throw std::runtime_error("failed to open iconv UTF-8 -> CP932");
        }
    }
}

void Charset::close_iconv() {
    if (iconv_sjis_to_utf8_ != nullptr && iconv_sjis_to_utf8_ != (iconv_t)-1) {
        iconv_close(static_cast<iconv_t>(iconv_sjis_to_utf8_));
        iconv_sjis_to_utf8_ = nullptr;
    }

    if (iconv_utf8_to_sjis_ != nullptr && iconv_utf8_to_sjis_ != (iconv_t)-1) {
        iconv_close(static_cast<iconv_t>(iconv_utf8_to_sjis_));
        iconv_utf8_to_sjis_ = nullptr;
    }
}

std::optional<char32_t> Charset::sjis_to_char_iconv(int sjis_int) const {
    ensure_iconv();

    // encode SJIS integer as bytes
    char inbuf[2];
    size_t inbytesleft;

    if (sjis_int <= 0xFF) {
        inbuf[0] = static_cast<char>(sjis_int);
        inbytesleft = 1;
    } else {
        inbuf[0] = static_cast<char>((sjis_int >> 8) & 0xFF);
        inbuf[1] = static_cast<char>(sjis_int & 0xFF);
        inbytesleft = 2;
    }

    char outbuf[6];
    size_t outbytesleft = sizeof(outbuf);
    char* outptr = outbuf;

// linux iconv uses char**, win-iconv uses const char**
#ifdef _WIN32
    const char* inptr = inbuf;
#else
    char* inptr = inbuf;
#endif

    // reset iconv state
    iconv(static_cast<iconv_t>(iconv_sjis_to_utf8_), nullptr, nullptr, nullptr, nullptr);

    size_t result = iconv(static_cast<iconv_t>(iconv_sjis_to_utf8_),
                          &inptr, &inbytesleft,
                          &outptr, &outbytesleft);

    if (result == (size_t)-1) {
        return std::nullopt;
    }

    // decode UTF-8 bytes to a single codepoint
    size_t utf8_len = sizeof(outbuf) - outbytesleft;

    if (utf8_len == 0) {
        return std::nullopt;
    }

    uint8_t* u = reinterpret_cast<uint8_t*>(outbuf);
    char32_t cp;

    if (u[0] < 0x80) {
        cp = u[0];
    } else if (u[0] < 0xE0 && utf8_len >= 2) {
        cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    } else if (u[0] < 0xF0 && utf8_len >= 3) {
        cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    } else if (utf8_len >= 4) {
        cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    } else {
        return std::nullopt;
    }

    return cp;
}

std::optional<std::vector<int>> Charset::char_to_sjis_iconv(char32_t ch) const {
    ensure_iconv();

    // encode Unicode codepoint as UTF-8
    char inbuf[6];
    size_t inbytesleft = 0;

    if (ch < 0x80) {
        inbuf[0] = static_cast<char>(ch);
        inbytesleft = 1;
    } else if (ch < 0x800) {
        inbuf[0] = static_cast<char>(0xC0 | (ch >> 6));
        inbuf[1] = static_cast<char>(0x80 | (ch & 0x3F));
        inbytesleft = 2;
    } else if (ch < 0x10000) {
        inbuf[0] = static_cast<char>(0xE0 | (ch >> 12));
        inbuf[1] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        inbuf[2] = static_cast<char>(0x80 | (ch & 0x3F));
        inbytesleft = 3;
    } else {
        inbuf[0] = static_cast<char>(0xF0 | (ch >> 18));
        inbuf[1] = static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
        inbuf[2] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        inbuf[3] = static_cast<char>(0x80 | (ch & 0x3F));
        inbytesleft = 4;
    }

    char outbuf[2];
    size_t outbytesleft = sizeof(outbuf);
    char* outptr = outbuf;

// linux iconv uses char**, win-iconv uses const char**
#ifdef _WIN32
    const char* inptr = inbuf;
#else
    char* inptr = inbuf;
#endif

    // reset iconv state
    iconv(static_cast<iconv_t>(iconv_utf8_to_sjis_), nullptr, nullptr, nullptr, nullptr);

    size_t result = iconv(static_cast<iconv_t>(iconv_utf8_to_sjis_),
                          &inptr, &inbytesleft,
                          &outptr, &outbytesleft);

    if (result == (size_t)-1) {
        return std::nullopt;
    }

    size_t sjis_len = sizeof(outbuf) - outbytesleft;

    if (sjis_len == 1) {
        return std::vector<int>{static_cast<uint8_t>(outbuf[0]), 0};
    } else if (sjis_len == 2) {
        return std::vector<int>{static_cast<uint8_t>(outbuf[0]), static_cast<uint8_t>(outbuf[1])};
    }

    return std::nullopt;
}

#else
// no iconv available, fallback functions return nullopt

std::optional<char32_t> Charset::sjis_to_char_iconv(int) const {
    return std::nullopt;
}

std::optional<std::vector<int>> Charset::char_to_sjis_iconv(char32_t) const {
    return std::nullopt;
}

#endif
