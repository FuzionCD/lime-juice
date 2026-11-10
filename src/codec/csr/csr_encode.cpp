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

#include "codec/csr/csr.h"

#include <stdexcept>
#include <string>

namespace csr {

static void write16(std::vector<uint8_t>& out, size_t pos, size_t value) {
    out[pos] = static_cast<uint8_t>(value & 0xFF);
    out[pos + 1] = static_cast<uint8_t>(value >> 8);
}

static void encode_frame(std::vector<uint8_t>& out, const Frame& frame, bool mc) {
    const IndexedImage& img = frame.img;
    std::string where = "entry " + std::to_string(frame.entry) + ": ";

    if (img.w % 8 != 0 || img.w > 255 * 8 || img.h > 255) {
        throw std::runtime_error(where + "size " + std::to_string(img.w) + "x" + std::to_string(img.h) +
            " not supported (width must be a multiple of 8, at most 2040x255)");
    }

    if (img.x < 0 || img.x > 255 || img.y < 0 || img.y > 255) {
        throw std::runtime_error(where + "hotspot must be within 0-255");
    }

    if (frame.planes < 1 || frame.planes > 4 || (mc && frame.planes != 4)) {
        throw std::runtime_error(where + "MC records hold 4 planes, plain records 1-4");
    }

    if (mc) {
        out.insert(out.end(), signature, signature + 4);
    }

    size_t width_bytes = img.w / 8;
    size_t plane_size = width_bytes * img.h;
    out.push_back(static_cast<uint8_t>(img.x));
    out.push_back(static_cast<uint8_t>(img.y));
    out.push_back(static_cast<uint8_t>(width_bytes));
    out.push_back(static_cast<uint8_t>(img.h));

    size_t base = out.size();
    out.resize(base + frame.planes * plane_size, 0);

    for (size_t i = 0; i < img.pixels.size(); i++) {
        uint8_t index = img.pixels[i];

        if (index >> frame.planes) {
            throw std::runtime_error(where + "pixel index " + std::to_string(index) +
                " does not fit in " + std::to_string(frame.planes) + " plane(s)");
        }

        for (int k = 0; k < frame.planes; k++) {

            if (index & (1 << k)) {
                out[base + k * plane_size + i / 8] |= 0x80 >> (i % 8);
            }
        }
    }
}

std::vector<uint8_t> encode(const File& file) {
    std::vector<uint8_t> out;

    if (!file.container) {

        if (!file.mc || file.frames.size() != 1) {
            throw std::runtime_error("a bare CSR holds exactly one MC record");
        }

        encode_frame(out, file.frames[0], true);
        return out;
    }

    // table: count, one offset per entry, end offset. offsets point at each entry's
    // 00 00 prefix, so the first one lands right after the table.
    size_t count = file.frames.empty() ? 0 : file.frames.back().entry + 1;
    out.resize(2 + count * 2 + 2, 0);
    write16(out, 0, count);
    size_t next = 0;

    for (const auto& frame : file.frames) {

        if (static_cast<size_t>(frame.entry) == next) {
            write16(out, 2 + next * 2, out.size());
            out.push_back(0);
            out.push_back(0);
            next++;
        } else if (static_cast<size_t>(frame.entry) + 1 != next) {
            throw std::runtime_error("frame entries must count up from 0 without gaps");
        }

        encode_frame(out, frame, file.mc);
    }

    // offsets are 16-bit, so the whole bank must stay under 64KB
    if (out.size() > 0xFFFF) {
        throw std::runtime_error("CSR output is " + std::to_string(out.size()) +
            " bytes, the offset table only addresses 65535");
    }

    write16(out, 2 + count * 2, out.size());
    return out;
}

} // namespace csr
