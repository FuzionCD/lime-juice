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
#include "tokens.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace ai1 {

// ── reverse opcode maps ──────────────────────────────────────────────

static const std::unordered_map<std::string, uint8_t>& cmd_map() {
    static const std::unordered_map<std::string, uint8_t> m = {
        {"set-reg:",     0x99},
        {"set-var",      0x9A},
        {"set-arr~",     0x9B},
        {"set-arr~b",    0x9C},
        {"while",        0x9E},
        {"continue",     0x9F},
        {"break",        0xA0},
        {"menu",         0xA1},
        {"mes-jump",     0xA2},
        {"mes-call",     0xA3},
        {"define-proc",  0xA4},
        {"com",          0xA5},
        {"wait",         0xA6},
        {"window",       0xA7},
        {"text-position",0xA8},
        {"text-color",   0xA9},
        {"number",       0xAB},
        {"call",         0xAC},
        {"image",        0xAD},
        {"load",         0xAE},
        {"execute",      0xAF},
        {"recover",      0xB0},
        {"set-mem",      0xB1},
        {"screen",       0xB2},
        {"mes-skip",     0xB3},
        {"flag",         0xB4},
        {"sound",        0xB6},
        {"animate",      0xB7},
        {"slot",         0xB8},
        {"set-bg",       0xB9},
        {"clear",        0xAA},
    };
    return m;
}

static const std::unordered_map<std::string, uint8_t>& exp_map() {
    static const std::unordered_map<std::string, uint8_t> m = {
        {"!=", 0x21},
        {"~b", 0x23},
        {"%",  0x25},
        {"&&", 0x26},
        {"*",  0x2A},
        {"+",  0x2B},
        {"-",  0x2D},
        {"/",  0x2F},
        {"<",  0x3C},
        {"==", 0x3D},
        {">",  0x3E},
        {"?",  0x3F},
        {"~",  0x5C},
        {"^",  0x5E},
        {"//", 0x7C},
    };
    return m;
}

// ── encoding helpers ─────────────────────────────────────────────────

static void emit_reg(ByteWriter& out, int n) {
    if (n < 0) {
        throw std::runtime_error("negative register index: " + std::to_string(n));
    }

    if (n <= 6) {
        out.emit(static_cast<uint8_t>(TOK_REG0 + n));
    } else if (n <= 0xFF) {
        out.emit(TOK_REG1);
        out.emit(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.emit(TOK_REG2);
        out.emit(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.emit(static_cast<uint8_t>(n & 0xFF));
    } else {
        throw std::runtime_error("register value too large: " + std::to_string(n));
    }
}

static void emit_num(ByteWriter& out, int n) {
    if (n < 0) {
        throw std::runtime_error("negative number: " + std::to_string(n));
    }

    if (n <= 6) {
        out.emit(static_cast<uint8_t>(TOK_NUM0 + n));
    } else if (n <= 0xFF) {
        out.emit(TOK_NUM1);
        out.emit(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.emit(TOK_NUM2);
        out.emit(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.emit(static_cast<uint8_t>(n & 0xFF));
    } else {
        throw std::runtime_error("number too large: " + std::to_string(n));
    }
}

static int var_char_to_byte(const AstNode& node) {
    // variable nodes: (var #\A) → 0x41, (var #\@) → 0x40
    if (node.is_character()) {
        return 0x40 + (static_cast<int>(node.char_val) - '@');
    }

    if (node.is_symbol()) {
        if (node.str_val.size() == 1) {
            return static_cast<int>(node.str_val[0]);
        }
    }

    throw std::runtime_error("cannot convert variable node to byte");
}

static bool is_var_char(const AstNode& node) {
    if (node.is_character()) {
        char32_t c = node.char_val;
        return (c >= '@' && c <= 'Z');
    }

    if (node.is_symbol()) {
        if (node.str_val.size() == 1) {
            char c = node.str_val[0];
            return (c >= '@' && c <= 'Z');
        }
    }

    return false;
}

static void emit_var(ByteWriter& out, const AstNode& node) {

    if (node.is_variable()) {
        int byte = 0x40 + (static_cast<int>(node.var_val) - '@');
        out.emit(static_cast<uint8_t>(byte));
        return;
    }

    out.emit(static_cast<uint8_t>(var_char_to_byte(node)));
}

static uint8_t checked_byte(int n, const std::string& context) {
    if (n < 0 || n > 255) {
        throw std::runtime_error("opcode out of byte range (" + context + "): " + std::to_string(n));
    }

    return static_cast<uint8_t>(n);
}

// ── string byte encoding ────────────────────────────────────────────

void emit_str_bytes(ByteWriter& out, const std::string& s) {
    size_t i = 0;

    while (i < s.size()) {
        uint8_t b = static_cast<uint8_t>(s[i]);

        if (b < 0x80) {
            out.emit(b);
            i++;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            // 3-byte UTF-8: check for half-width katakana (U+FF61-U+FF9F)
            char32_t cp = (static_cast<char32_t>(b & 0x0F) << 12) |
                          (static_cast<char32_t>(static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                          static_cast<char32_t>(static_cast<uint8_t>(s[i + 2]) & 0x3F);

            if (cp >= 0xFF61 && cp <= 0xFF9F) {
                out.emit(static_cast<uint8_t>(cp - 0xFEC0));
            } else {
                throw std::runtime_error("character '" + char32_to_utf8(cp) + "' cannot be encoded in a str node");
            }

            i += 3;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            // 2-byte UTF-8
            char32_t cp = (static_cast<char32_t>(b & 0x1F) << 6) |
                          static_cast<char32_t>(static_cast<uint8_t>(s[i + 1]) & 0x3F);

            if (cp <= 0xFF) {
                out.emit(static_cast<uint8_t>(cp));
            } else {
                throw std::runtime_error("character '" + char32_to_utf8(cp) + "' cannot be encoded in a str node");
            }

            i += 2;
        } else {
            out.emit(b);
            i++;
        }
    }
}

// ── expression encoding ──────────────────────────────────────────────

// forward declarations
static void emit_stmt(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs);
static void emit_stmts(ByteWriter& out, const std::vector<AstNode>& nodes,
                        size_t start, const Config& cfg, Charset& cs);

static void emit_expr(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {
    // integer literal
    if (node.is_integer()) {
        emit_num(out, node.int_val);
        return;
    }

    // variable node (Kind::Variable with var_val)
    if (node.is_variable()) {
        int byte = 0x40 + (static_cast<int>(node.var_val) - '@');
        out.emit(static_cast<uint8_t>(byte));
        return;
    }

    // variable character (bare A-Z or @)
    if (is_var_char(node)) {
        emit_var(out, node);
        return;
    }

    // register reference: (: n) → register encoding, NOT an operator
    if (node.is_list(":") && !node.children.empty()) {

        if (node.children[0].is_integer()) {
            emit_reg(out, node.children[0].int_val);
        } else {
            // complex expression as register index
            emit_expr(out, node.children[0], cfg, cs);
        }

        return;
    }

    // unary prefix: (? a)
    if (node.is_list("?") && !node.children.empty()) {
        out.emit(0x3F);
        emit_expr(out, node.children[0], cfg, cs);
        return;
    }

    // binary operator: (op a b [c ...])
    auto it = exp_map().find(node.tag);

    if (it != exp_map().end() && node.children.size() >= 2) {
        // left-associative: (+ a b c) → ((+ a b) c) → a b + c +
        emit_expr(out, node.children[0], cfg, cs);

        for (size_t i = 1; i < node.children.size(); i++) {
            emit_expr(out, node.children[i], cfg, cs);
            out.emit(it->second);
        }

        return;
    }

    // register access (~ var offset) - used in expressions like (~ M 0)
    if (node.is_list("~") && node.children.size() >= 2) {
        emit_expr(out, node.children[0], cfg, cs);

        for (size_t i = 1; i < node.children.size(); i++) {
            emit_expr(out, node.children[i], cfg, cs);
            out.emit(0x5C);
        }

        return;
    }

    // ~b operator
    if (node.is_list("~b") && node.children.size() >= 2) {
        emit_expr(out, node.children[0], cfg, cs);

        for (size_t i = 1; i < node.children.size(); i++) {
            emit_expr(out, node.children[i], cfg, cs);
            out.emit(0x23);
        }

        return;
    }

    // nested list that is a command (like (number expr) inside expressions)
    if (node.is_list()) {
        emit_stmt(out, node, cfg, cs);
        return;
    }

    throw std::runtime_error("ai1: unsupported expression node kind=" +
                             std::to_string(static_cast<int>(node.kind)) +
                             " tag=" + node.tag);
}

// ── parameter encoding ───────────────────────────────────────────────

static void emit_params(ByteWriter& out, const std::vector<AstNode>& params,
                         size_t start, const Config& cfg, Charset& cs) {
    // parameters are separated by CNT (0x2C) in reverse order
    // (from Racket: params builds the list reversed with CNT between)

    for (size_t i = start; i < params.size(); i++) {

        if (i > start) {
            out.emit(TOK_CNT);
        }

        const auto& p = params[i];

        // block parameter
        if (p.is_list("<>") || p.is_list("<*>") || p.is_list("<.>")) {
            emit_stmt(out, p, cfg, cs);
        }
        // string parameter
        else if (p.is_list("str") || p.is_string()) {
            emit_stmt(out, p, cfg, cs);
        }
        // expression parameter
        else {
            emit_expr(out, p, cfg, cs);
        }
    }
}

// ── text encoding ────────────────────────────────────────────────────

static void emit_text_chars(ByteWriter& out, const std::string& text, Charset& cs) {

    for (char32_t cp : utf8_to_codepoints(text)) {
        auto sjis_opt = cs.char_to_sjis(cp);

        if (!sjis_opt.has_value()) {
            throw std::runtime_error("cannot encode character '" + char32_to_utf8(cp) + "' to SJIS");
        }

        for (int b : *sjis_opt) {
            out.emit(static_cast<uint8_t>(b));
        }
    }
}

static void emit_text(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {
    // (text "str") → SJIS pairs
    // (text #:color N "str") → text-color N, SJIS pairs
    // (text "str" (number expr) "str") → SJIS, number cmd, SJIS

    size_t start = 0;

    // check for #:color keyword
    // (text #:color N "str" ...) → emit text-color cmd, color value, then text content
    if (node.children.size() >= 2 &&
        node.children[0].is_keyword() && (node.children[0].str_val == "color" || node.children[0].str_val == "col")) {
        out.emit(0xA9);
        emit_expr(out, node.children[1], cfg, cs);
        start = 2;
    }

    for (size_t i = start; i < node.children.size(); i++) {
        const auto& child = node.children[i];

        if (child.is_string()) {
            emit_text_chars(out, child.str_val, cs);
        } else if (child.is_list("number")) {
            // (number expr) embedded in text
            emit_stmt(out, child, cfg, cs);
        } else if (child.is_integer()) {
            // a bare number is a fused protagonist name: (proc n)
            emit_stmt(out, AstNode::make_list("proc", {child}), cfg, cs);
        } else if (child.is_variable()) {
            // a bare variable is a fused protagonist name: (call V)
            emit_stmt(out, AstNode::make_list("call", {child}), cfg, cs);
        } else if (child.is_list("proc") || child.is_list("call")) {
            emit_stmt(out, child, cfg, cs);
        } else if (child.is_character()) {
            // single character
            auto sjis_opt = cs.char_to_sjis(child.char_val);

            if (sjis_opt.has_value()) {
                for (int b : *sjis_opt) {
                    out.emit(static_cast<uint8_t>(b));
                }
            }
        }
    }
}

// ── statement encoding ───────────────────────────────────────────────

static void emit_stmt(ByteWriter& out, const AstNode& node, const Config& cfg, Charset& cs) {
    if (!node.is_list()) {

        // cut node → CNT token
        if (node.is_cut()) {
            out.emit(TOK_CNT);
            return;
        }

        // bare string → STR encoding (0x22 bytes 0x22)
        if (node.is_string()) {
            out.emit(TOK_STR);
            emit_str_bytes(out, node.str_val);
            out.emit(TOK_STR);
            return;
        }

        // bare value in statement position (variable, number, etc)
        emit_expr(out, node, cfg, cs);
        return;
    }

    const std::string& tag = node.tag;

    // ── meta (skip) ──────────────────────────────────────────────
    if (tag == "meta") {
        return;
    }

    // ── text ─────────────────────────────────────────────────────
    if (tag == "text") {
        emit_text(out, node, cfg, cs);
        return;
    }

    // ── text-raw ─────────────────────────────────────────────────
    if (tag == "text-raw") {
        for (const auto& child : node.children) {

            if (child.is_integer()) {
                auto sjis = Charset::integer_to_sjis(child.int_val);

                for (int b : sjis) {
                    out.emit(static_cast<uint8_t>(b));
                }
            }
        }

        return;
    }

    // ── str ──────────────────────────────────────────────────────
    if (tag == "str") {
        size_t str_start = 0;

        // handle #:col / #:color prefix
        if (str_start < node.children.size() && node.children[str_start].is_keyword() &&
            (node.children[str_start].str_val == "col" || node.children[str_start].str_val == "color")) {
            str_start++;

            if (str_start < node.children.size()) {
                out.emit(0xA9); // text-color CMD
                emit_expr(out, node.children[str_start], cfg, cs);
                str_start++;
            }
        }

        out.emit(TOK_STR);

        if (str_start < node.children.size() && node.children[str_start].is_string()) {
            emit_str_bytes(out, node.children[str_start].str_val);
        }

        out.emit(TOK_STR);
        return;
    }

    // ── blocks ───────────────────────────────────────────────────
    if (tag == "<>") {
        out.emit(TOK_BEG);
        emit_stmts(out, node.children, 0, cfg, cs);
        out.emit(TOK_END);
        return;
    }

    if (tag == "<*>") {
        emit_stmts(out, node.children, 0, cfg, cs);
        return;
    }

    if (tag == "<.>") {
        // menu block: items separated by CNT inside BEG/END
        out.emit(TOK_BEG);

        for (size_t i = 0; i < node.children.size(); i++) {

            if (i > 0) {
                out.emit(TOK_CNT);
            }

            emit_stmt(out, node.children[i], cfg, cs);
        }

        out.emit(TOK_END);
        return;
    }

    // ── cut ──────────────────────────────────────────────────────
    if (tag == "cut") {
        out.emit(TOK_CNT);
        return;
    }

    // ── proc ─────────────────────────────────────────────────────
    if (tag == "proc") {

        if (!node.children.empty() && node.children[0].is_integer()) {
            out.emit(checked_byte(TOK_PROC + node.children[0].int_val, "proc"));
        }

        return;
    }

    // ── conditionals ─────────────────────────────────────────────
    if (tag == "if") {
        // (if expr block) → CND expr block
        if (node.children.size() >= 2) {
            out.emit(TOK_CND);
            emit_expr(out, node.children[0], cfg, cs);
            emit_stmt(out, node.children[1], cfg, cs);
        }

        return;
    }

    if (tag == "if-else") {
        // (if-else expr then else) → CND expr then CNT else
        if (node.children.size() >= 3) {
            out.emit(TOK_CND);
            emit_expr(out, node.children[0], cfg, cs);
            emit_stmt(out, node.children[1], cfg, cs);
            out.emit(TOK_CNT);
            emit_stmt(out, node.children[2], cfg, cs);
        }

        return;
    }

    if (tag == "cond") {
        // (cond (expr1 block1) (expr2 block2) ... (else block))
        // → CND expr1 block1 CNT CND expr2 block2 CNT ... else-block
        for (size_t i = 0; i < node.children.size(); i++) {

            if (i > 0) {
                out.emit(TOK_CNT);
            }

            const auto& branch = node.children[i];

            // else clause: tag="else", 1 child (the block)
            if (branch.is_list("else") && !branch.children.empty()) {
                emit_stmt(out, branch.children[0], cfg, cs);
                continue;
            }

            // regular clause: untagged list with 2 children (expr, block)
            if (branch.is_list() && branch.children.size() >= 2) {
                out.emit(TOK_CND);
                emit_expr(out, branch.children[0], cfg, cs);
                emit_stmt(out, branch.children[1], cfg, cs);
            }
        }

        return;
    }

    // ── while ────────────────────────────────────────────────────
    if (tag == "while") {
        // (while expr block) → CMD:0x9E CND expr block
        if (node.children.size() >= 2) {
            out.emit(0x9E);
            out.emit(TOK_CND);
            emit_expr(out, node.children[0], cfg, cs);
            emit_stmt(out, node.children[1], cfg, cs);
        }

        return;
    }

    // ── set-var ──────────────────────────────────────────────────
    if (tag == "set-var") {
        // (set-var V expr) → 0x9A V CNT expr
        if (node.children.size() >= 2) {
            out.emit(0x9A);
            emit_params(out, node.children, 0, cfg, cs);
        }

        return;
    }

    // ── set-reg: ─────────────────────────────────────────────────
    if (tag == "set-reg:") {
        // (set-reg: val expr...) → 0x99 val params
        if (node.children.size() >= 1) {
            out.emit(0x99);
            emit_params(out, node.children, 0, cfg, cs);
        }

        return;
    }

    // ── set-arr~ / set-arr~b ─────────────────────────────────────
    if (tag == "set-arr~") {
        // (set-arr~ V idx expr...) → 0x9B V CNT idx CNT expr...
        if (node.children.size() >= 2) {
            out.emit(0x9B);
            emit_params(out, node.children, 0, cfg, cs);
        }

        return;
    }

    if (tag == "set-arr~b") {
        if (node.children.size() >= 2) {
            out.emit(0x9C);
            emit_params(out, node.children, 0, cfg, cs);
        }

        return;
    }

    // ── generic commands via cmd_map ──────────────────────────────
    auto it = cmd_map().find(tag);

    if (it != cmd_map().end()) {
        out.emit(it->second);
        emit_params(out, node.children, 0, cfg, cs);
        return;
    }

    // ── cmd:N fallback (unresolved commands) ─────────────────────
    if (tag.substr(0, 4) == "cmd:") {
        out.emit(checked_byte(std::stoi(tag.substr(4)), "cmd"));
        emit_params(out, node.children, 0, cfg, cs);
        return;
    }

    // ── expression operators in statement position ───────────────
    auto eit = exp_map().find(tag);

    if (eit != exp_map().end()) {
        emit_expr(out, node, cfg, cs);
        return;
    }

    // ── register access in statement position ────────────────────
    if (tag == ":" || tag == "?") {
        emit_expr(out, node, cfg, cs);
        return;
    }

    throw std::runtime_error("ai1 compiler: unsupported node: " + tag);
}

static void emit_stmts(ByteWriter& out, const std::vector<AstNode>& nodes,
                        size_t start, const Config& cfg, Charset& cs) {

    for (size_t i = start; i < nodes.size(); i++) {
        emit_stmt(out, nodes[i], cfg, cs);
    }
}

// ── top-level compile ────────────────────────────────────────────────

std::vector<uint8_t> compile_mes(const AstNode& ast, Config& cfg) {
    // extract configuration from meta node if present
    for (const auto& child : ast.children) {

        if (child.is_list("meta")) {

            for (const auto& entry : child.children) {

                if (entry.is_list("charset") && !entry.children.empty() &&
                    entry.children[0].is_string()) {
                    cfg.charset_name = entry.children[0].str_val;
                }
            }
        }
    }

    Charset cs;
    cs.load(cfg.charset_name);

    ByteWriter out;

    // emit statements (skip meta)
    for (const auto& child : ast.children) {

        if (child.is_list("meta")) {
            continue;
        }

        emit_stmt(out, child, cfg, cs);
    }

    // end marker
    out.emit(TOK_END);

    return out.take_data();
}

} // namespace ai1
