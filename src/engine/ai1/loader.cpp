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

#include "loader.h"
#include "opener.h"
#include "parser.h"
#include "../../charset.h"
#include "../../utf8.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ai1 {

// forward declarations
static AstNode lower_impl(const AstNode& node, const Config& cfg, const Charset& cs);
static AstNode lower_chr(const std::vector<int>& sjis_bytes, const Config& cfg, const Charset& cs);
static std::string resolve_exp(int byte_val);
static std::string resolve_cmd(int byte_val, bool do_resolve);
static AstNode resolve_impl(const AstNode& node, const Config& cfg);

// fuse passes (shared functions declared in loader.h)
static AstNode fuse_meta(const AstNode& node, const Config& cfg);

// --- main pipeline ---

AstNode load_mes(const std::string& path, Config& cfg) {
    Charset cs;
    cs.load(cfg.charset_name);

    // update config with charset's special characters
    cfg.char_space = cs.space_char();
    cfg.char_newline = cs.newline_char();

    auto code = open_mes(path);
    ByteStream stream(code);
    Parser parser(stream, cfg);
    auto parsed = parser.parse_mes();

    auto lowered = lower(parsed, cfg, cs);
    auto resolved = resolve(lowered, cfg);
    return fuse(resolved, cfg);
}

// --- lower ---

AstNode lower(const AstNode& node, const Config& cfg, const Charset& cs) {
    return lower_impl(node, cfg, cs);
}

static AstNode lower_impl(const AstNode& node, const Config& cfg, const Charset& cs) {
    if (node.is_list("num")) {
        // (num n) -> just n
        if (!node.children.empty() && node.children[0].is_integer()) {
            return node.children[0];
        }

        return node;
    }

    if (node.is_list("var")) {
        // (var byte) -> variable symbol (A-Z)
        if (!node.children.empty() && node.children[0].is_integer()) {
            int byte_val = node.children[0].int_val;
            char var_name = static_cast<char>(byte_val);
            return AstNode::make_variable(var_name);
        }

        return node;
    }

    if (node.is_list("chr-raw")) {
        // (chr-raw j1 j2) -> decoded character or kept raw
        std::vector<int> sjis_bytes;

        for (const auto& child : node.children) {

            if (child.is_integer()) {
                sjis_bytes.push_back(child.int_val);
            }
        }

        return lower_chr(sjis_bytes, cfg, cs);
    }

    if (node.is_list("expr")) {
        // (expr terms...) -> fold-expr on lowered terms
        std::vector<AstNode> lowered_terms;

        for (const auto& child : node.children) {
            lowered_terms.push_back(lower_impl(child, cfg, cs));
        }

        return fold_expr(lowered_terms);
    }

    if (node.is_list("param")) {
        // (param p) -> lower the inner value
        if (node.children.size() == 1) {
            return lower_impl(node.children[0], cfg, cs);
        }

        return node;
    }

    if (node.is_list("<*>")) {
        // (<*> s) -> unwrap single child
        if (node.children.size() == 1) {
            return lower_impl(node.children[0], cfg, cs);
        }

        std::vector<AstNode> lowered;

        for (const auto& child : node.children) {
            lowered.push_back(lower_impl(child, cfg, cs));
        }

        return AstNode::make_list("<*>", std::move(lowered));
    }

    if (node.is_list()) {
        // for any other list, flatten params into parent
        std::vector<AstNode> lowered;

        for (size_t i = 0; i < node.children.size(); i++) {
            const auto& child = node.children[i];

            if (child.is_list("params")) {
                // flatten params into parent
                for (const auto& param_child : child.children) {
                    lowered.push_back(lower_impl(param_child, cfg, cs));
                }
            } else {
                lowered.push_back(lower_impl(child, cfg, cs));
            }
        }

        return AstNode::make_list(node.tag, std::move(lowered));
    }

    // atoms pass through unchanged
    return node;
}

static AstNode lower_chr(const std::vector<int>& sjis_bytes, const Config& cfg, const Charset& cs) {
    auto opt_char = cs.sjis_to_char(sjis_bytes);

    if (opt_char.has_value()) {
        char32_t c = *opt_char;

        // check for special characters
        if (c == cfg.char_newline) {
            c = U'\n';
        } else if (c == cfg.char_continue) {
            c = U'\t';
        } else if (c == cfg.char_break) {
            c = U'\b';
        }

        if (cfg.decode) {
            return AstNode::make_list("chr", {AstNode::make_character(c)});
        }
    }

    // keep raw if decode is off or conversion failed
    std::vector<AstNode> children;

    for (int b : sjis_bytes) {
        children.push_back(AstNode::make_integer(b));
    }

    return AstNode::make_list("chr-raw", std::move(children));
}

AstNode fold_expr(const std::vector<AstNode>& terms) {
    // same fold-expr algorithm as AI5
    if (terms.empty()) {
        return AstNode::make_symbol("_");
    }

    struct FoldState {
        std::vector<AstNode> stack;
        size_t pos;
    };

    FoldState state;
    state.stack = {};
    state.pos = 0;

    while (state.pos < terms.size()) {
        const auto& term = terms[state.pos];

        if (term.is_list("term2") && state.stack.size() >= 2) {
            AstNode top = std::move(state.stack.back()); state.stack.pop_back();
            AstNode second = std::move(state.stack.back()); state.stack.pop_back();
            state.stack.push_back(AstNode::make_list("exp", {
                term.children[0], std::move(second), std::move(top)
            }));
        } else if (term.is_list("term1") && !state.stack.empty()) {
            AstNode a = std::move(state.stack.back()); state.stack.pop_back();
            state.stack.push_back(AstNode::make_list("exp", {
                term.children[0], std::move(a)
            }));
        } else if (term.is_list("term0") && state.pos + 1 < terms.size()) {
            state.pos++;
            const auto& operand = terms[state.pos];
            state.stack.push_back(AstNode::make_list("exp", {
                term.children[0], operand
            }));
        } else {
            state.stack.push_back(term);
        }

        state.pos++;
    }

    if (state.stack.size() == 1) {
        return state.stack[0];
    }

    if (state.stack.empty()) {
        return AstNode::make_symbol("_");
    }

    return state.stack.back();
}

// --- resolve ---

AstNode resolve(const AstNode& node, const Config& cfg) {
    return resolve_impl(node, cfg);
}

static AstNode resolve_impl(const AstNode& node, const Config& cfg) {
    if (node.is_list("exp")) {
        if (!node.children.empty() && node.children[0].is_integer()) {
            std::string op_name = resolve_exp(node.children[0].int_val);
            std::vector<AstNode> new_children;

            for (size_t i = 1; i < node.children.size(); i++) {
                new_children.push_back(resolve_impl(node.children[i], cfg));
            }

            return AstNode::make_list(op_name, std::move(new_children));
        }
    }

    if (node.is_list("cmd")) {
        if (!node.children.empty() && node.children[0].is_integer()) {
            std::string cmd_name = resolve_cmd(node.children[0].int_val, cfg.resolve);
            std::vector<AstNode> new_children;

            for (size_t i = 1; i < node.children.size(); i++) {
                new_children.push_back(resolve_impl(node.children[i], cfg));
            }

            return AstNode::make_list(cmd_name, std::move(new_children));
        }
    }

    if (node.is_list()) {
        std::vector<AstNode> new_children;

        for (const auto& child : node.children) {
            new_children.push_back(resolve_impl(child, cfg));
        }

        return AstNode::make_list(node.tag, std::move(new_children));
    }

    return node;
}

static std::string resolve_exp(int byte_val) {
    // translated from resolve-exp in ai1/mes-loader.rkt
    switch (byte_val) {
        case 0x21: return "!=";
        case 0x23: return "~b";
        case 0x25: return "%";
        case 0x26: return "&&";
        case 0x2A: return "*";
        case 0x2B: return "+";
        case 0x2D: return "-";
        case 0x2F: return "/";
        case 0x3C: return "<";
        case 0x3D: return "==";
        case 0x3E: return ">";
        case 0x3F: return "?";
        case 0x5C: return "~";
        case 0x5E: return "^";
        case 0x7C: return "//";
        default:   return "exp:" + std::to_string(byte_val);
    }
}

static std::string resolve_cmd(int byte_val, bool do_resolve) {
    // translated from resolve-cmd in ai1/mes-loader.rkt
    if (!do_resolve) {
        return "cmd:" + std::to_string(byte_val);
    }

    switch (byte_val) {
        case 0x99: return "set-reg:";
        case 0x9A: return "set-var";
        case 0x9B: return "set-arr~";
        case 0x9C: return "set-arr~b";
        case 0x9E: return "while";
        case 0x9F: return "continue";
        case 0xA0: return "break";
        case 0xA1: return "menu";
        case 0xA2: return "mes-jump";
        case 0xA3: return "mes-call";
        case 0xA4: return "define-proc";
        case 0xA5: return "com";
        case 0xA6: return "wait";
        case 0xA7: return "window";
        case 0xA8: return "text-position";
        case 0xA9: return "text-color";
        case 0xAA: return "clear";
        case 0xAB: return "number";
        case 0xAC: return "call";
        case 0xAD: return "image";
        case 0xAE: return "load";
        case 0xAF: return "execute";
        case 0xB0: return "recover";
        case 0xB1: return "set-mem";
        case 0xB2: return "screen";
        case 0xB3: return "mes-skip";
        case 0xB4: return "flag";
        case 0xB6: return "sound";
        case 0xB7: return "animate";
        case 0xB8: return "slot";
        case 0xB9: return "set-bg";
        // TODO: uncomment when we find test files containing these
        // case 0xBA: return "map-put";
        // case 0xBB: return "chara-put";
        // case 0xBC: return "window-put";
        default:   return "cmd:" + std::to_string(byte_val);
    }
}

// --- fuse ---

AstNode fuse(const AstNode& node, const Config& cfg) {
    // apply all 9 fuse passes in order (no fuse_dic/fuse_dict for AI1)
    auto result = node;
    result = fuse_while(result);
    result = fuse_operator(result);
    result = fuse_text(result);
    result = fuse_text_proc_call(result, cfg);
    result = fuse_text_number(result);
    result = fuse_text_multiple(result);
    result = fuse_text_color(result);
    result = fuse_menu_block(result);
    result = fuse_meta(result, cfg);
    return result;
}

// --- fuse passes ---

AstNode fuse_while(const AstNode& node) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (size_t i = 0; i < node.children.size(); i++) {
        auto child = fuse_while(node.children[i]);

        if (child.is_list("while") && child.children.empty() &&
            i + 1 < node.children.size()) {

            auto next = fuse_while(node.children[i + 1]);

            if (next.is_list("if") && next.children.size() == 2) {
                new_children.push_back(AstNode::make_list("while", {
                    std::move(next.children[0]),
                    std::move(next.children[1])
                }));
                i++;
                continue;
            }

            if (next.is_list("if-else") && next.children.size() == 3 &&
                next.children[2].is_list("<*>") && next.children[2].children.empty()) {
                new_children.push_back(AstNode::make_list("while", {
                    std::move(next.children[0]),
                    std::move(next.children[1])
                }));
                i++;
                continue;
            }

            new_children.push_back(std::move(child));
            continue;
        }

        new_children.push_back(std::move(child));
    }

    return AstNode::make_list(node.tag, std::move(new_children));
}

void flatten_assoc_op(const AstNode& node, const std::string& op, std::vector<AstNode>& out) {
    if (node.is_list(op)) {

        for (const auto& child : node.children) {
            flatten_assoc_op(child, op, out);
        }
    } else {
        out.push_back(node);
    }
}

AstNode fuse_operator(const AstNode& node) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (const auto& child : node.children) {
        new_children.push_back(fuse_operator(child));
    }

    auto result = AstNode::make_list(node.tag, std::move(new_children));

    static const std::unordered_set<std::string> assoc_ops = {"&&", "//", "+", "-", "*", "/"};

    if (assoc_ops.count(result.tag) && result.children.size() == 2) {

        if (result.children[0].is_list(result.tag)) {
            std::vector<AstNode> flattened;
            flatten_assoc_op(result.children[0], result.tag, flattened);
            flattened.push_back(std::move(result.children[1]));
            result.children = std::move(flattened);
        }
    }

    return result;
}

AstNode fuse_text(const AstNode& node) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (size_t i = 0; i < node.children.size(); i++) {
        auto child = fuse_text(node.children[i]);

        if (child.is_list("chrs")) {
            size_t j = 0;

            while (j < child.children.size()) {

                if (child.children[j].is_list("chr")) {
                    std::string text;

                    while (j < child.children.size() && child.children[j].is_list("chr")) {

                        if (!child.children[j].children.empty() &&
                            child.children[j].children[0].is_character()) {
                            text += char32_to_utf8(child.children[j].children[0].char_val);
                        }

                        j++;
                    }

                    if (!text.empty()) {
                        size_t pos = 0;

                        while (pos < text.size()) {
                            size_t nl = text.find('\n', pos);

                            if (nl == std::string::npos) {
                                new_children.push_back(AstNode::make_list("text", {
                                    AstNode::make_string(text.substr(pos))
                                }));
                                break;
                            }

                            new_children.push_back(AstNode::make_list("text", {
                                AstNode::make_string(text.substr(pos, nl - pos + 1))
                            }));
                            pos = nl + 1;
                        }
                    }

                    continue;
                }

                if (child.children[j].is_list("chr-raw")) {
                    std::vector<AstNode> raw_vals;

                    while (j < child.children.size() && child.children[j].is_list("chr-raw")) {
                        const auto& cr = child.children[j];

                        if (cr.children.size() == 2 &&
                            cr.children[0].is_integer() && cr.children[1].is_integer()) {
                            // pack SJIS byte pair into a single integer
                            int packed = cr.children[0].int_val * 256 + cr.children[1].int_val;
                            raw_vals.push_back(AstNode::make_integer(packed));
                        }

                        j++;
                    }

                    new_children.push_back(AstNode::make_list("text-raw", std::move(raw_vals)));
                    continue;
                }

                new_children.push_back(std::move(child.children[j]));
                j++;
            }

            continue;
        }

        new_children.push_back(std::move(child));
    }

    return AstNode::make_list(node.tag, std::move(new_children));
}

AstNode fuse_text_proc_call(const AstNode& node, const Config& cfg) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (size_t i = 0; i < node.children.size(); i++) {
        auto child = fuse_text_proc_call(node.children[i], cfg);

        if (child.is_list("proc") && child.children.size() == 1) {

            if (child.children[0].is_integer() && cfg.is_protag(child.children[0].int_val)) {
                new_children.push_back(AstNode::make_list("text", {
                    AstNode::make_integer(child.children[0].int_val)
                }));
                continue;
            }

            // a bare variable in text means (call V), so don't unwrap a proc on a variable
            if (child.children[0].is_variable() && cfg.is_protag(child.children[0].var_val)) {
                new_children.push_back(AstNode::make_list("text", {child}));
                continue;
            }
        }

        if (child.is_list("call") && child.children.size() == 1) {

            if (child.children[0].is_variable() && cfg.is_protag(child.children[0].var_val)) {
                new_children.push_back(AstNode::make_list("text", {child.children[0]}));
                continue;
            }
        }

        new_children.push_back(std::move(child));
    }

    return AstNode::make_list(node.tag, std::move(new_children));
}

AstNode fuse_text_number(const AstNode& node) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (size_t i = 0; i < node.children.size(); i++) {
        auto child = fuse_text_number(node.children[i]);

        if (child.is_list("number")) {
            new_children.push_back(AstNode::make_list("text", {std::move(child)}));
            continue;
        }

        new_children.push_back(std::move(child));
    }

    return AstNode::make_list(node.tag, std::move(new_children));
}

bool is_same_line(const AstNode& text_item) {
    if (text_item.is_string()) {
        const auto& s = text_item.str_val;
        return s.empty() || s.back() != '\n';
    }

    return true;
}

AstNode fuse_text_multiple(const AstNode& node) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (size_t i = 0; i < node.children.size(); i++) {
        auto child = fuse_text_multiple(node.children[i]);

        if (child.is_list("text") && !new_children.empty() &&
            new_children.back().is_list("text")) {

            auto& prev = new_children.back();

            if (!prev.children.empty() && is_same_line(prev.children.back())) {

                for (auto& tc : child.children) {
                    prev.children.push_back(std::move(tc));
                }

                continue;
            }
        }

        new_children.push_back(std::move(child));
    }

    return AstNode::make_list(node.tag, std::move(new_children));
}

AstNode fuse_text_color(const AstNode& node) {
    if (!node.is_list()) {
        return node;
    }

    std::vector<AstNode> new_children;

    for (size_t i = 0; i < node.children.size(); i++) {
        auto child = fuse_text_color(node.children[i]);

        if (child.is_list("text-color") && !child.children.empty() &&
            i + 1 < node.children.size()) {

            auto next = fuse_text_color(node.children[i + 1]);

            if (next.is_list("text")) {
                std::vector<AstNode> merged;
                merged.push_back(AstNode::make_keyword("color"));
                merged.push_back(child.children[0]);

                for (auto& tc : next.children) {
                    merged.push_back(std::move(tc));
                }

                new_children.push_back(AstNode::make_list("text", std::move(merged)));
                i++;
                continue;
            }

            new_children.push_back(std::move(child));
            new_children.push_back(std::move(next));
            i++;
            continue;
        }

        new_children.push_back(std::move(child));
    }

    return AstNode::make_list(node.tag, std::move(new_children));
}

AstNode fuse_menu_block(const AstNode& node) {
    // translated from fuse-menu-block in ai1/mes-loader.rkt
    // converts blocks with (cut) separators into menu blocks (<.>)

    if (!node.is_list()) {
        return node;
    }

    // recurse into children first
    std::vector<AstNode> recursed;

    for (const auto& child : node.children) {
        recursed.push_back(fuse_menu_block(child));
    }

    auto result = AstNode::make_list(node.tag, std::move(recursed));

    // only apply to <> blocks
    if (result.tag != "<>") {
        return result;
    }

    auto& ch = result.children;

    if (ch.size() < 2) {
        return result;
    }

    // validate the menu pattern: non-cut [, (cut) non-cut]* [, (cut)]
    std::vector<size_t> item_indices;
    size_t pos = 0;
    bool valid = true;
    bool has_empty_first = false;

    if (pos < ch.size() && !ch[pos].is_cut()) {
        item_indices.push_back(pos);
        pos++;
    } else if (pos < ch.size() && ch[pos].is_cut()) {
        has_empty_first = true;
        pos++;
    } else {
        return result;
    }

    while (pos < ch.size()) {

        if (!ch[pos].is_cut()) {
            valid = false;
            break;
        }

        pos++;

        if (pos < ch.size() && !ch[pos].is_cut()) {
            item_indices.push_back(pos);
            pos++;
        }
    }

    if (!valid || (item_indices.size() + (has_empty_first ? 1 : 0)) < 2) {
        return result;
    }

    // build menu items
    std::vector<AstNode> menu_items;

    if (has_empty_first) {
        menu_items.push_back(AstNode::make_list("_", {}));
    }

    for (size_t idx : item_indices) {
        menu_items.push_back(std::move(ch[idx]));
    }

    return AstNode::make_list("<.>", std::move(menu_items));
}

static AstNode fuse_meta(const AstNode& node, const Config& cfg) {
    if (!node.is_list("mes")) {
        return node;
    }

    std::vector<AstNode> meta_entries;

    // engine
    meta_entries.push_back(AstNode::make_list("engine", {
        AstNode::make_quote(AstNode::make_symbol("AI1"))
    }));

    // charset
    meta_entries.push_back(AstNode::make_list("charset", {
        AstNode::make_string(cfg.charset_name)
    }));

    // prepend meta to the mes children
    std::vector<AstNode> new_children;
    new_children.push_back(AstNode::make_list("meta", std::move(meta_entries)));

    for (auto& child : node.children) {
        new_children.push_back(std::move(child));
    }

    return AstNode::make_list("mes", std::move(new_children));
}

} // namespace ai1
