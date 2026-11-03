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
#include "../ai1/compiler.h"
#include "../../byte_writer.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace ai5 {

// ── reverse opcode maps ──────────────────────────────────────────────

static const std::unordered_map<std::string, uint8_t>& cmd_map() {
    static const std::unordered_map<std::string, uint8_t> m = {
        {"text-color",   0x10},
        {"wait",         0x11},
        {"define-proc",  0x12},
        {"proc",         0x13},
        {"call",         0x14},
        {"number",       0x15},
        {"delay",        0x16},
        {"clear",        0x17},
        {"color",        0x18},
        {"util",         0x19},
        {"animate",      0x1A},
    };
    return m;
}

static const std::unordered_map<std::string, uint8_t>& exp_map() {
    static const std::unordered_map<std::string, uint8_t> m = {
        {"+",  0x20},
        {"-",  0x21},
        {"*",  0x22},
        {"/",  0x23},
        {"%",  0x24},
        {"//", 0x25},
        {"&&", 0x26},
        {"==", 0x27},
        {"!=", 0x28},
        {">",  0x29},
        {"<",  0x2A},
        {"~",  0x2B},
        {"~b", 0x2C},
        {":",  0x2D},
        {"::", 0x2E},
        {"?",  0x2F},
    };
    return m;
}

static const std::unordered_map<std::string, uint8_t>& sys_map() {
    static const std::unordered_map<std::string, uint8_t> m = {
        {"while",      0x10},
        {"continue",   0x11},
        {"break",      0x12},
        {"menu-show",  0x13},
        {"menu-init",  0x14},
        {"mouse",      0x15},
        {"palette",    0x16},
        {"box",        0x17},
        {"box-inv",    0x18},
        {"blit",       0x19},
        {"blit-swap",  0x1A},
        {"blit-mask",  0x1B},
        {"load",       0x1C},
        {"image",      0x1D},
        {"mes-jump",   0x1E},
        {"mes-call",   0x1F},
        {"flag",       0x21},
        {"slot",       0x22},
        {"click",      0x23},
        {"sound",      0x24},
        {"field",      0x26},
    };
    return m;
}

// ── encoding helpers ─────────────────────────────────────────────────

// encode an integer as AI5 variable-length base-64 number
static void emit_num(ByteWriter& out, int n) {
    int token;
    std::vector<int> payload;

    if (n < 0 || n >= 262144){
        // 262144+: invalid value, not supported by engine
        throw std::runtime_error("ai5: integer in script out of range (negative or greater than 262,143)");
    }

    // determine path based on num size
    if (n < 16){
        // values 0-15: single byte (0x30 + n)
        out.emit(static_cast<uint8_t>(0x30 + n));
        return;
    }else if (n < 64){
        // values 16-63: TOK_NUM1 (0x07) + 1 payload byte
        token = TOK_NUM1;
        payload = {n};
    }else if (n < 4096){
        // values 64-4095: TOK_NUM2 (0x08) + 2 payload bytes
        token = TOK_NUM2;
        payload = {(n >> 6), (n & 63)};
    }else if (n < 262144){
        // values 4096-262143: TOK_NUM3 (0x09) + 3 payload bytes
        token = TOK_NUM3;
        payload = {(n >> 12), ((n >> 6) & 63), (n & 63)};
    }

    out.emit(static_cast<uint8_t>(token));

    // each payload byte = (6_bit_chunk << 2) | 0x03
    // chunks are emitted MSB-first
    for (int chunk : payload){
        out.emit(static_cast<uint8_t>((chunk << 2) | 3));
    }
}

static void emit_var(ByteWriter& out, const AstNode& node) {

    if (node.is_variable()) {
        int byte = 0x40 + (static_cast<int>(node.var_val) - '@');
        out.emit(static_cast<uint8_t>(byte));
        return;
    }

    if (node.is_character()) {
        int byte = 0x40 + (static_cast<int>(node.char_val) - '@');
        out.emit(static_cast<uint8_t>(byte));
        return;
    }

    if (node.is_symbol() && node.str_val.size() == 1) {
        out.emit(static_cast<uint8_t>(node.str_val[0]));
        return;
    }

    throw std::runtime_error("ai5: cannot convert variable node to byte");
}

static bool is_var_char(const AstNode& node) {

    if (node.is_character()) {
        char32_t c = node.char_val;
        return (c >= '@' && c <= 'Z');
    }

    if (node.is_symbol() && node.str_val.size() == 1) {
        char c = node.str_val[0];
        return (c >= '@' && c <= 'Z');
    }

    return false;
}

// ── expression encoding ──────────────────────────────────────────────

// forward declarations
static void emit_stmt(ByteWriter& out, const AstNode& node, const Config& cfg,
                       Charset& cs, const std::unordered_map<uint32_t, int>& dict_lookup);
static void emit_stmts(ByteWriter& out, const std::vector<AstNode>& nodes,
                        size_t start, const Config& cfg, Charset& cs,
                        const std::unordered_map<uint32_t, int>& dict_lookup);

static void emit_expr(ByteWriter& out, const AstNode& node, const Config& cfg,
                       Charset& cs, const std::unordered_map<uint32_t, int>& dict_lookup) {
    // integer literal
    if (node.is_integer()) {
        emit_num(out, node.int_val);
        return;
    }

    // variable node
    if (node.is_variable()) {
        int byte = 0x40 + (static_cast<int>(node.var_val) - '@');
        out.emit(static_cast<uint8_t>(byte));
        return;
    }

    // bare variable character
    if (is_var_char(node)) {
        emit_var(out, node);
        return;
    }

    // operator: binary, unary prefix (term0), or unary postfix (term1)
    auto it = exp_map().find(node.tag);

    if (it != exp_map().end()) {

        if (node.children.size() == 1) {
            // unary: term0 or term1
            uint8_t op_byte = it->second;

            if (op_byte == 0x2D || op_byte == 0x2F) {
                // term0 (prefix): op then operand
                out.emit(op_byte);
                emit_expr(out, node.children[0], cfg, cs, dict_lookup);
            } else if (op_byte == 0x2E) {
                // term1 (postfix): operand then op
                emit_expr(out, node.children[0], cfg, cs, dict_lookup);
                out.emit(op_byte);
            } else {
                // treat as prefix
                out.emit(op_byte);
                emit_expr(out, node.children[0], cfg, cs, dict_lookup);
            }

            return;
        }

        if (node.children.size() >= 2) {
            // binary infix (left-associative): a b op c op ...
            emit_expr(out, node.children[0], cfg, cs, dict_lookup);

            for (size_t i = 1; i < node.children.size(); i++) {
                emit_expr(out, node.children[i], cfg, cs, dict_lookup);
                out.emit(it->second);
            }

            return;
        }
    }

    // nested list (command in expression position)
    if (node.is_list()) {
        emit_stmt(out, node, cfg, cs, dict_lookup);
        return;
    }

    throw std::runtime_error("ai5: unsupported expression node kind=" +
                             std::to_string(static_cast<int>(node.kind)) +
                             " tag=" + node.tag);
}

// emit a single expression with VAL terminator
static void emit_expr_val(ByteWriter& out, const AstNode& node, const Config& cfg,
                           Charset& cs, const std::unordered_map<uint32_t, int>& dict_lookup) {
    emit_expr(out, node, cfg, cs, dict_lookup);
    out.emit(TOK_VAL);
}

// emit multiple expressions separated by CNT, each with VAL terminator
static void emit_exprs(ByteWriter& out, const std::vector<AstNode>& exprs,
                        size_t start, const Config& cfg, Charset& cs,
                        const std::unordered_map<uint32_t, int>& dict_lookup) {

    for (size_t i = start; i < exprs.size(); i++) {

        if (i > start) {
            out.emit(TOK_CNT);
        }

        emit_expr_val(out, exprs[i], cfg, cs, dict_lookup);
    }
}

// ── parameter encoding ───────────────────────────────────────────────

static void emit_params(ByteWriter& out, const std::vector<AstNode>& params,
                         size_t start, const Config& cfg, Charset& cs,
                         const std::unordered_map<uint32_t, int>& dict_lookup) {

    for (size_t i = start; i < params.size(); i++) {

        if (i > start) {
            out.emit(TOK_CNT);
        }

        const auto& p = params[i];

        // block parameter
        if (p.is_list("<>") || p.is_list("<*>") || p.is_list("<.>")) {
            emit_stmt(out, p, cfg, cs, dict_lookup);
        }
        // string parameter
        else if (p.is_list("str") || p.is_string()) {
            emit_stmt(out, p, cfg, cs, dict_lookup);
        }
        // expression parameter (with VAL terminator)
        else {
            emit_expr_val(out, p, cfg, cs, dict_lookup);
        }
    }
}

// ── text/character encoding ──────────────────────────────────────────

static uint32_t sjis_pair_key(int c1, int c2) {
    return (static_cast<uint32_t>(c1) << 16) | static_cast<uint32_t>(c2);
}

// ── dict-build helpers ──────────────────────────────────────────────

// a single SJIS unit: 1 byte (ASCII) or 2 bytes (double-byte character)
using SjisUnit = std::vector<int>;
// a run of contiguous text units (breaks at non-text boundaries)
using SjisRun = std::vector<SjisUnit>;

// convert a UTF-8 string to a run of SJIS units
static SjisRun string_to_sjis_run(const std::string& text, Charset& cs) {
    SjisRun run;

    for (char32_t cp : utf8_to_codepoints(text)) {
        auto sjis_opt = cs.char_to_sjis(cp);

        if (sjis_opt.has_value()) {
            run.push_back(*sjis_opt);
        }
    }

    return run;
}

// recursively collect all text content from the AST as SJIS runs
static void collect_text_sjis(const AstNode& node, Charset& cs,
                               std::vector<SjisRun>& runs) {

    if (!node.is_list()) {
        return;
    }

    const std::string& tag = node.tag;

    // skip non-text structural nodes
    if (tag == "meta" || tag == "dict" || tag == "dict-build" || tag == "str") {
        return;
    }

    if (tag == "text") {
        SjisRun run;

        for (const auto& child : node.children) {

            if (child.is_string()) {
                auto sub = string_to_sjis_run(child.str_val, cs);
                run.insert(run.end(), sub.begin(), sub.end());
            } else if (child.is_character()) {
                auto sjis_opt = cs.char_to_sjis(child.char_val);

                if (sjis_opt.has_value()) {
                    run.push_back(*sjis_opt);
                }
            } else {
                // non-text child (number, proc, etc.) breaks the run
                if (!run.empty()) {
                    runs.push_back(std::move(run));
                    run = {};
                }
            }
        }

        if (!run.empty()) {
            runs.push_back(std::move(run));
        }

        return;
    }

    // recurse into other list nodes
    for (const auto& child : node.children) {
        collect_text_sjis(child, cs, runs);
    }
}

static void emit_chr(ByteWriter& out, int c1, int c2, const Config& cfg,
                      const std::unordered_map<uint32_t, int>& dict_lookup) {
    uint32_t key = sjis_pair_key(c1, c2);
    auto it = dict_lookup.find(key);

    if (cfg.compress && it != dict_lookup.end()) {
        // dictionary reference: single byte
        out.emit(static_cast<uint8_t>(cfg.dict_base + it->second));
    } else if (cfg.use_dict) {
        // with dict: apply -0x20 offset to c1
        out.emit(static_cast<uint8_t>(c1 - 0x20));
        out.emit(static_cast<uint8_t>(c2));
    } else {
        // no dict: raw SJIS pair
        out.emit(static_cast<uint8_t>(c1));
        out.emit(static_cast<uint8_t>(c2));
    }
}

static void emit_text_chars(ByteWriter& out, const std::string& text, Charset& cs,
                             const Config& cfg,
                             const std::unordered_map<uint32_t, int>& dict_lookup) {
    // convert UTF-8 text to SJIS units, then emit with dictionary compression
    std::vector<std::vector<int>> units;

    for (char32_t cp : utf8_to_codepoints(text)) {
        auto sjis_opt = cs.char_to_sjis(cp);

        if (!sjis_opt.has_value()) {
            throw std::runtime_error("cannot encode character '" + char32_to_utf8(cp) + "' to SJIS");
        }

        units.push_back(*sjis_opt);
    }

    // emit units with ASCII bigram lookahead for dictionary compression
    size_t i = 0;

    while (i < units.size()) {
        const auto& unit = units[i];

        if (unit.size() == 2) {
            // double-byte SJIS: use existing dictionary-aware emit
            emit_chr(out, unit[0], unit[1], cfg, dict_lookup);
            i++;
        } else {
            // single-byte: check for ASCII bigram in dictionary
            if (cfg.compress && i + 1 < units.size() && units[i + 1].size() == 1) {
                uint32_t key = sjis_pair_key(unit[0], units[i + 1][0]);
                auto it = dict_lookup.find(key);

                if (it != dict_lookup.end()) {
                    out.emit(static_cast<uint8_t>(cfg.dict_base + it->second));
                    i += 2;
                    continue;
                }
            }

            out.emit(static_cast<uint8_t>(unit[0]));
            i++;
        }
    }
}

static void emit_text(ByteWriter& out, const AstNode& node, const Config& cfg,
                       Charset& cs, const std::unordered_map<uint32_t, int>& dict_lookup) {
    size_t start = 0;

    // check for #:color keyword
    if (node.children.size() >= 2 &&
        node.children[0].is_keyword() && 
        (node.children[0].str_val == "color" || node.children[0].str_val == "col")) {
        out.emit(0x10);  // text-color CMD byte
        emit_expr_val(out, node.children[1], cfg, cs, dict_lookup);
        start = 2;
    }

    for (size_t i = start; i < node.children.size(); i++) {
        const auto& child = node.children[i];

        if (child.is_string()) {
            emit_text_chars(out, child.str_val, cs, cfg, dict_lookup);
        } else if (child.is_list("number")) {
            emit_stmt(out, child, cfg, cs, dict_lookup);
        } else if (child.is_integer()) {
            // a bare number is a fused protagonist name: (proc n)
            emit_stmt(out, AstNode::make_list("proc", {child}), cfg, cs, dict_lookup);
        } else if (child.is_variable()) {
            // a bare variable is a fused protagonist name: (call V)
            emit_stmt(out, AstNode::make_list("call", {child}), cfg, cs, dict_lookup);
        } else if (child.is_list("proc") || child.is_list("call")) {
            emit_stmt(out, child, cfg, cs, dict_lookup);
        } else if (child.is_character()) {
            auto sjis_opt = cs.char_to_sjis(child.char_val);

            if (sjis_opt.has_value() && sjis_opt->size() == 2) {
                emit_chr(out, (*sjis_opt)[0], (*sjis_opt)[1], cfg, dict_lookup);
            }
        }
    }
}

// ── sys opcode encoding ──────────────────────────────────────────────

// esoteric opcodes (0x29-0x2A) carry an extra 3-byte number
static bool is_esoteric_sys(uint8_t opcode) {
    return opcode >= 0x29 && opcode <= 0x2A;
}

// encode a number as 3 raw AI5 payload bytes (for esoteric sys opcodes)
static void emit_num_3bytes(ByteWriter& out, int n) {
    // 3 payload bytes, each carrying 6 bits
    int c2 = n & 0x3F;
    n >>= 6;
    int c1 = n & 0x3F;
    n >>= 6;
    int c0 = n & 0x3F;
    out.emit(static_cast<uint8_t>((c0 << 2) | 0x03));
    out.emit(static_cast<uint8_t>((c1 << 2) | 0x03));
    out.emit(static_cast<uint8_t>((c2 << 2) | 0x03));
}

static uint8_t checked_byte(int n, const std::string& context) {
    if (n < 0 || n > 255) {
        throw std::runtime_error("opcode out of byte range (" + context + "): " + std::to_string(n));
    }

    return static_cast<uint8_t>(n);
}

static void emit_sys(ByteWriter& out, const std::string& tag,
                      const std::vector<AstNode>& children, const Config& cfg,
                      Charset& cs, const std::unordered_map<uint32_t, int>& dict_lookup) {
    out.emit(TOK_SYS);

    // look up sys opcode
    auto it = sys_map().find(tag);
    uint8_t opcode;

    if (it != sys_map().end()) {
        opcode = it->second;
    } else if (tag.substr(0, 4) == "sys:") {
        opcode = checked_byte(std::stoi(tag.substr(4)), "sys");
    } else {
        throw std::runtime_error("ai5: unknown sys opcode: " + tag);
    }

    out.emit(opcode);

    // esoteric opcodes have an extra 3-byte number before params
    size_t params_start = 0;

    if (cfg.extra_op && is_esoteric_sys(opcode) && !children.empty() &&
        children[0].is_integer()) {
        emit_num_3bytes(out, children[0].int_val);
        params_start = 1;
    }

    emit_params(out, children, params_start, cfg, cs, dict_lookup);
}

// ── statement encoding ───────────────────────────────────────────────

static void emit_stmt(ByteWriter& out, const AstNode& node, const Config& cfg,
                       Charset& cs, const std::unordered_map<uint32_t, int>& dict_lookup) {

    if (!node.is_list()) {

        // cut node → CNT token
        if (node.is_cut()) {
            out.emit(TOK_CNT);
            return;
        }

        // bare string → STR encoding
        if (node.is_string()) {
            out.emit(TOK_STR);
            ai1::emit_str_bytes(out, node.str_val);
            out.emit(TOK_STR);
            return;
        }

        // bare value in statement position
        emit_expr(out, node, cfg, cs, dict_lookup);
        return;
    }

    const std::string& tag = node.tag;

    // ── meta / dict (skip) ──────────────────────────────────────
    if (tag == "meta" || tag == "dict") {
        return;
    }

    // ── text ─────────────────────────────────────────────────────
    if (tag == "text") {
        emit_text(out, node, cfg, cs, dict_lookup);
        return;
    }

    // ── text-raw ─────────────────────────────────────────────────
    if (tag == "text-raw") {
        // children are packed SJIS integers (j1*256 + j2)
        for (const auto& child : node.children) {

            if (child.is_integer()) {
                auto sjis = Charset::integer_to_sjis(child.int_val);

                if (sjis.size() == 2) {
                    emit_chr(out, sjis[0], sjis[1], cfg, dict_lookup);
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
                out.emit(0x10); // text-color CMD
                emit_expr_val(out, node.children[str_start], cfg, cs, dict_lookup);
                str_start++;
            }
        }

        out.emit(TOK_STR);

        if (str_start < node.children.size() && node.children[str_start].is_string()) {
            ai1::emit_str_bytes(out, node.children[str_start].str_val);
        }

        out.emit(TOK_STR);
        return;
    }

    // ── blocks ───────────────────────────────────────────────────
    if (tag == "<>") {
        out.emit(TOK_BEG);
        emit_stmts(out, node.children, 0, cfg, cs, dict_lookup);
        out.emit(TOK_END);
        return;
    }

    if (tag == "<*>") {
        emit_stmts(out, node.children, 0, cfg, cs, dict_lookup);
        return;
    }

    if (tag == "<.>") {
        out.emit(TOK_BEG);

        for (size_t i = 0; i < node.children.size(); i++) {

            if (i > 0) {
                out.emit(TOK_CNT);
            }

            emit_stmt(out, node.children[i], cfg, cs, dict_lookup);
        }

        out.emit(TOK_END);
        return;
    }

    // ── cut ──────────────────────────────────────────────────────
    if (tag == "cut") {
        out.emit(TOK_CNT);
        return;
    }

    // ── conditionals ─────────────────────────────────────────────
    if (tag == "if") {
        // (if expr block) → CND expr block
        if (node.children.size() >= 2) {
            out.emit(TOK_CND);
            emit_expr_val(out, node.children[0], cfg, cs, dict_lookup);
            emit_stmt(out, node.children[1], cfg, cs, dict_lookup);
        }

        return;
    }

    if (tag == "if-else") {
        // (if-else expr then else) → CND expr then CNT else
        if (node.children.size() >= 3) {
            out.emit(TOK_CND);
            emit_expr_val(out, node.children[0], cfg, cs, dict_lookup);
            emit_stmt(out, node.children[1], cfg, cs, dict_lookup);
            out.emit(TOK_CNT);
            emit_stmt(out, node.children[2], cfg, cs, dict_lookup);
        }

        return;
    }

    if (tag == "cond") {
        for (size_t i = 0; i < node.children.size(); i++) {

            if (i > 0) {
                out.emit(TOK_CNT);
            }

            const auto& branch = node.children[i];

            // else clause
            if (branch.is_list("else") && !branch.children.empty()) {
                emit_stmt(out, branch.children[0], cfg, cs, dict_lookup);
                continue;
            }

            // wildcard clause: (_ body) is an empty-condition branch
            if (branch.is_list("_") && !branch.children.empty()) {
                out.emit(TOK_CND);
                out.emit(TOK_VAL);
                emit_stmt(out, branch.children[0], cfg, cs, dict_lookup);
                continue;
            }

            // regular clause: (expr body)
            if (branch.is_list() && branch.children.size() >= 2) {
                out.emit(TOK_CND);
                emit_expr_val(out, branch.children[0], cfg, cs, dict_lookup);
                emit_stmt(out, branch.children[1], cfg, cs, dict_lookup);
            }
        }

        return;
    }

    // ── while ────────────────────────────────────────────────────
    if (tag == "while") {
        // (while expr block) → SYS:while CND expr block
        if (node.children.size() >= 2) {
            out.emit(TOK_SYS);
            out.emit(0x10);  // sys:while
            out.emit(TOK_CND);
            emit_expr_val(out, node.children[0], cfg, cs, dict_lookup);
            emit_stmt(out, node.children[1], cfg, cs, dict_lookup);
        }

        return;
    }

    // ── set operations ───────────────────────────────────────────
    if (tag == "set-var") {
        // (set-var V expr) → SETV V expr
        if (node.children.size() >= 2) {
            out.emit(TOK_SETV);
            emit_var(out, node.children[0]);
            emit_expr_val(out, node.children[1], cfg, cs, dict_lookup);
        }

        return;
    }

    if (tag == "set-reg:") {
        // (set-reg: num expr...) → SETRC num exprs
        if (node.children.size() >= 1) {
            out.emit(TOK_SETRC);
            emit_num(out, node.children[0].int_val);
            emit_exprs(out, node.children, 1, cfg, cs, dict_lookup);
        }

        return;
    }

    if (tag == "set-reg::") {
        // (set-reg:: expr exprs...) → SETRE expr exprs
        if (node.children.size() >= 1) {
            out.emit(TOK_SETRE);
            emit_expr_val(out, node.children[0], cfg, cs, dict_lookup);
            emit_exprs(out, node.children, 1, cfg, cs, dict_lookup);
        }

        return;
    }

    if (tag == "set-arr~") {
        // (set-arr~ V expr exprs...) → SETAW V expr exprs
        if (node.children.size() >= 2) {
            out.emit(TOK_SETAW);
            emit_var(out, node.children[0]);
            emit_expr_val(out, node.children[1], cfg, cs, dict_lookup);
            emit_exprs(out, node.children, 2, cfg, cs, dict_lookup);
        }

        return;
    }

    if (tag == "set-arr~b") {
        // (set-arr~b V expr exprs...) → SETAB V expr exprs
        if (node.children.size() >= 2) {
            out.emit(TOK_SETAB);
            emit_var(out, node.children[0]);
            emit_expr_val(out, node.children[1], cfg, cs, dict_lookup);
            emit_exprs(out, node.children, 2, cfg, cs, dict_lookup);
        }

        return;
    }

    // ── sys commands ─────────────────────────────────────────────
    auto sit = sys_map().find(tag);

    if (sit != sys_map().end()) {
        emit_sys(out, tag, node.children, cfg, cs, dict_lookup);
        return;
    }

    if (tag.size() > 4 && tag.substr(0, 4) == "sys:") {
        emit_sys(out, tag, node.children, cfg, cs, dict_lookup);
        return;
    }

    // ── generic commands via cmd_map ──────────────────────────────
    auto cit = cmd_map().find(tag);

    if (cit != cmd_map().end()) {
        out.emit(cit->second);
        emit_params(out, node.children, 0, cfg, cs, dict_lookup);
        return;
    }

    // ── cmd:N fallback ───────────────────────────────────────────
    if (tag.size() > 4 && tag.substr(0, 4) == "cmd:") {
        out.emit(checked_byte(std::stoi(tag.substr(4)), "cmd"));
        emit_params(out, node.children, 0, cfg, cs, dict_lookup);
        return;
    }

    // ── expression operators in statement position ───────────────
    auto eit = exp_map().find(tag);

    if (eit != exp_map().end()) {
        emit_expr(out, node, cfg, cs, dict_lookup);
        return;
    }

    if (tag == ":" || tag == "::" || tag == "?") {
        emit_expr(out, node, cfg, cs, dict_lookup);
        return;
    }

    // ── empty placeholder ────────────────────────────────────────
    if (tag == "_") {
        return;
    }

    throw std::runtime_error("ai5 compiler: unsupported node: " + tag);
}

static void emit_stmts(ByteWriter& out, const std::vector<AstNode>& nodes,
                        size_t start, const Config& cfg, Charset& cs,
                        const std::unordered_map<uint32_t, int>& dict_lookup) {

    for (size_t i = start; i < nodes.size(); i++) {
        emit_stmt(out, nodes[i], cfg, cs, dict_lookup);
    }
}

// ── dictionary encoding ─────────────────────────────────────────────

struct DictState {
    std::vector<std::vector<int>> entries;  // SJIS pairs
    std::unordered_map<uint32_t, int> lookup;  // sjis_pair_key → index
};

static DictState build_dict_from_ast(const AstNode& dict_node, Charset& cs) {
    DictState state;

    for (const auto& entry : dict_node.children) {

        if (entry.is_character()) {
            // decoded character → convert back to SJIS
            auto sjis_opt = cs.char_to_sjis(entry.char_val);

            if (sjis_opt.has_value() && sjis_opt->size() == 2) {
                uint32_t key = sjis_pair_key((*sjis_opt)[0], (*sjis_opt)[1]);
                state.lookup[key] = static_cast<int>(state.entries.size());
                state.entries.push_back(*sjis_opt);
            }
        } else if (entry.is_list("_sjis_")) {
            // raw SJIS pair (via quote)
            std::vector<int> pair;

            for (const auto& b : entry.children) {

                if (b.is_integer()) {
                    pair.push_back(b.int_val);
                }
            }

            if (pair.size() == 2) {
                uint32_t key = sjis_pair_key(pair[0], pair[1]);
                state.lookup[key] = static_cast<int>(state.entries.size());
                state.entries.push_back(pair);
            }
        } else if (entry.is_quote()) {
            // quoted entry: '(_sjis_ b1 b2)
            const auto& inner = entry.children.empty() ? entry : entry.children[0];

            if (inner.is_list("_sjis_")) {
                std::vector<int> pair;

                for (const auto& b : inner.children) {

                    if (b.is_integer()) {
                        pair.push_back(b.int_val);
                    }
                }

                if (pair.size() == 2) {
                    uint32_t key = sjis_pair_key(pair[0], pair[1]);
                    state.lookup[key] = static_cast<int>(state.entries.size());
                    state.entries.push_back(pair);
                }
            }
        }
    }

    return state;
}

static void emit_dict_header(ByteWriter& out, const DictState& dict) {
    // dict header: [entry_count * 2 as LE16] [sjis_pairs...]
    uint16_t byte_count = static_cast<uint16_t>(dict.entries.size() * 2);
    uint16_t offset = byte_count + 2;  // +2 for the offset field itself
    out.emit(static_cast<uint8_t>(offset & 0xFF));
    out.emit(static_cast<uint8_t>((offset >> 8) & 0xFF));

    for (const auto& pair : dict.entries) {
        out.emit(static_cast<uint8_t>(pair[0]));
        out.emit(static_cast<uint8_t>(pair[1]));
    }
}

// ── dict-build: auto-generate dictionary from text bigrams ──────────
static DictState build_dict_from_text(const AstNode& ast, const Config& cfg, Charset& cs) {
    // collect all text from the script as SJIS unit runs
    // this is a vector<vector<int>>
    std::vector<SjisRun> runs;
    collect_text_sjis(ast, cs, runs);

    int max_entries = 256 - static_cast<int>(cfg.dict_base);
    DictState state;
    std::unordered_map<uint32_t, int> pair_freq;

    for (const SjisRun &run : runs){
        for (size_t i = 0; i < run.size(); i++) {
            SjisUnit unit = run[i];
            uint32_t bi;

            if (unit.size() > 1) {
                //this is a sjis char & should go right into the dict
                bi = sjis_pair_key(unit[0], unit[1]);
            } else {
                // this is an ascii char, include the next character in the run
                if (i == run.size() - 1 || run[i+1].size() > 1) {
                    // skip the last one because it's not starting a bigram OR is a SJIS character
                    continue;
                }

                bi = sjis_pair_key(unit[0], run[i+1][0]);
            }

            if (pair_freq.find(bi) == pair_freq.end()) {
                //key isn't present, insert it
                pair_freq[bi] = 1;
            } else {
                pair_freq[bi]++;
            }
        }
    }
    
    // convert pair freq list to a vector and sort
    std::vector<std::pair<uint32_t, int>> bi_counts;
    bi_counts.reserve(pair_freq.size());
    for (auto &p : pair_freq){
        bi_counts.emplace_back(p.first, p.second);
    }

    if (bi_counts.empty()) {
        return state;
    }

    // bound max_entries to the amt of bigrams to avoid going over the array bounds
    int count = static_cast<int>(bi_counts.size());
    if (max_entries > count) {
        max_entries = count;
    }

    std::partial_sort(bi_counts.begin(), bi_counts.begin() + max_entries, bi_counts.end(),
                        [](const auto& a, const auto& b) { return a.second > b.second; });

    // build dict based on sorted bigram values
    for (int i = 0; i < max_entries; i++) {
        if (bi_counts[i].second >= 3){
            uint32_t key = bi_counts[i].first;
            state.lookup[key] = static_cast<int>(state.entries.size());
            state.entries.push_back({(int)key >> 16, (int)key & 0xFFFF});
        }
    }

    return state;
}

// ── top-level compile ────────────────────────────────────────────────

std::vector<uint8_t> compile_mes(const AstNode& ast, Config& cfg) {
    // extract configuration from meta node
    for (const auto& child : ast.children) {

        if (child.is_list("meta")) {

            for (const auto& entry : child.children) {

                if (entry.is_list("charset") && !entry.children.empty() &&
                    entry.children[0].is_string()) {
                    cfg.charset_name = entry.children[0].str_val;
                }

                if (entry.is_list("dictbase") && !entry.children.empty() &&
                    entry.children[0].is_integer()) {
                    cfg.dict_base = static_cast<uint8_t>(entry.children[0].int_val);
                }

                if (entry.is_list("extraop") && !entry.children.empty() &&
                    entry.children[0].is_boolean()) {
                    cfg.extra_op = entry.children[0].bool_val;
                }
            }
        }
    }

    Charset cs;
    cs.load(cfg.charset_name);

    // build dictionary from dict node or auto-generate from text
    DictState dict;

    for (const auto& child : ast.children) {

        if (child.is_list("dict")) {
            dict = build_dict_from_ast(child, cs);
            break;
        }

        if (child.is_list("dict-build") && cfg.compress) {
            dict = build_dict_from_text(ast, cfg, cs);
            break;
        }
    }

    ByteWriter out;

    // emit dictionary header
    if (!dict.entries.empty()) {
        emit_dict_header(out, dict);
    } else {
        // empty dict: offset = 2 (no dict bytes)
        out.emit(0x02);
        out.emit(0x00);
    }

    // emit statements (skip meta and dict)
    for (const auto& child : ast.children) {

        if (child.is_list("meta") || child.is_list("dict") || child.is_list("dict-build")) {
            continue;
        }

        emit_stmt(out, child, cfg, cs, dict.lookup);
    }

    // end marker
    out.emit(TOK_END);

    return out.take_data();
}

} // namespace ai5
