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

#include <algorithm>
#include <stdexcept>
#include <string>

namespace csr {

static size_t read16(const std::vector<uint8_t>& data, size_t pos) {
    return data[pos] | (data[pos + 1] << 8);
}

static bool has_signature(const std::vector<uint8_t>& data, size_t pos) {
    return pos + 4 <= data.size() && std::equal(signature, signature + 4, data.begin() + pos);
}

bool detect(const std::vector<uint8_t>& data) {

    if (has_signature(data, 0)) {
        return true;
    }

    // container: count, count offsets, end offset. the first entry sits right
    // after the table and the end offset is the file size.
    if (data.size() < 4) {
        return false;
    }

    size_t count = read16(data, 0);
    size_t table = 2 + count * 2 + 2;
    return count > 0 && table + 2 <= data.size() &&
           read16(data, 2) == table && read16(data, table - 2) == data.size();
}

// decode every frame packed into one entry's byte range [pos, end)
static void decode_entry(const std::vector<uint8_t>& data, size_t pos, size_t end,
                         int entry, File& file) {
    // MC header: signature, hotspot x, hotspot y, width / 8, height. plain records drop the signature.
    size_t head = file.mc ? 8 : 4;
    int planes = file.mc ? 4 : 0;

    while (pos < end) {

        if (end - pos < head) {
            throw std::runtime_error("truncated CSR record in entry " + std::to_string(entry));
        }

        const uint8_t* rec = &data[pos];
        size_t width_bytes = rec[head - 2];
        size_t plane_size = width_bytes * rec[head - 1];

        // plain records do not store their plane count: it is the only count (1-4) that
        // splits the entry into whole frames, which is unique for real cursor sizes
        for (int p = 1; planes == 0 && p <= 4; p++) {

            if ((end - pos) % (head + p * plane_size) == 0) {
                planes = p;
            }
        }

        size_t frame_size = head + planes * plane_size;

        if (planes == 0 || end - pos < frame_size) {
            throw std::runtime_error("CSR entry " + std::to_string(entry) + " does not split into whole frames");
        }

        Frame frame;
        frame.entry = entry;
        frame.planes = planes;
        frame.img.x = rec[head - 4];
        frame.img.y = rec[head - 3];
        frame.img.w = static_cast<uint16_t>(width_bytes * 8);
        frame.img.h = rec[head - 1];
        frame.img.pixels.resize(static_cast<size_t>(frame.img.w) * frame.img.h);

        // planes are row-major with the leftmost pixel in the high bit
        const uint8_t* bits = rec + head;

        for (size_t i = 0; i < frame.img.pixels.size(); i++) {
            uint8_t mask = 0x80 >> (i % 8);

            for (int k = 0; k < planes; k++) {

                if (bits[k * plane_size + i / 8] & mask) {
                    frame.img.pixels[i] |= 1 << k;
                }
            }
        }

        // the palette lives in the game, not the file, so preview as a gray ramp
        int levels = 1 << planes;

        for (int c = 0; c < levels; c++) {
            uint8_t g = static_cast<uint8_t>(c * 255 / (levels - 1));
            frame.img.palette[c] = {g, g, g};
        }

        file.frames.push_back(std::move(frame));
        pos += frame_size;
    }
}

File decode(const std::vector<uint8_t>& data) {
    File file;
    file.container = !has_signature(data, 0);

    if (!file.container) {
        decode_entry(data, 0, data.size(), 0, file);
        return file;
    }

    size_t count = read16(data, 0);
    file.mc = has_signature(data, read16(data, 2) + 2);

    for (size_t e = 0; e < count; e++) {
        // each entry opens with a 2 byte 00 00 prefix, then its frames run up to the
        // next offset. the last entry runs up to the end offset that follows the table.
        size_t start = read16(data, 2 + e * 2) + 2;
        size_t stop = read16(data, 4 + e * 2);

        if (start > stop || stop > data.size()) {
            throw std::runtime_error("corrupt CSR offset table");
        }

        decode_entry(data, start, stop, static_cast<int>(e), file);
    }

    return file;
}

} // namespace csr
