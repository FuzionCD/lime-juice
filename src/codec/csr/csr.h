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

#pragma once

#include "image/image.h"

#include <cstdint>
#include <vector>

// CSR cursor/icon banks, built on the MC format notes from the "Cocktail Soft CSR
// tools" contribution. full format description in doc/csr.md.
namespace csr {

// MC records start with these 4 bytes; plain records have no signature at all
inline constexpr uint8_t signature[4] = {'M', 'C', 0x00, 0x4F};

// one bitmap. img.x and img.y hold the hotspot.
struct Frame {
    int entry = 0;  // offset table entry this frame is stored under
    int planes = 4; // bitplanes stored in the file (1-4); pixel bit k comes from plane k
    IndexedImage img;
};

struct File {
    bool container = true; // false: a single bare MC record with no offset table
    bool mc = true;         // records carry the MC signature
    std::vector<Frame> frames;
};

// true for a bare MC record or an offset table container
bool detect(const std::vector<uint8_t>& data);

// decode a CSR file; frames get a grayscale preview palette
File decode(const std::vector<uint8_t>& data);

// encode frames back into CSR file bytes
std::vector<uint8_t> encode(const File& file);

} // namespace csr
