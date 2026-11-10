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

#include "../charset.h"

// english charset definition
// translated from _charset_english.rkt

void register_charset_english(Charset& cs) {
    // chains pc98 as base
    register_charset_pc98(cs);

    // ASCII at kuten row 9, starting at column 1
    // (charset* 9 1 #\! #\" ... #\¯)
    cs.register_kuten_range(9, 1, {
        U'!', U'"', U'#', U'$', U'%', U'&', U'\'', U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
        U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9',
        U':', U';', U'<', U'=', U'>', U'?', U'@',
        U'A', U'B', U'C', U'D', U'E', U'F', U'G', U'H', U'I', U'J', U'K', U'L', U'M',
        U'N', U'O', U'P', U'Q', U'R', U'S', U'T', U'U', U'V', U'W', U'X', U'Y', U'Z',
        U'[', U'\u00A5', U']', U'^', U'_', U'`',
        U'a', U'b', U'c', U'd', U'e', U'f', U'g', U'h', U'i', U'j', U'k', U'l', U'm',
        U'n', U'o', U'p', U'q', U'r', U's', U't', U'u', U'v', U'w', U'x', U'y', U'z',
        U'{', U'|', U'}', U'\u00AF',
    });

    // katakana at kuten row 10, starting at column 1
    // (charset* 10 1 #\。 #\「 ... #\ポ)
    cs.register_kuten_range_str(10, 1,
        u8"。「」、・ヲ"
        u8"ァィゥェォャュョッ"
        u8"ーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン"
        u8"゛゜"
        u8"ヰヱヮヵヶ"
        u8"ヴガギグゲゴザジズゼゾダヂヅデドバパビピブプベペボポ");

    // space at kuten row 11, column 1
    cs.register_kuten_range(11, 1, { U' ' });

    // punctuation at kuten row 11, column 2
    cs.register_kuten_range(11, 2, { U'\u301E', U'\u301F' });

    // various brackets and symbols at kuten row 11, column 80
    // (charset* 11 80 #\´ #\¨ #\‘ #\“ #\〔 ... #\ｰ)
    cs.register_kuten_range(11, 80, {
        U'\u00B4', U'\u00A8',         // ´ ¨
        U'\u2018', U'\u201C',         // ‘ “
        U'\u3014', U'\u3015',         // 〔 〕
        U'\u3008', U'\u3009',         // 〈 〉
        U'\u300A', U'\u300B',         // 《 》
        U'\u300E', U'\u300F',         // 『 』
        U'\u3010', U'\u3011',         // 【 】
        U'\uFF70',                     // ｰ
    });
    // (charset* 48 2 #\”)
    // (charset* 48 7 #\’)
    cs.register_kuten_range_str(48, 2, U"\u201D");
    cs.register_kuten_range_str(48, 7, U"\u2019");

    cs.set_fontwidth(1);
    cs.set_space_char(U' ');
}
