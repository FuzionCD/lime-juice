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

// pc-98 charset definition
// translated from _charset_pc98.rkt
//
// to add characters: register_sjis_mapping(sjis_int, U'x') maps a single SJIS
// code to a unicode char (for encode and decode), register_kuten_range maps a whole
// block at once.
//
// Important! if you're using the ADV engine and you add a character whose SJIS
// lead byte is in the 0xEB-0xEF range, it decodes in normal text but stays raw
// inside (str) strings. to force those to decode too, bypass the sjis2+/gaiji path
// by removing the '+' from the 'chr-sjis2+' list call in parse_chrs_escaped() in
// src/engine/adv/parser.cpp.
// Note: May cause character encoding problems. hack at your own risk.
//
// don't map a character that already has a standard SJIS code (like ・ at 0x8145):
// mappings work both ways, so every use of it would then encode to your new code.

void register_charset_pc98(Charset& cs) {
    // box drawing characters at kuten row 12, starting at column 4
    // (charset* 12 4 #\─ #\━ #\│ #\┃ ...)
    cs.register_kuten_range(12, 4, {
        U'\u2500', U'\u2501', U'\u2502', U'\u2503', // ─ ━ │ ┃
        U'\u2504', U'\u2505', U'\u2506', U'\u2507', // ┄ ┅ ┆ ┇
        U'\u2508', U'\u2509', U'\u250A', U'\u250B', // ┈ ┉ ┊ ┋
        U'\u250C', U'\u250D', U'\u250E', U'\u250F', // ┌ ┍ ┎ ┏
        U'\u2510', U'\u2511', U'\u2512', U'\u2513', // ┐ ┑ ┒ ┓
        U'\u2514', U'\u2515', U'\u2516', U'\u2517', // └ ┕ ┖ ┗
        U'\u2518', U'\u2519', U'\u251A', U'\u251B', // ┘ ┙ ┚ ┛
        U'\u251C', U'\u251D', U'\u251E', U'\u251F', // ├ ┝ ┞ ┟
        U'\u2520', U'\u2521', U'\u2522', U'\u2523', // ┠ ┡ ┢ ┣
        U'\u2524', U'\u2525', U'\u2526', U'\u2527', // ┤ ┥ ┦ ┧
        U'\u2528', U'\u2529', U'\u252A', U'\u252B', // ┨ ┩ ┪ ┫
        U'\u252C', U'\u252D', U'\u252E', U'\u252F', // ┬ ┭ ┮ ┯
        U'\u2530', U'\u2531', U'\u2532', U'\u2533', // ┰ ┱ ┲ ┳
        U'\u2534', U'\u2535', U'\u2536', U'\u2537', // ┴ ┵ ┶ ┷
        U'\u2538', U'\u2539', U'\u253A', U'\u253B', // ┸ ┹ ┺ ┻
        U'\u253C', U'\u253D', U'\u253E', U'\u253F', // ┼ ┽ ┾ ┿
        U'\u2540', U'\u2541', U'\u2542', U'\u2543', // ╀ ╁ ╂ ╃
        U'\u2544', U'\u2545', U'\u2546', U'\u2547', // ╄ ╅ ╆ ╇
        U'\u2548', U'\u2549', U'\u254A', U'\u254B', // ╈ ╉ ╊ ╋
    });

    // circled numbers at kuten row 13, starting at column 1
    // (charset* 13 1 #\① #\② ... #\⑳)
    cs.register_kuten_range(13, 1, {
        U'\u2460', U'\u2461', U'\u2462', U'\u2463', // ① ② ③ ④
        U'\u2464', U'\u2465', U'\u2466', U'\u2467', // ⑤ ⑥ ⑦ ⑧
        U'\u2468', U'\u2469', U'\u246A', U'\u246B', // ⑨ ⑩ ⑪ ⑫
        U'\u246C', U'\u246D', U'\u246E', U'\u246F', // ⑬ ⑭ ⑮ ⑯
        U'\u2470', U'\u2471', U'\u2472', U'\u2473', // ⑰ ⑱ ⑲ ⑳
    });

    // roman numerals at kuten row 13, starting at column 21
    // (charset* 13 21 #\Ⅰ #\Ⅱ ... #\Ⅹ)
    cs.register_kuten_range(13, 21, {
        U'\u2160', U'\u2161', U'\u2162', U'\u2163', // Ⅰ Ⅱ Ⅲ Ⅳ
        U'\u2164', U'\u2165', U'\u2166', U'\u2167', // Ⅴ Ⅵ Ⅶ Ⅷ
        U'\u2168', U'\u2169',                         // Ⅸ Ⅹ
    });

    // unit symbols at kuten row 13, starting at column 32
    // (charset* 13 32 #\㍉ #\㌔ #\㌢ ...)
    cs.register_kuten_range(13, 32, {
        U'\u3349', U'\u3314', U'\u3322', U'\u334D', // ㍉ ㌔ ㌢ ㍍
        U'\u3318', U'\u3327', U'\u3303', U'\u3336', // ㌘ ㌧ ㌃ ㌶
        U'\u3351', U'\u3357', U'\u330D', U'\u3326', // ㍑ ㍗ ㌍ ㌦
        U'\u3323', U'\u332B', U'\u334A', U'\u333B', // ㌣ ㌫ ㍊ ㌻
        U'\u339C', U'\u339D', U'\u339E', U'\u338E', // ㎜ ㎝ ㎞ ㎎
        U'\u338F', U'\u33C4', U'\u33A1',             // ㎏ ㏄ ㎡
    });

    // era name at kuten row 13, column 63
    // (charset* 13 63 #\㍻)
    cs.register_kuten_range(13, 63, {
        U'\u337B',                                    // ㍻
    });

    // special symbols at kuten row 13, starting at column 66
    // (charset* 13 66 #\〝 #\〟 #\№ ...)
    cs.register_kuten_range(13, 66, {
        U'\u301D', U'\u301F', U'\u2116', U'\u33CD', // 〝 〟 № ㏍
        U'\u2121', U'\u32A4', U'\u32A5', U'\u32A6', // ℡ ㊤ ㊥ ㊦
        U'\u32A7', U'\u32A8', U'\u3231', U'\u3232', // ㊧ ㊨ ㈱ ㈲
        U'\u3239', U'\u337E', U'\u337D', U'\u337C', // ㈹ ㍾ ㍽ ㍼
        U'\u2252', U'\u2261', U'\u222B', U'\u222E', // ≒ ≡ ∫ ∮
        U'\u2211', U'\u221A', U'\u22A5', U'\u2220', // ∑ √ ⊥ ∠
        U'\u221F', U'\u22BF', U'\u2235', U'\u2229', // ∟ ⊿ ∵ ∩
        U'\u222A',                                    // ∪
    });

    // set special characters
    cs.set_fontwidth(2);
    cs.set_space_char(U'\u3000');
    cs.set_newline_char(U'\uFF05');  // fullwidth %
}
