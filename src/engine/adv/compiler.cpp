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

#include "compiler.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../sexp_writer.h"
#include "../../utf8.h"

#include <stdexcept>
#include <unordered_map>

namespace adv {

// ── forward declarations ─────────────────────────────────────────────

static void emit_stmt(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs);
static void emit_stmts(ByteWriter& out, const std::vector<AstNode>& nodes,
                        size_t start, const Config& cfg, Charset& cs);

// ── command unresolve map ────────────────────────────────────────────

static const std::unordered_map<std::string, uint8_t>& cmd_map() {
    static const std::unordered_map<std::string, uint8_t> map = {
        {"text-break",    0xA5},
        {"text-frame",    0xA6},
        {"text-pos",      0xA7},
        {"text-color",    0xA8},
        {"text-delay",    0xA9},
        {"text-reset",    0xAA},
        {"wait",          0xAB},
        {"delay",         0xAC},
        {"menu1",         0xAD},
        {"menu2",         0xAE},
        {"seg-call",      0xAF},
        {"exec-file",     0xB0},
        {"mes-jump",      0xB1},
        {"branch-random", 0xB2},
        {"branch-index",  0xB3},
        {"branch-var",    0xB4},
        {"branch-reg",    0xB5},
        {"execute-var",   0xB6},
        {"mouse",         0xB7},
        {"define-proc",   0xB9},
        {"proc",          0xBA},
        {"repeat",        0xBB},
        {"if",            0xBC},
        {"when",          0xBD},
        {"flag-save",     0xBE},
        {"flag-load",     0xBF},
        {"mes-load?",     0xC0},
        {"load-mem",      0xC8},
        {"image-file",    0xC9},
        {"print-var",     0xCA},
        {"exec-mem",      0xCD},
        {"image-mem",     0xCF},
        {"sound",         0xD0},
        {"decrypt",       0xD5},
    };
    return map;
}

// ── variable name → index conversion ─────────────────────────────────

static int var_to_index(const AstNode& node) {
    // single uppercase letter: Variable node or single-char Symbol
    if (node.is_variable()) {
        return node.var_val - 'A';
    }

    if (node.is_symbol()) {
        const std::string& name = node.str_val;

        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
            return name[0] - 'A';
        }

        // multi-char: AA=26, AB=27, ...
        if (name.size() == 2) {
            int hi = name[0] - 'A';
            int lo = name[1] - 'A';
            return 26 + hi * 26 + lo;
        }
    }

    throw std::runtime_error("invalid variable node");
}

static bool is_var_node(const AstNode& node) {
    if (node.is_variable()) {
        return true;
    }

    if (node.is_symbol()) {
        const std::string& s = node.str_val;

        if (s.empty()) {
            return false;
        }

        for (char c : s) {

            if (c < 'A' || c > 'Z') {
                return false;
            }
        }

        return true;
    }

    return false;
}

// ── number encoding ──────────────────────────────────────────────────

static void emit_num(ByteWriter& out, int n) {
    // NUM0: 0-4 → single byte 0x23+n
    if (n >= 0 && n <= 4) {
        out.emit(static_cast<uint8_t>(0x23 + n));
        return;
    }

    // NUM1: 5-127 → 0x28, n
    if (n >= 0 && n <= 127) {
        out.emit(0x28);
        out.emit(static_cast<uint8_t>(n));
        return;
    }

    // NUM2: 128-65535 → encoded as 3 bytes
    if (n >= 128 && n <= 65535) {
        int n1 = n / 0x4000;
        int rem = n % 0x4000;
        int n2 = rem >> 7;
        int n3 = rem & 0x7F;
        out.emit(static_cast<uint8_t>(0x29 + n1));
        out.emit(static_cast<uint8_t>(n2));
        out.emit(static_cast<uint8_t>(n3));
        return;
    }

    throw std::runtime_error("number out of range for ADV encoding: " + std::to_string(n));
}

// ── register encoding ────────────────────────────────────────────────

static void emit_reg(ByteWriter& out, int n, bool flag) {
    // format: [0000][1][v:1][i:7][o:3]
    // the fixed 1 at bit 11 distinguishes reg from var encoding
    int n1 = n + 1;
    int i = n1 / 8;
    int o = n1 % 8;
    int v = flag ? 1 : 0;

    uint16_t val = static_cast<uint16_t>((1 << 11) | (v << 10) | (i << 3) | o);
    out.emit(static_cast<uint8_t>(val >> 8));
    out.emit(static_cast<uint8_t>(val & 0xFF));
}

// ── variable encoding ────────────────────────────────────────────────

// encode a variable operation/condition (no extraop, 2 bytes)
static void emit_var(ByteWriter& out, int var_idx, int f, int val) {

    if (var_idx < 0 || var_idx > 63) {
        throw std::runtime_error("variable index " + std::to_string(var_idx) +
            " out of range for non-extraop encoding (0-63)");
    }

    if (val < 0 || val > 15) {
        throw std::runtime_error("variable value " + std::to_string(val) +
            " out of range for non-extraop encoding (0-15)");
    }

    uint16_t v = static_cast<uint16_t>(0x1000 | (f << 10) | (var_idx << 4) | (val & 0x0F));
    out.emit(static_cast<uint8_t>(v >> 8));
    out.emit(static_cast<uint8_t>(v & 0xFF));
}

// encode a variable operation/condition (extraop, 3 bytes)
// bit layout: [0001:4][f:3][m:1] [pad:2][i:5][j1:1] [pad:1][j2:7]
static uint8_t checked_byte(int n, const std::string& context) {
    if (n < 0 || n > 255) {
        throw std::runtime_error("opcode out of byte range (" + context + "): " + std::to_string(n));
    }

    return static_cast<uint8_t>(n);
}

static void emit_var2(ByteWriter& out, int var_idx, int f, int j, bool m) {

    if (var_idx < 0 || var_idx > 31) {
        throw std::runtime_error("variable index " + std::to_string(var_idx) +
            " out of range for extraop encoding (0-31)");
    }

    if (j < 0 || j > 255) {
        throw std::runtime_error("variable value " + std::to_string(j) +
            " out of range for extraop encoding (0-255)");
    }

    uint8_t j1 = static_cast<uint8_t>((j >> 7) & 0x01);
    uint8_t j2 = static_cast<uint8_t>(j & 0x7F);
    uint8_t c1 = static_cast<uint8_t>(0x10 | (f << 1) | (m ? 1 : 0));
    uint8_t c2 = static_cast<uint8_t>(((var_idx & 0x1F) << 1) | j1);
    uint8_t c3 = static_cast<uint8_t>(j2);
    out.emit(c1);
    out.emit(c2);
    out.emit(c3);
}

// ── ARG encoding ─────────────────────────────────────────────────────

// encode a string as ARG format: 0x22 + bytes + 0x22
// reverses the ¥→\ and ‾→~ substitution from lower_chr_ascii
static void emit_arg(ByteWriter& out, const std::string& str, Charset& cs) {
    out.emit(0x22);

    // process each UTF-8 character
    size_t i = 0;

    while (i < str.size()) {
        unsigned char c = static_cast<unsigned char>(str[i]);

        // decode UTF-8 to get the codepoint
        char32_t cp;
        int len;

        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            // invalid UTF-8, emit raw byte
            out.emit(c);
            i++;
            continue;
        }

        for (int j = 1; j < len && i + j < str.size(); j++) {
            cp = (cp << 6) | (static_cast<unsigned char>(str[i + j]) & 0x3F);
        }

        i += len;

        // reverse the lower_chr_ascii substitution
        if (cp == U'\u00A5') {
            out.emit(0x5C); // ¥ → backslash
            continue;
        }

        if (cp == U'\u203E') {
            out.emit(0x7E); // ‾ → tilde
            continue;
        }

        // ASCII range
        if (cp >= 0x20 && cp <= 0x7E) {
            out.emit(static_cast<uint8_t>(cp));
            continue;
        }

        // SJIS encoding for non-ASCII
        auto sjis_opt = cs.char_to_sjis(cp);

        if (sjis_opt.has_value()) {
            const auto& sjis = *sjis_opt;
            out.emit(static_cast<uint8_t>(sjis[0]));

            // second byte of 0 means single-byte char (half-width katakana etc.)
            if (sjis.size() >= 2 && sjis[1] != 0) {
                out.emit(static_cast<uint8_t>(sjis[1]));
            }

            continue;
        }

        // fallback: emit raw byte
        out.emit(static_cast<uint8_t>(cp & 0xFF));
    }

    out.emit(0x22);
}

// ── text character encoding ──────────────────────────────────────────

// encode a single Unicode character as SJIS bytes with CHR1 optimization
static void emit_text_char(ByteWriter& out, char32_t cp, Charset& cs) {
    // reverse the lower_chr_ascii substitution
    if (cp == U'\u00A5') {
        cp = U'\\';
    }

    if (cp == U'\u203E') {
        cp = U'~';
    }

    auto sjis_opt = cs.char_to_sjis(cp);

    if (!sjis_opt.has_value()) {
        throw std::runtime_error("cannot encode character '" + char32_to_utf8(cp) + "' to SJIS");
    }

    const auto& sjis = *sjis_opt;

    if (sjis.size() >= 2 && sjis[1] != 0) {
        int j1 = sjis[0];
        int j2 = sjis[1];

        // CHR1 optimization: if (0x82, X) where 0x9F <= X <= 0xF1,
        // emit single byte X - 0x72 instead of 2 bytes
        if (j1 == 0x82 && j2 >= 0x9F && j2 <= 0xF1) {
            out.emit(static_cast<uint8_t>(j2 - 0x72));
            return;
        }

        // standard CHR2: emit both bytes
        out.emit(static_cast<uint8_t>(j1));
        out.emit(static_cast<uint8_t>(j2));
        return;
    }

    // single-byte char (half-width katakana, etc., or sjis[1]==0)
    if (!sjis.empty()) {
        out.emit(static_cast<uint8_t>(sjis[0]));
        return;
    }

    throw std::runtime_error("empty SJIS encoding for character");
}

// encode a UTF-8 string as SJIS text bytes
static void emit_text_string(ByteWriter& out, const std::string& str, Charset& cs) {

    for (char32_t cp : utf8_to_codepoints(str)) {
        emit_text_char(out, cp, cs);
    }
}

// ── param encoding ───────────────────────────────────────────────────

// encode a child node as a param (number, string/ARG, or block)
static void emit_param(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {

    if (node.is_integer()) {
        emit_num(out, node.int_val);
        return;
    }

    if (node.is_string()) {
        emit_arg(out, node.str_val, cs);
        return;
    }

    if (node.is_boolean()) {
        emit_num(out, node.bool_val ? 1 : 0);
        return;
    }

    if (node.is_variable()) {
        // variables as params are raw ASCII bytes (used by special commands
        // like 0xB4 branch-var, 0xB6 execute-var, 0xCA print-var)
        int idx = var_to_index(node);
        out.emit(static_cast<uint8_t>('A' + idx));
        return;
    }

    // list children get compiled as statements
    if (node.is_list()) {
        emit_stmt(out, node, cfg, cs);
        return;
    }

    // quote with children (raw SJIS integer in str context)
    if (node.is_quote() && !node.children.empty()) {
        const auto& inner = node.children[0];

        if (inner.is_symbol()) {
            const std::string& sym = inner.str_val;

            // a stray 'br/'pg as a command arg means "end this command, then
            // break/frame", same as a following (text-break)/(text-frame).
            // emitting the cmd byte here lands it right after the other params.
            if (sym == "br") {
                out.emit(0xA5);
                return;
            }

            if (sym == "pg") {
                out.emit(0xA6);
                return;
            }

            // otherwise it must be a quoted SJIS integer like '33450
            if (sym.empty() || !(std::isdigit(static_cast<unsigned char>(sym[0])) || sym[0] == '-')) {
                throw std::runtime_error("cannot encode quoted symbol '" + sym +
                    " as a parameter (expected a number, 'br, or 'pg)");
            }

            int sjis_int = std::stoi(sym);
            auto pair = Charset::integer_to_sjis(sjis_int);

            if (pair.size() >= 2) {
                out.emit(static_cast<uint8_t>(pair[0]));
                out.emit(static_cast<uint8_t>(pair[1]));
            } else if (pair.size() == 1) {
                out.emit(static_cast<uint8_t>(pair[0]));
            }

            return;
        }

        if (inner.is_integer()) {
            emit_num(out, inner.int_val);
            return;
        }
    }

    // bare quote (no children, as in sound commands)
    if (node.is_quote() && node.children.empty()) {
        return;
    }

    throw std::runtime_error("cannot encode param: unknown node kind");
}

// ── condition encoding ───────────────────────────────────────────────

// get the var? f-value from the condition tag
static int var_cond_f(const std::string& tag, bool extraop) {

    if (extraop) {
        if (tag == "=")  { return 0; }
        if (tag == ">=") { return 1; }
        if (tag == "<=") { return 2; }
        if (tag == "!=") { return 3; }
    } else {
        if (tag == "=")  { return 2; }
        if (tag == ">=") { return 3; }
        if (tag == "<=") { return 1; }
    }

    return 0;
}

static void emit_cond(ByteWriter& out, const AstNode& node, const Config& cfg) {
    // (= N #t/#f) where N is integer → reg condition
    // (= var val) / (>= var val) / (<= var val) / (!= var val) → var condition

    if (!node.is_list() || node.children.size() < 2) {
        throw std::runtime_error("invalid condition node");
    }

    const AstNode& first = node.children[0];
    const AstNode& second = node.children[1];

    // reg condition: first child is an integer
    if (first.is_integer()) {
        int n = first.int_val;
        bool flag = second.is_boolean() ? second.bool_val : (second.int_val != 0);
        emit_reg(out, n, flag);
        return;
    }

    // var condition: first child is a variable
    if (is_var_node(first)) {
        int var_idx = var_to_index(first);
        int f = var_cond_f(node.tag, cfg.extra_op);

        if (cfg.extra_op) {
            bool m = is_var_node(second);
            int j = m ? var_to_index(second) : second.int_val;
            emit_var2(out, var_idx, f, j, m);
        } else {
            int val = second.int_val;
            emit_var(out, var_idx, f, val);
        }

        return;
    }

    throw std::runtime_error("invalid condition: first child is neither integer nor variable");
}

static void emit_conds(ByteWriter& out, const AstNode& conds_node, const Config& cfg) {
    // (? cond1 cond2 ...)
    for (const auto& cond : conds_node.children) {
        emit_cond(out, cond, cfg);
    }
}

// ── text node encoding ───────────────────────────────────────────────

static void emit_text(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {
    // (text [#:col N] [#:pos '(params)] string... 'br ...)
    size_t i = 0;

    // handle #:col prefix
    if (i < node.children.size() && node.children[i].is_keyword() &&
        node.children[i].str_val == "col" || node.children[i].str_val == "color") {
        i++; // skip #:col keyword

        if (i < node.children.size()) {
            out.emit(0xA8); // text-color CMD
            emit_num(out, node.children[i].int_val);
            i++;
        }
    }

    // handle #:pos prefix
    if (i < node.children.size() && node.children[i].is_keyword() &&
        node.children[i].str_val == "pos") {
        i++; // skip #:pos keyword

        if (i < node.children.size()) {
            out.emit(0xA7); // text-pos CMD

            // the position params are quoted as a list: '(x y ...)
            const AstNode& pos_node = node.children[i];

            if (pos_node.is_quote() && !pos_node.children.empty() &&
                pos_node.children[0].is_list()) {
                const auto& params = pos_node.children[0].children;

                for (const auto& p : params) {
                    emit_num(out, p.int_val);
                }
            }

            i++;
        }
    }

    // handle #:wrap prefix (if present, just skip it, no byte encoding)
    if (i < node.children.size() && node.children[i].is_keyword() &&
        node.children[i].str_val == "wrap") {
        i++;

        if (i < node.children.size()) {
            i++; // skip the wrap value
        }
    }

    // handle #:font prefix (if present, just skip it)
    if (i < node.children.size() && node.children[i].is_keyword() &&
        node.children[i].str_val == "font") {
        i++;

        if (i < node.children.size()) {
            i++; // skip the font value
        }
    }

    // emit remaining children
    for (; i < node.children.size(); i++) {
        const auto& child = node.children[i];

        // string content → SJIS text bytes
        if (child.is_string()) {
            emit_text_string(out, child.str_val, cs);
            continue;
        }

        // 'br → text-break (0xA5)
        if (child.is_quote() && !child.children.empty() &&
            child.children[0].is_symbol() && child.children[0].str_val == "br") {
            out.emit(0xA5);
            continue;
        }

        // 'pg → text-frame (0xA6) - not used in ADV but handle defensively
        if (child.is_quote() && !child.children.empty() &&
            child.children[0].is_symbol() && child.children[0].str_val == "pg") {
            out.emit(0xA6);
            continue;
        }

        // quoted SJIS integer → raw chr2 bytes
        // handles both Quote(Symbol("34368")) from in-memory AST and
        // Quote(Integer(34368)) from SexpReader round-trip
        if (child.is_quote() && !child.children.empty()) {
            int sjis_int = -1;

            if (child.children[0].is_symbol()) {
                const std::string& sym = child.children[0].str_val;

                if (!sym.empty() && (std::isdigit(static_cast<unsigned char>(sym[0])) || sym[0] == '-')) {
                    sjis_int = std::stoi(sym);
                }
            } else if (child.children[0].is_integer()) {
                sjis_int = child.children[0].int_val;
            }

            if (sjis_int >= 0) {
                auto pair = Charset::integer_to_sjis(sjis_int);

                if (pair.size() >= 2) {
                    out.emit(static_cast<uint8_t>(pair[0]));
                    out.emit(static_cast<uint8_t>(pair[1]));
                } else if (pair.size() == 1) {
                    out.emit(static_cast<uint8_t>(pair[0]));
                }

                continue;
            }
        }

        // (proc N) embedded in text → proc CMD + num
        if (child.is_list("proc")) {

            if (!child.children.empty()) {
                out.emit(0xBA); // proc CMD
                emit_param(out, child.children[0], cfg, cs);
            }

            continue;
        }

        // integer protagonist reference → proc CMD + num
        if (child.is_integer()) {
            out.emit(0xBA); // proc CMD
            emit_num(out, child.int_val);
            continue;
        }

        // character node (undecoded) → encode directly
        if (child.is_character()) {
            emit_text_char(out, child.char_val, cs);
            continue;
        }

        // char-raw node → emit raw SJIS bytes
        if (child.is_char_raw()) {

            for (uint8_t b : child.raw_bytes) {
                out.emit(b);
            }

            continue;
        }
    }
}

// ── str node encoding (CHRS!) ────────────────────────────────────────

static void emit_str(ByteWriter& out, const AstNode& node, Charset& cs) {
    // (str [#:col N] "text" ['br] ...) → [0xA8 N] 0x21 + SJIS bytes + 0x00 [+ 0xA5 ...]
    // 'br/'pg from the text fuser close the str, emit the cmd, then
    // re-open the str only if more content follows.
    size_t start = 0;

    // handle #:col / #:color prefix
    if (start < node.children.size() && node.children[start].is_keyword() &&
        (node.children[start].str_val == "col" || node.children[start].str_val == "color")) {
        start++;

        if (start < node.children.size()) {
            out.emit(0xA8); // text-color CMD
            emit_num(out, node.children[start].int_val);
            start++;
        }
    }

    bool str_open = false;

    auto ensure_open = [&]() {

        if (!str_open) {
            out.emit(0x21);
            str_open = true;
        }
    };

    auto ensure_closed = [&]() {

        if (str_open) {
            out.emit(0x00);
            str_open = false;
        }
    };

    ensure_open();

    for (size_t ci = start; ci < node.children.size(); ci++) {
        const auto& child = node.children[ci];

        if (child.is_string()) {
            ensure_open();

            // encode each char as SJIS (ARG-style, not text-style: no CHR1 opt)
            size_t i = 0;

            while (i < child.str_val.size()) {
                unsigned char c = static_cast<unsigned char>(child.str_val[i]);
                char32_t cp;
                int len;

                if (c < 0x80) {
                    cp = c;
                    len = 1;
                } else if ((c & 0xE0) == 0xC0) {
                    cp = c & 0x1F;
                    len = 2;
                } else if ((c & 0xF0) == 0xE0) {
                    cp = c & 0x0F;
                    len = 3;
                } else if ((c & 0xF8) == 0xF0) {
                    cp = c & 0x07;
                    len = 4;
                } else {
                    out.emit(c);
                    i++;
                    continue;
                }

                for (int j = 1; j < len && i + j < child.str_val.size(); j++) {
                    cp = (cp << 6) | (static_cast<unsigned char>(child.str_val[i + j]) & 0x3F);
                }

                i += len;

                // reverse lower_chr_ascii
                if (cp == U'\u00A5') { cp = U'\\'; }
                if (cp == U'\u203E') { cp = U'~'; }

                // ASCII range
                if (cp >= 0x20 && cp <= 0x7E) {
                    out.emit(static_cast<uint8_t>(cp));
                    continue;
                }

                // SJIS encoding
                auto sjis_opt = cs.char_to_sjis(cp);

                if (sjis_opt.has_value()) {
                    const auto& sjis = *sjis_opt;
                    out.emit(static_cast<uint8_t>(sjis[0]));

                    if (sjis.size() >= 2 && sjis[1] != 0) {
                        out.emit(static_cast<uint8_t>(sjis[1]));
                    }

                    continue;
                }

                out.emit(static_cast<uint8_t>(cp & 0xFF));
            }

            continue;
        }

        // 'br → close str, emit text-break
        if (child.is_quote() && !child.children.empty() &&
            child.children[0].is_symbol() && child.children[0].str_val == "br") {
            ensure_closed();
            out.emit(0xA5);
            continue;
        }

        // 'pg → close str, emit text-frame
        if (child.is_quote() && !child.children.empty() &&
            child.children[0].is_symbol() && child.children[0].str_val == "pg") {
            ensure_closed();
            out.emit(0xA6);
            continue;
        }

        // quoted SJIS integer (symbol or integer form from SexpReader)
        if (child.is_quote() && !child.children.empty()) {
            ensure_open();
            int sjis_int = -1;

            if (child.children[0].is_symbol()) {
                const std::string& sym = child.children[0].str_val;

                if (!sym.empty() && (std::isdigit(static_cast<unsigned char>(sym[0])) || sym[0] == '-')) {
                    sjis_int = std::stoi(sym);
                }
            } else if (child.children[0].is_integer()) {
                sjis_int = child.children[0].int_val;
            }

            if (sjis_int >= 0) {
                auto pair = Charset::integer_to_sjis(sjis_int);

                for (int b : pair) {
                    out.emit(static_cast<uint8_t>(b));
                }

                continue;
            }
        }
    }

    ensure_closed();
}

// ── block encoding ───────────────────────────────────────────────────

// emit children of a (/ ...) or (// ...) branch
static void emit_branch_children(ByteWriter& out, const AstNode& node,
                                  const Config& cfg, Charset& cs) {

    if (node.tag == "//") {
        // (// (? conds...) stmts...) → conds + stmts
        if (!node.children.empty() && node.children[0].is_list("?")) {
            emit_conds(out, node.children[0], cfg);
            emit_stmts(out, node.children, 1, cfg, cs);
        } else {
            emit_stmts(out, node.children, 0, cfg, cs);
        }
    } else if (node.tag == "/") {
        // (/ stmts...) → just stmts
        emit_stmts(out, node.children, 0, cfg, cs);
    } else {
        // any other tag, emit as a stmt
        emit_stmt(out, node, cfg, cs);
    }
}

static void emit_block(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {
    const std::string& tag = node.tag;

    // (<> stmts...) → BEG + stmts + END
    if (tag == "<>") {
        out.emit(0xA2);
        emit_stmts(out, node.children, 0, cfg, cs);
        out.emit(0xA3);
        return;
    }

    // (</> branch1 branch2 ...) → BEG + branch1 + CNT + branch2 + ... + END
    if (tag == "</>") {

        out.emit(0xA2);

        for (size_t i = 0; i < node.children.size(); i++) {

            if (i > 0) {
                out.emit(0xA4); // CNT
            }

            emit_branch_children(out, node.children[i], cfg, cs);
        }

        out.emit(0xA3);
        return;
    }

    // (<+> item1 item2 ...) → BEG+ + item1 + CNT + item2 + ... + END*
    if (tag == "<+>") {
        out.emit(0xA0);

        for (size_t i = 0; i < node.children.size(); i++) {

            if (i > 0) {
                out.emit(0xA4); // CNT
            }

            // each item is (+ (?) stmts...) → conds + stmts
            const auto& item = node.children[i];

            if (item.is_list("+")) {

                if (!item.children.empty() && item.children[0].is_list("?")) {
                    emit_conds(out, item.children[0], cfg);
                    emit_stmts(out, item.children, 1, cfg, cs);
                } else {
                    emit_stmts(out, item.children, 0, cfg, cs);
                }
            } else {
                emit_stmt(out, item, cfg, cs);
            }
        }

        out.emit(0xA1);
        return;
    }

    // (<*> items...) → BEG + items + END
    if (tag == "<*>") {
        out.emit(0xA2);

        for (const auto& item : node.children) {
            // each item is (* (?) stmts... (<+> ...))
            if (item.is_list("*")) {

                if (!item.children.empty() && item.children[0].is_list("?")) {
                    emit_conds(out, item.children[0], cfg);
                }

                // emit stmts (non-? and non-<+> children)
                for (size_t i = 0; i < item.children.size(); i++) {
                    const auto& c = item.children[i];

                    if (c.is_list("?")) { continue; }
                    if (c.is_list("<+>")) { continue; }

                    emit_stmt(out, c, cfg, cs);
                }

                // emit the <+> block
                for (const auto& c : item.children) {

                    if (c.is_list("<+>")) {
                        emit_block(out, c, cfg, cs);
                        break;
                    }
                }
            } else {
                emit_stmt(out, item, cfg, cs);
            }
        }

        out.emit(0xA3);
        return;
    }
}

// ── statement encoding ───────────────────────────────────────────────

// get the var! f-value from the statement tag
static int var_stmt_f(const std::string& tag, bool extraop) {

    if (extraop) {
        if (tag == "set-var")  { return 0; }
        if (tag == "inc-var")  { return 1; }
        if (tag == "dec-var")  { return 2; }
        if (tag == "set-var2") { return 3; }
    } else {
        if (tag == "set-var")  { return 2; }
        if (tag == "inc-var")  { return 3; }
        if (tag == "dec-var")  { return 1; }
    }

    return 0;
}

static void emit_stmt(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {

    if (!node.is_list()) {
        // bare symbol that's a stmt (e.g., wait$, nop@ as symbols rather than lists)
        if (node.is_symbol()) {

            if (node.str_val == "wait$") {
                out.emit(0x81);
                out.emit(0x90);
                return;
            }

            if (node.str_val == "nop@") {
                out.emit(0x81);
                out.emit(0x97);
                return;
            }
        }

        return;
    }

    const std::string& tag = node.tag;

    // ── special forms ────────────────────────────────────────────

    // (wait$) → CHR-WAIT
    if (tag == "wait$") {
        out.emit(0x81);
        out.emit(0x90);
        return;
    }

    // (nop@) → CHR-NOP
    if (tag == "nop@") {
        out.emit(0x81);
        out.emit(0x97);
        return;
    }

    // (loop stmts...) → LBEG + stmts + LEND
    if (tag == "loop") {
        out.emit(0x81);
        out.emit(0x6F);
        emit_stmts(out, node.children, 0, cfg, cs);
        out.emit(0x81);
        out.emit(0x70);
        return;
    }

    // (while cond body) → LBEG + when + cond + body + LEND
    if (tag == "while") {
        out.emit(0x81);
        out.emit(0x6F);
        out.emit(0xBD); // when CMD

        // condition is in a </> block
        if (node.children.size() >= 2) {
            emit_stmt(out, node.children[0], cfg, cs); // condition (block)

            for (size_t i = 1; i < node.children.size(); i++) {
                emit_stmt(out, node.children[i], cfg, cs);
            }
        }

        out.emit(0x81);
        out.emit(0x70);
        return;
    }

    // ── text / str ───────────────────────────────────────────────

    if (tag == "text") {
        emit_text(out, node, cfg, cs);
        return;
    }

    if (tag == "text-raw") {
        // (text-raw sjis_int...) → emit each as chr2 bytes
        for (const auto& child : node.children) {

            if (child.is_integer()) {
                auto pair = Charset::integer_to_sjis(child.int_val);

                for (int b : pair) {
                    out.emit(static_cast<uint8_t>(b));
                }
            }
        }

        return;
    }

    if (tag == "str") {
        emit_str(out, node, cs);
        return;
    }

    // ── register / variable statements ───────────────────────────

    if (tag == "set-reg") {
        // (set-reg N #t/#f) → reg! encoding
        if (node.children.size() >= 2) {
            int n = node.children[0].int_val;
            bool flag = node.children[1].is_boolean() ? node.children[1].bool_val :
                        (node.children[1].int_val != 0);
            emit_reg(out, n, flag);
        }

        return;
    }

    if (tag == "set-var" || tag == "inc-var" || tag == "dec-var" || tag == "set-var2") {
        // (set-var var val) → var! encoding
        if (node.children.size() >= 2) {
            int var_idx = var_to_index(node.children[0]);
            int f = var_stmt_f(tag, cfg.extra_op);
            const AstNode& val_node = node.children[1];

            if (cfg.extra_op) {
                bool m = is_var_node(val_node);
                int j = m ? var_to_index(val_node) : val_node.int_val;
                emit_var2(out, var_idx, f, j, m);
            } else {
                emit_var(out, var_idx, f, val_node.int_val);
            }
        }

        return;
    }

    // ── blocks ───────────────────────────────────────────────────

    if (tag == "<>" || tag == "</>" || tag == "<+>" || tag == "<*>") {
        emit_block(out, node, cfg, cs);
        return;
    }

    // transparent wrappers (from parser's branch structures)
    if (tag == "/" || tag == "//") {
        emit_branch_children(out, node, cfg, cs);
        return;
    }

    // ── sound ────────────────────────────────────────────────────

    if (tag == "sound") {
        out.emit(0xD0);

        // first child is the quoted mode symbol (e.g., 'se  , 'fm, or bare ')
        size_t i = 0;

        if (i < node.children.size()) {
            const auto& mode = node.children[i];

            if (mode.is_quote()) {

                if (!mode.children.empty() && mode.children[0].is_symbol()) {
                    // emit ASCII bytes of the mode string
                    const std::string& mode_str = mode.children[0].str_val;

                    for (char c : mode_str) {
                        out.emit(static_cast<uint8_t>(c));
                    }
                }
                // bare quote (empty symbol) → emit nothing for mode
            }

            i++;
        }

        // emit remaining children as params
        for (; i < node.children.size(); i++) {
            emit_param(out, node.children[i], cfg, cs);
        }

        return;
    }

    // ── if / when ────────────────────────────────────────────────

    if (tag == "if" || tag == "when") {
        auto it = cmd_map().find(tag);
        out.emit(it->second);

        // the child should be a </> block
        if (!node.children.empty()) {
            emit_stmt(out, node.children[0], cfg, cs);
        }

        return;
    }

    // ── menu1 / menu2 ────────────────────────────────────────────

    if (tag == "menu1" || tag == "menu2") {
        auto it = cmd_map().find(tag);
        out.emit(it->second);

        // emit children: nums, texts, then block
        for (const auto& child : node.children) {
            emit_param(out, child, cfg, cs);
        }

        return;
    }

    // ── branch-var ───────────────────────────────────────────────

    if (tag == "branch-var") {
        if (node.children.empty() || !is_var_node(node.children[0])) {
            throw std::runtime_error("'branch-var' requires a variable as first argument");
        }

        out.emit(0xB4);
        int idx = var_to_index(node.children[0]);
        out.emit(static_cast<uint8_t>('A' + idx));

        // remaining children (block)
        for (size_t i = 1; i < node.children.size(); i++) {
            emit_stmt(out, node.children[i], cfg, cs);
        }

        return;
    }

    // ── execute-var ──────────────────────────────────────────────

    if (tag == "execute-var") {
        out.emit(0xB6);

        for (const auto& child : node.children) {
            emit_param(out, child, cfg, cs);
        }

        return;
    }

    // ── decrypt ──────────────────────────────────────────────────

    if (tag == "decrypt") {
        out.emit(0xD5);

        if (!node.children.empty()) {
            emit_num(out, node.children[0].int_val);

            int n = node.children[0].int_val;

            if (n != 0 && node.children.size() > 1) {
                // the second child is a (mes ...) sub-tree
                // compile it, then encrypt
                auto sub_bytes = compile_mes(node.children[1], cfg);

                // remove the EOM (FF FE) from the sub-output since it's
                // implicit from the outer segment's EOS
                if (sub_bytes.size() >= 2 &&
                    sub_bytes[sub_bytes.size() - 2] == 0xFF &&
                    sub_bytes[sub_bytes.size() - 1] == 0xFE) {
                    sub_bytes.resize(sub_bytes.size() - 2);
                }

                // encrypt
                uint8_t key = static_cast<uint8_t>(n);

                for (uint8_t b : sub_bytes) {
                    uint8_t encrypted = b ^ key;
                    encrypted = ((encrypted & 0xF0) >> 4) | ((encrypted & 0x0F) << 4);
                    out.emit(encrypted);
                }
            }
        }

        return;
    }

    // ── nop:N forms ──────────────────────────────────────────────

    if (tag.size() > 4 && tag.substr(0, 4) == "nop:") {
        out.emit(checked_byte(std::stoi(tag.substr(4)), "nop"));

        for (const auto& child : node.children) {
            emit_param(out, child, cfg, cs);
        }

        return;
    }

    // ── generic resolved commands ────────────────────────────────

    auto it = cmd_map().find(tag);

    if (it != cmd_map().end()) {
        out.emit(it->second);

        for (const auto& child : node.children) {
            emit_param(out, child, cfg, cs);
        }

        return;
    }

    // ── cmd:N forms (unresolved commands) ──────────────────────

    if (tag.size() > 4 && tag.substr(0, 4) == "cmd:") {
        out.emit(checked_byte(std::stoi(tag.substr(4)), "cmd"));

        for (const auto& child : node.children) {
            emit_param(out, child, cfg, cs);
        }

        return;
    }

    // ── tagless lists: emit each child ──────────────────────────

    if (tag.empty()) {
        // if the node has no children () or doesn't start with a list (e.g. unnamed commands like (cmd:209)),
        // then it's probably a raw char/str/int value that leaked into the tag. reject it as invalid because
        // it's syntactically meaningless.
        if (node.children.empty() || !node.children[0].is_list()) {
            throw std::runtime_error("node '" + SexpWriter().format(node) + "' does not start with a command or operator");
        }

        for (const auto& child : node.children) {
            emit_param(out, child, cfg, cs);
        }

        return;
    }

    // ── bytes (raw fallback) ────────────────────────────────────

    if (tag == "bytes") {
        // raw byte fallback from fuse_arg when an ARG string contained
        // non-printable bytes; wrap in ARG delimiters (0x22)
        out.emit(0x22);

        for (const auto& child : node.children) {

            if (child.is_integer()) {
                out.emit(static_cast<uint8_t>(child.int_val & 0xFF));
            }
        }

        out.emit(0x22);
        return;
    }

    // ── unknown tag ──────────────────────────────────────────────

    throw std::runtime_error("unknown statement tag: " + tag);
}

static void emit_stmts(ByteWriter& out, const std::vector<AstNode>& nodes,
                        size_t start, const Config& cfg, Charset& cs) {

    for (size_t i = start; i < nodes.size(); i++) {
        emit_stmt(out, nodes[i], cfg, cs);
    }
}

// ── segment encoding ─────────────────────────────────────────────────

static void emit_seg(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {

    if (node.tag == "seg*") {
        // (seg* stmts...) → stmts + EOS
        emit_stmts(out, node.children, 0, cfg, cs);
    } else if (node.tag == "seg") {
        // (seg (? conds...) stmts...) → conds + stmts + EOS
        if (!node.children.empty() && node.children[0].is_list("?")) {
            emit_conds(out, node.children[0], cfg);

            // if the first statement is a reg/var operation, its leading byte
            // falls in 0x00-0x1F which the parser would greedily absorb as a
            // condition. emit a CNT separator to prevent this.
            if (node.children.size() > 1) {
                const auto& first_stmt = node.children[1];

                if (first_stmt.is_list()) {
                    const std::string& t = first_stmt.tag;

                    if (t == "set-reg" ||
                        t == "set-var" || t == "inc-var" ||
                        t == "dec-var" || t == "set-var2") {
                        out.emit(0xA4);
                    }
                }
            }

            emit_stmts(out, node.children, 1, cfg, cs);
        } else {
            emit_stmts(out, node.children, 0, cfg, cs);
        }
    }

    // EOS
    out.emit(0xFF);
    out.emit(0xFF);
}

// ── top-level compile ────────────────────────────────────────────────

std::vector<uint8_t> compile_mes(const AstNode& ast, const Config& cfg) {
    // make a local copy so meta extraction doesn't mutate the caller's config
    Config local_cfg = cfg;

    // extract configuration from meta node if present
    for (const auto& child : ast.children) {

        if (child.is_list("meta")) {

            for (const auto& entry : child.children) {

                if (entry.is_list("charset") && !entry.children.empty() &&
                    entry.children[0].is_string()) {
                    local_cfg.charset_name = entry.children[0].str_val;
                }

                if (entry.is_list("extraop") && !entry.children.empty() &&
                    entry.children[0].is_boolean()) {
                    local_cfg.extra_op = entry.children[0].bool_val;
                }
            }
        }
    }

    Charset cs;
    cs.load(local_cfg.charset_name);

    ByteWriter out;

    // emit segments (skip meta)
    for (const auto& child : ast.children) {

        if (child.is_list("meta")) {
            continue;
        }

        emit_seg(out, child, local_cfg, cs);
    }

    // EOM
    out.emit(0xFF);
    out.emit(0xFE);

    return out.take_data();
}

} // namespace adv
