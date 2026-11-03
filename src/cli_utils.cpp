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

#include "cli_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ansi color helpers
// color codes: b-white=97, b-green=92, b-red=91, b-yellow=93, b-blue=94, b-cyan=96, b-magenta=95
void print_color(const std::string& color, const std::string& text) {
    std::string code;

    if (color == "b-white")        { code = "97"; }
    else if (color == "b-green")   { code = "92"; }
    else if (color == "b-red")     { code = "91"; }
    else if (color == "b-yellow")  { code = "93"; }
    else if (color == "b-blue")    { code = "94"; }
    else if (color == "b-cyan")    { code = "96"; }
    else if (color == "b-magenta") { code = "95"; }

    if (!code.empty()) {
        std::cout << "\033[" << code << "m" << text << "\033[0m";
    } else {
        std::cout << text;
    }
}

void println_color(const std::string& color, const std::string& text) {
    print_color(color, text);
    std::cout << std::endl;
}

void enable_ansi_console() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;

        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
}

bool has_extension(const std::string& path, const std::string& ext) {
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    std::string lower_ext = ext;
    std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
    return lower_path.size() >= lower_ext.size() &&
           lower_path.compare(lower_path.size() - lower_ext.size(),
                              lower_ext.size(), lower_ext) == 0;
}

std::string replace_ext(const std::string& path, const std::string& new_ext) {
    size_t dot = path.rfind('.');

    if (dot != std::string::npos) {
        return path.substr(0, dot) + new_ext;
    }

    return path + new_ext;
}

std::string get_ext(const std::string& path) {
    size_t dot = path.rfind('.');

    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);

        for (auto& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        return ext;
    }

    return "";
}

std::string get_stem(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    std::string name = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    size_t dot = name.rfind('.');

    if (dot != std::string::npos) {
        return name.substr(0, dot);
    }

    return name;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("cannot open file: " + path);
    }

    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("cannot write file: " + path);
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();
    if (!file) {
        throw std::runtime_error("cannot write file: " + path);
    }
}

// for globbing: applies multiple replacements in a single pass (first match wins at each position)
static void replace_all(std::string& s,
    const std::vector<std::pair<std::string, std::string>>& ops) {
    std::string buf;
    buf.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ) {
        bool matched = false;

        for (const auto& [needle, repl] : ops) {

            if (s.compare(i, needle.size(), needle) == 0) {
                buf += repl;
                i += needle.size();
                matched = true;
                break;
            }
        }

        if (!matched) {
            buf += s[i];
            i++;
        }
    }

    s.swap(buf);
}

std::vector<std::string> expand_globs(const std::vector<std::string>& args) {
    std::vector<std::string> result;

    for (const auto& arg : args) {

        // check if it contains glob characters
        if (arg.find('*') != std::string::npos || arg.find('?') != std::string::npos) {
            fs::path dir = fs::path(arg).parent_path();

            if (dir.empty()) {
                dir = ".";
            }

            std::string pattern = fs::path(arg).filename().string();
            // convert glob to regex: wildcards first, then escape regex metacharacters
            std::string patternGlob2Reg = pattern;
            replace_all(patternGlob2Reg, {
                {"*", ".*"},
                {"?", "."},
                {"\\", "\\\\"},
                {".", "\\."},
                {"+", "\\+"},
                {"(", "\\("},
                {")", "\\)"},
                {"[", "\\["},
                {"]", "\\]"},
                {"{", "\\{"},
                {"}", "\\}"},
                {"^", "\\^"},
                {"$", "\\$"},
                {"|", "\\|"},
            });

            std::regex re(patternGlob2Reg);

            if (fs::exists(dir) && fs::is_directory(dir)) {

                for (const auto& entry : fs::directory_iterator(dir)) {

                    if (!entry.is_regular_file()) {
                        continue;
                    }

                    std::string name = entry.path().filename().string();

                    if (std::regex_match(name, re)) {
                        result.push_back(entry.path().string());
                    }
                }
            }
        } else {
            result.push_back(arg);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}
