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

#include "parser.h"

#include <cstdio>

namespace adv {

Parser::Parser(ByteStream& stream, const Config& cfg)
    : stream_(stream), cfg_(cfg) {}

// --- lexer helpers ---

static std::string index_to_var_name(int i) {
    // excel-style column naming:
    // 0-25 → A-Z, 26-51 → AA-AZ, 52-77 → BA-BZ, etc.
    if (i < 26) {
        return std::string(1, static_cast<char>('A' + i));
    }

    int hi = (i - 26) / 26;
    int lo = (i - 26) % 26;
    return std::string(1, static_cast<char>('A' + hi)) +
           std::string(1, static_cast<char>('A' + lo));
}

AstNode Parser::integer_to_var(int i) {
    std::string name = index_to_var_name(i);

    if (name.size() == 1) {
        // single char: store as character node (matches racket's (var #\A) form)
        return AstNode::make_list("var", {AstNode::make_character(static_cast<char32_t>(name[0]))});
    }

    // multi-char: store as string node for the lower stage to convert to a symbol
    return AstNode::make_list("var", {AstNode::make_string(name)});
}

AstNode Parser::lex_reg(uint8_t c1, uint8_t c2) {
    // 2 bytes → 16 bits: [0000] [_:1] [f:1] [i:7] [o:3]
    uint16_t v = (static_cast<uint16_t>(c1) << 8) | c2;
    int f = (v >> 10) & 1;
    int i = (v >> 3) & 0x7F;
    int o = v & 0x07;
    int n = 8 * i + o - 1;

    return AstNode::make_list("reg*", {
        AstNode::make_integer(n),
        AstNode::make_quote(AstNode::make_symbol("=")),
        AstNode::make_integer(f)
    });
}

AstNode Parser::lex_var(uint8_t c1, uint8_t c2) {
    // 2 bytes → 16 bits: [0001] [f:2] [i:6] [v:4]
    uint16_t val = (static_cast<uint16_t>(c1) << 8) | c2;
    int f = (val >> 10) & 0x03;
    int i = (val >> 4) & 0x3F;
    int v = val & 0x0F;

    auto c = integer_to_var(i);

    std::string s;

    switch (f) {
        case 3: s = "+="; break;
        case 1: s = "-="; break;
        default: s = "="; break;
    }

    return AstNode::make_list("var*", {
        std::move(c),
        AstNode::make_quote(AstNode::make_symbol(s)),
        AstNode::make_integer(v)
    });
}

AstNode Parser::lex_var2(uint8_t c1, uint8_t c2, uint8_t c3) {
    // 3 bytes → 24 bits: [0001:4][f:3][m:1] [pad:2][i:5][j1:1] [pad:1][j2:7]
    uint32_t val = (static_cast<uint32_t>(c1) << 16) | (static_cast<uint32_t>(c2) << 8) | c3;
    int f = (val >> 17) & 0x07;
    int m = (val >> 16) & 0x01;
    int i = (val >> 9) & 0x1F;
    int j1_bit = (val >> 8) & 0x01;
    int j2_bits = val & 0x7F;

    // reconstruct j: j1 is the high bit (from c2 bit 0), j2 is lower 7 bits
    int j = (j1_bit << 7) | j2_bits;

    auto c = integer_to_var(i);

    AstNode v_node = (m == 1) ? integer_to_var(j) : AstNode::make_integer(j);

    std::string s;

    switch (f) {
        case 0: s = "="; break;
        case 1: s = "+="; break;
        case 2: s = "-="; break;
        default: s = "!="; break;
    }

    return AstNode::make_list("var*", {
        std::move(c),
        AstNode::make_quote(AstNode::make_symbol(s)),
        std::move(v_node)
    });
}

AstNode Parser::lex_num0(uint8_t c) {
    return AstNode::make_list("num", {AstNode::make_integer(c - 0x23)});
}

AstNode Parser::lex_num1(uint8_t c1, uint8_t c2) {
    (void)c1;
    return AstNode::make_list("num", {AstNode::make_integer(c2)});
}

AstNode Parser::lex_num2(uint8_t c1, uint8_t c2, uint8_t c3) {
    int i1 = c1;
    int i2 = c2;
    int i3 = c3;
    int val = (i1 - 0x29) * 0x4000 + i2 * 0x80 + (i3 & 0x7F);
    return AstNode::make_list("num", {AstNode::make_integer(val)});
}

AstNode Parser::lex_chr1(uint8_t c) {
    // chr1: maps to SJIS (0x82, 0x72 + c)
    return AstNode::make_list("chr-sjis2", {
        AstNode::make_integer(0x82),
        AstNode::make_integer(0x72 + c)
    });
}

AstNode Parser::lex_chr2(uint8_t c1, uint8_t c2) {
    return AstNode::make_list("chr-sjis2", {
        AstNode::make_integer(c1),
        AstNode::make_integer(c2)
    });
}

// --- token parsers ---

AstNode Parser::parse_reg_star() {
    uint8_t c1 = consume();
    uint8_t c2 = consume();
    return lex_reg(c1, c2);
}

AstNode Parser::parse_var_star() {

    if (cfg_.extra_op) {
        uint8_t c1 = consume();
        uint8_t c2 = consume();
        uint8_t c3 = consume();
        return lex_var2(c1, c2, c3);
    }

    uint8_t c1 = consume();
    uint8_t c2 = consume();
    return lex_var(c1, c2);
}

AstNode Parser::parse_num() {
    uint8_t b = peek();

    if (b >= 0x23 && b <= 0x27) {
        consume();
        return lex_num0(b);
    }

    if (b == 0x28) {
        uint8_t c1 = consume();
        uint8_t c2 = consume();
        return lex_num1(c1, c2);
    }

    if (b >= 0x29 && b <= 0x2C) {
        uint8_t c1 = consume();
        uint8_t c2 = consume();
        uint8_t c3 = consume();
        return lex_num2(c1, c2, c3);
    }

    throw ParseError("expected number token", stream_.position());
}

AstNode Parser::parse_arg() {
    // ARG: 0x22 + chars + 0x22
    expect(0x22);
    std::vector<AstNode> chars;

    while (!at_end()) {
        uint8_t b = peek();

        if (b == 0x22) {
            consume();
            break;
        }

        // cASCII: 0x20-0x7E
        if (b >= 0x20 && b <= 0x7E) {
            consume();
            chars.push_back(AstNode::make_list("chr-ascii", {AstNode::make_character(static_cast<char32_t>(b))}));
            continue;
        }

        // cSJIS1: 0xA1-0xDF
        if (b >= 0xA1 && b <= 0xDF) {
            consume();
            chars.push_back(AstNode::make_list("chr-sjis1", {AstNode::make_integer(b)}));
            continue;
        }

        // cSJIS2: lead byte 0x80-0x9F or 0xE0-0xEA
        if ((b >= 0x80 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEA)) {
            uint8_t c1 = consume();
            uint8_t c2 = consume();
            chars.push_back(AstNode::make_list("chr-sjis2", {
                AstNode::make_integer(c1),
                AstNode::make_integer(c2)
            }));
            continue;
        }

        // cBYTE: fallback for any byte 0x00-0xFF
        consume();
        chars.push_back(AstNode::make_list("chr-byte", {AstNode::make_character(static_cast<char32_t>(b))}));
    }

    return AstNode::make_list("arg", std::move(chars));
}

AstNode Parser::parse_chrs_escaped() {
    // CHRS!: 0x21 + chars + (0x00 | 0xFF)
    expect(0x21);
    std::vector<AstNode> chars;

    while (!at_end()) {
        uint8_t b = peek();

        // terminator: 0x00 or 0xFF
        if (b == 0x00 || b == 0xFF) {
            consume();
            break;
        }

        // try cASCII first: 0x20-0x7E
        if (b >= 0x20 && b <= 0x7E) {
            consume();
            chars.push_back(AstNode::make_list("chr-ascii", {AstNode::make_character(static_cast<char32_t>(b))}));
            continue;
        }

        // cSJIS1: 0xA1-0xDF
        if (b >= 0xA1 && b <= 0xDF) {
            consume();
            chars.push_back(AstNode::make_list("chr-sjis1", {AstNode::make_integer(b)}));
            continue;
        }

        // cSJIS2: lead 0x80-0x9F or 0xE0-0xEA, follow 0x40-0x7E or 0x80-0xFC
        if ((b >= 0x80 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEA)) {
            uint8_t c1 = consume();
            uint8_t c2 = consume();
            chars.push_back(AstNode::make_list("chr-sjis2", {
                AstNode::make_integer(c1),
                AstNode::make_integer(c2)
            }));
            continue;
        }

        // cSJIS2+: lead 0xEB-0xEF
        if (b >= 0xEB && b <= 0xEF) {
            uint8_t c1 = consume();
            uint8_t c2 = consume();
            chars.push_back(AstNode::make_list("chr-sjis2+", {
                AstNode::make_integer(c1),
                AstNode::make_integer(c2)
            }));
            continue;
        }

        // shouldn't reach here given the terminator check, but consume anyway
        consume();
        chars.push_back(AstNode::make_list("chr-ascii", {AstNode::make_character(static_cast<char32_t>(b))}));
    }

    return AstNode::make_list("chrs!", std::move(chars));
}

AstNode Parser::parse_chr() {
    uint8_t b = peek();

    // CHR1: 0x2D-0x7F
    if (b >= 0x2D && b <= 0x7F) {
        consume();
        return lex_chr1(b);
    }

    // CHR2 range (0x80-0x9F, 0xE0-0xEA) but must check for special sequences first
    if ((b >= 0x80 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEA)) {

        // the 0x81 byte needs special handling: check for CHR$ sequences
        if (b == 0x81 && !peek_chr_special()) {
            // not a special sequence, parse as CHR2
            uint8_t c1 = consume();
            uint8_t c2 = consume();
            return lex_chr2(c1, c2);
        }

        if (b == 0x81 && peek_chr_special()) {
            // this is a special sequence, not a CHR2
            throw ParseError("chr$ sequence in chr context", stream_.position());
        }

        // non-0x81 lead byt safe to parse as CHR2
        uint8_t c1 = consume();
        uint8_t c2 = consume();
        return lex_chr2(c1, c2);
    }

    // CHR2+: 0xEB-0xEF
    if (b >= 0xEB && b <= 0xEF) {
        uint8_t c1 = consume();
        uint8_t c2 = consume();
        return lex_chr2(c1, c2);
    }

    throw ParseError("expected character token, got 0x" +
        std::to_string(b), stream_.position());
}

AstNode Parser::parse_chrs() {
    std::vector<AstNode> chars;
    chars.push_back(parse_chr());

    while (!at_end() && peek_is_chr()) {
        chars.push_back(parse_chr());
    }

    return AstNode::make_list("chrs", std::move(chars));
}

AstNode Parser::parse_cmd() {
    uint8_t b = consume();
    return AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(b))});
}

// --- special 2-byte sequence detection ---

bool Parser::peek_chr_lbeg() const {
    if (stream_.position() + 1 >= stream_.size()) {
        return false;
    }

    return stream_.data()[stream_.position()] == 0x81 &&
           stream_.data()[stream_.position() + 1] == 0x6F;
}

bool Parser::peek_chr_lend() const {
    if (stream_.position() + 1 >= stream_.size()) {
        return false;
    }

    return stream_.data()[stream_.position()] == 0x81 &&
           stream_.data()[stream_.position() + 1] == 0x70;
}

bool Parser::peek_chr_wait() const {
    if (stream_.position() + 1 >= stream_.size()) {
        return false;
    }

    return stream_.data()[stream_.position()] == 0x81 &&
           stream_.data()[stream_.position() + 1] == 0x90;
}

bool Parser::peek_chr_nop() const {
    if (stream_.position() + 1 >= stream_.size()) {
        return false;
    }

    return stream_.data()[stream_.position()] == 0x81 &&
           stream_.data()[stream_.position() + 1] == 0x97;
}

bool Parser::peek_chr_special() const {
    return peek_chr_lbeg() || peek_chr_lend() || peek_chr_wait() || peek_chr_nop();
}

bool Parser::peek_eos() const {
    if (stream_.position() + 1 >= stream_.size()) {
        return false;
    }

    return stream_.data()[stream_.position()] == 0xFF &&
           stream_.data()[stream_.position() + 1] == 0xFF;
}

bool Parser::peek_eom() const {
    if (stream_.position() + 1 >= stream_.size()) {
        return false;
    }

    return stream_.data()[stream_.position()] == 0xFF &&
           stream_.data()[stream_.position() + 1] == 0xFE;
}

void Parser::consume_chr_lbeg() {
    expect(0x81);
    expect(0x6F);
}

void Parser::consume_chr_lend() {
    expect(0x81);
    expect(0x70);
}

// --- grammar rules ---

AstNode Parser::parse_reg_bang() {
    // reg! → (set-reg n v)
    auto reg = parse_reg_star();
    // reg* has children: [n, '=, f]
    auto& ch = reg.children;
    int n = ch[0].int_val;
    int f = ch[2].int_val;

    bool v = (f == 1);
    return AstNode::make_list("set-reg", {
        AstNode::make_integer(n),
        AstNode::make_boolean(v)
    });
}

AstNode Parser::parse_var_bang() {
    // var! → (set-var|inc-var|dec-var|set-var2 c v)
    auto var = parse_var_star();
    // var* has children: [(var C), 'op, val]
    auto& ch = var.children;

    // extract the operation symbol
    std::string op_str;

    if (ch[1].is_quote() && !ch[1].children.empty()) {
        op_str = ch[1].children[0].str_val;
    }

    std::string tag;

    if (op_str == "+=") {
        tag = "inc-var";
    } else if (op_str == "-=") {
        tag = "dec-var";
    } else if (op_str == "!=") {
        tag = "set-var2";
    } else {
        tag = "set-var";
    }

    return AstNode::make_list(tag, {std::move(ch[0]), std::move(ch[2])});
}

AstNode Parser::parse_reg_question() {
    // reg? → (= n v)
    auto reg = parse_reg_star();
    auto& ch = reg.children;
    int n = ch[0].int_val;
    int f = ch[2].int_val;

    bool v = (f == 1);
    return AstNode::make_list("=", {
        AstNode::make_integer(n),
        AstNode::make_boolean(v)
    });
}

AstNode Parser::parse_var_question() {
    // var? → (op c v)
    auto var = parse_var_star();
    auto& ch = var.children;

    std::string op_str;

    if (ch[1].is_quote() && !ch[1].children.empty()) {
        op_str = ch[1].children[0].str_val;
    }

    std::string tag;

    if (op_str == "+=") {
        tag = ">=";
    } else if (op_str == "-=") {
        tag = "<=";
    } else if (op_str == "!=") {
        tag = "!=";
    } else {
        tag = "=";
    }

    return AstNode::make_list(tag, {std::move(ch[0]), std::move(ch[2])});
}

AstNode Parser::parse_conds() {
    // conds = (? reg?... var?...)
    std::vector<AstNode> conds;

    while (!at_end()) {
        uint8_t b = peek();

        // reg? range: 0x00-0x0F
        if (b >= 0x00 && b <= 0x0F) {
            conds.push_back(parse_reg_question());
            continue;
        }

        // var? range: 0x10-0x1F
        if (b >= 0x10 && b <= 0x1F) {
            conds.push_back(parse_var_question());
            continue;
        }

        break;
    }

    auto result = AstNode::make_list("?", std::move(conds));
    return result;
}

AstNode Parser::parse_param() {
    uint8_t b = peek();

    // NUM
    if (peek_is_num()) {
        return AstNode::make_list("param", {parse_num()});
    }

    // ARG
    if (b == 0x22) {
        return AstNode::make_list("param", {parse_arg()});
    }

    // block, block*, block/
    // order matches racket grammar: plain block first, then block*, then block/.
    // plain block parses stmts which will fail on a top-level CNT, allowing
    // fallthrough to block* or block/. but if the CNT is nested inside a
    // sub-block (e.g. inside an if), stmts succeeds and we get <> not </>.
    if (b == 0xA2) {
        auto block_result = try_parse([this]() { return parse_block(); });

        if (block_result.has_value()) {
            return AstNode::make_list("param", {std::move(*block_result)});
        }

        auto block_star_result = try_parse([this]() { return parse_block_star(); });

        if (block_star_result.has_value()) {
            return AstNode::make_list("param", {std::move(*block_star_result)});
        }

        return AstNode::make_list("param", {parse_block_slash()});
    }

    throw ParseError("expected param", stream_.position());
}

AstNode Parser::parse_params() {
    std::vector<AstNode> params;

    while (!at_end() && peek_is_param()) {
        params.push_back(parse_param());
    }

    return AstNode::make_list("params", std::move(params));
}

AstNode Parser::parse_text() {
    uint8_t b = peek();

    // CHRS!
    if (b == 0x21) {
        return parse_chrs_escaped();
    }

    // CHRS
    if (peek_is_chr()) {
        return parse_chrs();
    }

    throw ParseError("expected text", stream_.position());
}

bool Parser::peek_is_block_end() const {
    if (at_end()) {
        return false;
    }

    uint8_t b = peek();

    // explicit END
    if (b == 0xA3) {
        return true;
    }

    // implicit END at segment boundary
    if (peek_eos()) {
        return true;
    }

    // implicit END at loop boundary
    if (peek_chr_lend()) {
        return true;
    }

    return false;
}

bool Parser::peek_is_loop_end() const {
    if (at_end()) {
        return false;
    }

    // CHR-LEND
    if (peek_chr_lend()) {
        return true;
    }

    // implicit at EOS
    if (peek_eos()) {
        return true;
    }

    return false;
}

void Parser::consume_block_end() {
    // block-end: END | lookAhead(EOS) | lookAhead(CHR-LEND)
    // must match one of these, otherwise the block parse fails

    if (!at_end() && peek() == 0xA3) {
        consume(); // consume END
        return;
    }

    if (peek_eos()) {
        return; // implicit end at segment boundary (not consumed)
    }

    if (peek_chr_lend()) {
        return; // implicit end at loop boundary (not consumed)
    }

    throw ParseError("expected block end (END, EOS, or CHR-LEND)", stream_.position());
}

void Parser::consume_loop_end() {
    // loop-end: CHR-LEND | lookAhead(EOS)
    // must match one of these

    if (peek_chr_lend()) {
        consume_chr_lend();
        return;
    }

    if (peek_eos()) {
        return; // implicit end at segment boundary (not consumed)
    }

    throw ParseError("expected loop end (CHR-LEND or EOS)", stream_.position());
}

AstNode Parser::parse_block() {
    // block: BEG stmts block-end → (<> stmts...)
    expect(0xA2);
    auto stmts = parse_stmts();
    consume_block_end();
    return AstNode::make_list("<>", std::move(stmts));
}

AstNode Parser::parse_item_plus() {
    // item+: conds stmts → (+ conds stmts...)
    auto c = parse_conds();
    auto s = parse_stmts();

    std::vector<AstNode> children;
    children.push_back(std::move(c));

    for (auto& stmt : s) {
        children.push_back(std::move(stmt));
    }

    return AstNode::make_list("+", std::move(children));
}

std::vector<AstNode> Parser::parse_items_plus() {
    // items+: sepBy(item+, CNT)
    std::vector<AstNode> items;
    items.push_back(parse_item_plus());

    while (!at_end() && peek() == 0xA4) {
        consume(); // consume CNT
        items.push_back(parse_item_plus());
    }

    return items;
}

AstNode Parser::parse_item() {
    // item: conds stmts BEG+ items+ END* → (* conds stmts... (<+> items+...))
    auto c = parse_conds();
    auto s = parse_stmts();

    expect(0xA0); // BEG+
    auto ip = parse_items_plus();
    expect(0xA1); // END*

    std::vector<AstNode> children;
    children.push_back(std::move(c));

    for (auto& stmt : s) {
        children.push_back(std::move(stmt));
    }

    children.push_back(AstNode::make_list("<+>", std::move(ip)));
    return AstNode::make_list("*", std::move(children));
}

AstNode Parser::parse_block_star() {
    // block*: BEG items block-end → (<*> items...)
    expect(0xA2);
    std::vector<AstNode> items;

    while (!at_end() && !peek_is_block_end()) {
        items.push_back(parse_item());
    }

    if (items.empty()) {
        throw ParseError("block* requires at least one item", stream_.position());
    }

    consume_block_end();
    return AstNode::make_list("<*>", std::move(items));
}

AstNode Parser::parse_branch() {
    // branch: stmts → (/ stmts...)
    auto s = parse_stmts();
    return AstNode::make_list("/", std::move(s));
}

AstNode Parser::parse_branch_q() {
    // branch?: conds stmts [LEND] → (// conds stmts...)
    auto c = parse_conds();
    auto s = parse_stmts();

    // consume trailing LEND only when followed by CNT, confirming it's a
    // branch-internal marker (seen in bishoujo _hp files) rather
    // than a loop terminator that belongs to an enclosing LBEG..LEND.
    if (!at_end() && peek_chr_lend() &&
        stream_.position() + 2 < stream_.size() &&
        stream_.data()[stream_.position() + 2] == 0xA4) {
        consume_chr_lend();
    }

    std::vector<AstNode> children;
    children.push_back(std::move(c));

    for (auto& stmt : s) {
        children.push_back(std::move(stmt));
    }

    return AstNode::make_list("//", std::move(children));
}

AstNode Parser::parse_block_slash() {
    // block/: BEG branches block-end → (</> branches...)
    expect(0xA2);

    // sepBy1(branch, CNT)
    std::vector<AstNode> branches;
    branches.push_back(parse_branch());

    while (!at_end() && !peek_is_block_end() && peek() == 0xA4) {
        consume(); // consume CNT
        branches.push_back(parse_branch());
    }

    consume_block_end();
    return AstNode::make_list("</>", std::move(branches));
}

AstNode Parser::parse_block_slash_q() {
    // block/?: BEG branches? block-end → (</> branches?...)
    expect(0xA2);

    // sepBy1(branch?, CNT)
    std::vector<AstNode> branches;
    branches.push_back(parse_branch_q());

    while (!at_end() && !peek_is_block_end() && peek() == 0xA4) {
        consume(); // consume CNT
        branches.push_back(parse_branch_q());
    }

    consume_block_end();
    return AstNode::make_list("</>", std::move(branches));
}

AstNode Parser::parse_loop() {
    // loop: CHR-LBEG stmts [loop-end] → (loop stmts...)
    // some games (e.g. bishoujo tsuushin) use LEND inside conditional
    // blocks as "continue loop" (goto LBEG). when all LENDs in the loop
    // body are consumed by branch parsing, the loop has no remaining
    // LEND of its own. this is probably valid, I think? I am open to
    // hearing improvements.
    size_t loop_start = stream_.position();
    consume_chr_lbeg();
    auto stmts = parse_stmts();

    if (!at_end() && peek_chr_lend()) {
        consume_chr_lend();
    } else if (!peek_eos()) {
        // open loop: no LEND found, all were consumed internally
    }

    return AstNode::make_list("loop", std::move(stmts));
}

// --- special operations ---

AstNode Parser::parse_op_chr() {
    // CHR-WAIT → (wait$), CHR-NOP → (nop@)
    if (peek_chr_wait()) {
        consume();
        consume();
        return AstNode::make_list("wait$", {});
    }

    if (peek_chr_nop()) {
        consume();
        consume();
        return AstNode::make_list("nop@", {});
    }

    throw ParseError("expected chr operation", stream_.position());
}

AstNode Parser::parse_op_menu() {
    // (0xAD|0xAE) NUM* text* (block/|block*) → ((cmd c) nums... texts... block)
    uint8_t c = consume();

    // many NUM
    std::vector<AstNode> nums;

    while (!at_end() && peek_is_num()) {
        nums.push_back(parse_num());
    }

    // many text
    std::vector<AstNode> texts;

    while (!at_end() && peek_is_text()) {
        texts.push_back(parse_text());
    }

    // block/ or block*
    AstNode block_node = AstNode::make_list("_", {});

    auto block_slash_result = try_parse([this]() { return parse_block_slash(); });

    if (block_slash_result.has_value()) {
        block_node = std::move(*block_slash_result);
    } else {
        block_node = parse_block_star();
    }

    // build result: ((cmd c) nums... texts... block)
    std::vector<AstNode> children;
    children.push_back(AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}));

    for (auto& n : nums) {
        children.push_back(std::move(n));
    }

    for (auto& t : texts) {
        children.push_back(std::move(t));
    }

    children.push_back(std::move(block_node));
    return AstNode::make_list("", std::move(children));
}

AstNode Parser::parse_op_if_when() {
    // (0xBC|0xBD) block/? → ((cmd c) block)
    uint8_t c = consume();
    auto block = parse_block_slash_q();

    return AstNode::make_list("", {
        AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}),
        std::move(block)
    });
}

AstNode Parser::parse_op_branch_var() {
    // 0xB4 VAR block/ → ((cmd c) var block)
    uint8_t c = consume();

    // VAR: single uppercase letter A-Z
    uint8_t var_byte = consume();
    auto var_node = AstNode::make_list("var", {AstNode::make_character(static_cast<char32_t>(var_byte))});

    auto block = parse_block_slash();

    return AstNode::make_list("", {
        AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}),
        std::move(var_node),
        std::move(block)
    });
}

AstNode Parser::parse_op_execute_var() {
    // 0xB6 (ARG | VAR NUM) → ((cmd c) args...)
    uint8_t c = consume();

    std::vector<AstNode> children;
    children.push_back(AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}));

    uint8_t b = peek();

    if (b == 0x22) {
        // ARG
        children.push_back(parse_arg());
    } else if (b >= 'A' && b <= 'Z') {
        // VAR NUM
        uint8_t var_byte = consume();
        children.push_back(AstNode::make_list("var", {AstNode::make_character(static_cast<char32_t>(var_byte))}));
        children.push_back(parse_num());
    } else {
        throw ParseError("expected ARG or VAR in execute-var", stream_.position());
    }

    return AstNode::make_list("", std::move(children));
}

AstNode Parser::parse_print_var() {
    // 0xCA (print-var) VAR NUM* → ((cmd c) var nums...)
    uint8_t c = consume();

    uint8_t var_byte = consume();
    auto var_node = AstNode::make_list("var", {AstNode::make_character(static_cast<char32_t>(var_byte))});

    std::vector<AstNode> children;
    children.push_back(AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}));
    children.push_back(std::move(var_node));

    // many NUM (variable length)
    while (!at_end() && peek_is_num()) {
        children.push_back(parse_num());
    }

    return AstNode::make_list("", std::move(children));
}

AstNode Parser::parse_op_sound() {
    // 0xD0 alphanums* spaces* params → ((cmd c) (params 'symbol params...))
    uint8_t c = consume();

    // read alphanums (se, fm, pm, cd)
    std::string symbol_str;

    while (!at_end()) {
        uint8_t b = peek();

        if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')) {
            symbol_str += static_cast<char>(consume());
        } else {
            break;
        }
    }

    // read spaces (0x20)
    while (!at_end() && peek() == 0x20) {
        symbol_str += static_cast<char>(consume());
    }

    // parse remaining params
    auto params = parse_params();

    // build result: ((cmd c) (params 'symbol rest...))
    std::vector<AstNode> params_children;
    params_children.push_back(AstNode::make_quote(AstNode::make_symbol(symbol_str)));

    for (auto& ch : params.children) {
        params_children.push_back(std::move(ch));
    }

    return AstNode::make_list("", {
        AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}),
        AstNode::make_list("params", std::move(params_children))
    });
}

AstNode Parser::parse_op_decrypt() {
    // 0xD5 NUM → if n=0: ((cmd c) n), else: decrypt + re-parse
    uint8_t c = consume();
    auto num = parse_num();

    int n = 0;

    if (!num.children.empty() && num.children[0].is_integer()) {
        n = num.children[0].int_val;
    }

    if (n == 0) {
        return AstNode::make_list("", {
            AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}),
            std::move(num)
        });
    }

    // collect remaining bytes until EOS
    std::vector<uint8_t> encrypted;

    while (!at_end() && !peek_eos()) {
        encrypted.push_back(consume());
    }

    // decrypt
    uint8_t key = static_cast<uint8_t>(n);
    std::vector<uint8_t> decrypted;

    for (uint8_t v : encrypted) {
        uint8_t flipped = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
        decrypted.push_back(flipped ^ key);
    }

    // re-parse decrypted content as a <mes> sub-stream
    ByteStream sub_stream(std::move(decrypted));
    Parser sub_parser(sub_stream, cfg_);
    auto parsed = sub_parser.parse_mes();

    return AstNode::make_list("", {
        AstNode::make_list("cmd", {AstNode::make_character(static_cast<char32_t>(c))}),
        std::move(num),
        std::move(parsed)
    });
}

AstNode Parser::parse_op_cmd_star() {
    // try special operations in order
    uint8_t b = peek();

    // op-chr: CHR-WAIT or CHR-NOP
    if (peek_chr_wait() || peek_chr_nop()) {
        return parse_op_chr();
    }

    // op-menu: 0xAD or 0xAE
    if (b == 0xAD || b == 0xAE) {
        return parse_op_menu();
    }

    // op-if-when: 0xBC or 0xBD
    if (b == 0xBC || b == 0xBD) {
        return parse_op_if_when();
    }

    // op-branch-var: 0xB4
    if (b == 0xB4) {
        return parse_op_branch_var();
    }

    // op-execute-var: 0xB6
    if (b == 0xB6) {
        return parse_op_execute_var();
    }

    // print-var: 0xCA (extraop only)
    if (b == 0xCA && cfg_.extra_op) {
        return parse_print_var();
    }

    // op-sound: 0xD0
    if (b == 0xD0) {
        return parse_op_sound();
    }

    // op-decrypt: 0xD5 (extraop + decrypt_op)
    if (b == 0xD5 && cfg_.extra_op && cfg_.decrypt_op) {
        return parse_op_decrypt();
    }

    throw ParseError("not a special op-cmd*", stream_.position());
}

AstNode Parser::parse_op_cmd() {
    // CMD params → ((cmd c) params...)
    auto cmd = parse_cmd();
    auto params = parse_params();

    // flatten: (cmd params)
    std::vector<AstNode> children;
    children.push_back(std::move(cmd));
    children.push_back(std::move(params));

    return AstNode::make_list("", std::move(children));
}

AstNode Parser::parse_op() {
    uint8_t b = peek();

    // reg!: 0x00-0x0F
    if (b >= 0x00 && b <= 0x0F) {
        return parse_reg_bang();
    }

    // var!: 0x10-0x1F
    if (b >= 0x10 && b <= 0x1F) {
        return parse_var_bang();
    }

    // try op-cmd* first (special operations)
    auto cmd_star_result = try_parse([this]() { return parse_op_cmd_star(); });

    if (cmd_star_result.has_value()) {
        return std::move(*cmd_star_result);
    }

    // fallback: op-cmd (generic CMD params)
    if (b >= 0xA5 && b <= 0xDF) {
        return parse_op_cmd();
    }

    throw ParseError("expected operation", stream_.position());
}

AstNode Parser::parse_stmt() {
    // stmt: loops | op | text | block
    // block-as-statement is not in the racket grammar but occurs in
    // practice (e.g. xgirl/MD_D03 has nested BEG inside define-proc).

    // loops: try CHR-LBEG first
    if (peek_chr_lbeg()) {
        size_t lbeg_pos = stream_.position();
        auto loop_result = try_parse([this]() { return parse_loop(); });

        if (loop_result.has_value()) {
            return std::move(*loop_result);
        }
    }

    // op
    if (peek_is_op()) {
        return parse_op();
    }

    // text: CHRS! or CHRS
    if (peek_is_text()) {
        return parse_text();
    }

    // standalone BEG+ ... END* as statement: appears at the tail of
    // if-block branches (e.g. bishoujo robsys5). same structure as the
    // BEG+/END* part of parse_item(), but without a preceding BEG block.
    if (peek() == 0xA0) {
        consume(); // BEG+
        auto items = parse_items_plus();
        expect(0xA1); // END*
        return AstNode::make_list("<+>", std::move(items));
    }

    // bare block as statement: BEG ... END appearing as a standalone stmt.
    // not in the racket grammar but occurs in practice (e.g. xgirl/MD_D03
    // has nested BEG inside define-proc). order matches racket param grammar:
    // plain block first, then block*, then block/.
    if (peek() == 0xA2) {
        auto block_result = try_parse([this]() { return parse_block(); });

        if (block_result.has_value()) {
            return std::move(*block_result);
        }

        auto block_star_result = try_parse([this]() { return parse_block_star(); });

        if (block_star_result.has_value()) {
            return std::move(*block_star_result);
        }

        return parse_block_slash();
    }

    throw ParseError("expected statement, got byte 0x" +
        std::to_string(peek()), stream_.position());
}

std::vector<AstNode> Parser::parse_stmts() {
    std::vector<AstNode> stmts;

    while (!at_end() && peek_is_stmt()) {
        stmts.push_back(parse_stmt());
    }

    return stmts;
}

AstNode Parser::parse_seg() {
    // seg: conds stmts [structural-tokens] EOS
    auto c = parse_conds();
    auto s = parse_stmts();

    // segment-level recovery: skip content between stmts and EOS that
    // the parser can't match as a stmt. the game engine processes
    // bytecode linearly and tolerates these; the racket decompiler
    // crashes on most of them. known patterns:
    //   - bare CNT at top level (JYB/m_memo.mes)
    //   - dangling CHR-LEND without matching LBEG (cm2/hit.mes)
    //   - orphaned block-structural tokens (marine/000012, cosps/ACT2-1S)
    //   - stray single bytes (dracula/d03ds.mes)
    // helper for hex formatting in warnings
    auto hex = [](uint8_t v) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02X", v);
        return std::string(buf);
    };

    while (!at_end() && !peek_eos()) {

        // skip block-structural tokens (BEG+, END*, BEG, END, CNT)
        if (peek() >= 0xA0 && peek() <= 0xA4) {
            warnings_.push_back("seg-recovery: skipped orphaned structural token " +
                hex(peek()) + " at position " + std::to_string(stream_.position()));
            consume();
        }

        // skip dangling CHR-LEND (0x81 0x70) at segment level
        else if (peek_chr_lend()) {
            warnings_.push_back("seg-recovery: skipped dangling CHR-LEND at position " +
                std::to_string(stream_.position()));
            consume();
            consume();
        }

        // try to parse more stmts if something parseable follows
        else if (peek_is_stmt()) {
            auto more = parse_stmts();

            for (auto& stmt : more) {
                s.push_back(std::move(stmt));
            }
        }

        // skip a single unparseable byte. this can cause nearby
        // stmts to be misaligned and garbled, but the parser
        // typically re-aligns at the next CMD boundary.
        else {
            warnings_.push_back("seg-recovery: skipped byte " +
                hex(peek()) + " at position " + std::to_string(stream_.position()) +
                " (nearby output may be garbled)");
            consume();
        }
    }

    // consume EOS (0xFF 0xFF)
    expect(0xFF);
    expect(0xFF);

    // distinguish seg/seg* based on whether conditions are empty
    bool empty_conds = (c.children.empty());

    if (empty_conds) {
        return AstNode::make_list("seg*", std::move(s));
    }

    std::vector<AstNode> children;
    children.push_back(std::move(c));

    for (auto& stmt : s) {
        children.push_back(std::move(stmt));
    }

    return AstNode::make_list("seg", std::move(children));
}

AstNode Parser::parse_mes() {
    // <mes> = (mes seg1 seg2 ...) EOM
    // uses many1: tries to parse segs until one fails, then stops
    std::vector<AstNode> segs;

    while (!at_end() && !peek_eom()) {
        size_t seg_start = stream_.position();
        auto seg_result = try_parse([this]() { return parse_seg(); });

        if (!seg_result.has_value()) {
            break;
        }

        segs.push_back(std::move(*seg_result));
    }

    // consume EOM (0xFF 0xFE)
    if (!at_end() && peek_eom()) {
        consume();
        consume();
    }

    return AstNode::make_list("mes", std::move(segs));
}

// --- utility ---

bool Parser::at_end() const {
    return stream_.at_end();
}

uint8_t Parser::peek() const {
    return stream_.peek();
}

uint8_t Parser::consume() {
    return stream_.consume();
}

void Parser::expect(uint8_t b) {
    // helper to format a byte as hex string
    auto hex = [](uint8_t v) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02X", v);
        return std::string(buf);
    };

    if (at_end()) {
        throw ParseError("unexpected end of input, expected " + hex(b), stream_.position());
    }

    uint8_t actual = consume();

    if (actual != b) {
        throw ParseError(
            "expected byte " + hex(b) + " but got " + hex(actual),
            stream_.position() - 1);
    }
}

bool Parser::peek_is_chr() const {
    if (at_end()) {
        return false;
    }

    uint8_t b = stream_.data()[stream_.position()];

    // CHR1: 0x2D-0x7F
    if (b >= 0x2D && b <= 0x7F) {
        return true;
    }

    // CHR2: 0x80-0x9F or 0xE0-0xEA (but NOT if it's a special sequence)
    if ((b >= 0x80 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEA)) {

        // check for special 2-byte sequences that overlap with CHR2
        if (b == 0x81 && peek_chr_special()) {
            return false;
        }

        return true;
    }

    // CHR2+: 0xEB-0xEF
    if (b >= 0xEB && b <= 0xEF) {
        return true;
    }

    return false;
}

bool Parser::peek_is_num() const {
    if (at_end()) {
        return false;
    }

    uint8_t b = stream_.data()[stream_.position()];
    return (b >= 0x23 && b <= 0x27) || b == 0x28 || (b >= 0x29 && b <= 0x2C);
}

bool Parser::peek_is_text() const {
    if (at_end()) {
        return false;
    }

    uint8_t b = stream_.data()[stream_.position()];

    // CHRS!: 0x21
    if (b == 0x21) {
        return true;
    }

    // CHRS: starts with CHR
    return peek_is_chr();
}

bool Parser::peek_is_op() const {
    if (at_end()) {
        return false;
    }

    uint8_t b = stream_.data()[stream_.position()];

    // reg!: 0x00-0x0F
    if (b >= 0x00 && b <= 0x0F) {
        return true;
    }

    // var!: 0x10-0x1F
    if (b >= 0x10 && b <= 0x1F) {
        return true;
    }

    // op-chr: CHR-WAIT, CHR-NOP (these start with 0x81 which is also CHR2 range)
    if (peek_chr_wait() || peek_chr_nop()) {
        return true;
    }

    // CMD: 0xA5-0xDF
    if (b >= 0xA5 && b <= 0xDF) {
        return true;
    }

    return false;
}

bool Parser::peek_is_stmt() const {
    if (at_end()) {
        return false;
    }

    // not at EOS, EOM, block-end tokens, or loop-end
    if (peek_eos() || peek_eom()) {
        return false;
    }

    uint8_t b = stream_.data()[stream_.position()];

    // block structure tokens are not stmts (BEG+ 0xA0 is, though)
    if (b == 0xA1 || b == 0xA3 || b == 0xA4) {
        return false;
    }

    // CHR-LEND is not a stmt (it's a loop terminator)
    if (peek_chr_lend()) {
        return false;
    }

    // loop: starts with CHR-LBEG
    if (peek_chr_lbeg()) {
        return true;
    }

    // op
    if (peek_is_op()) {
        return true;
    }

    // text
    if (peek_is_text()) {
        return true;
    }

    // bare block (BEG as standalone statement)
    if (b == 0xA2) {
        return true;
    }

    // standalone BEG+ ... END* expression
    if (b == 0xA0) {
        return true;
    }

    return false;
}

bool Parser::peek_is_param() const {
    if (at_end()) {
        return false;
    }

    uint8_t b = stream_.data()[stream_.position()];

    // NUM
    if (peek_is_num()) {
        return true;
    }

    // ARG
    if (b == 0x22) {
        return true;
    }

    // block (BEG)
    if (b == 0xA2) {
        return true;
    }

    return false;
}

template<typename F>
auto Parser::try_parse(F func) -> std::optional<decltype(func())> {
    size_t saved = stream_.position();

    try {
        return func();
    } catch (const ParseError&) {
        stream_.set_position(saved);
        return std::nullopt;
    }
}

} // namespace adv
