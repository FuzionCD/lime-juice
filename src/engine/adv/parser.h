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

#include "../../ast.h"
#include "../../byte_stream.h"
#include "../../config.h"
#include "../engine.h"

namespace adv {

// recursive descent parser for ADV MES bytecode
// translated from engine/adv/mes-parser.rkt
class Parser {
public:
    Parser(ByteStream& stream, const Config& cfg);

    // parse the entire MES bytecode stream
    // returns (mes seg1 seg2 ...)
    AstNode parse_mes();

    // warnings accumulated during parsing (e.g. segment recovery)
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    std::vector<std::string> warnings_;
    ByteStream& stream_;
    const Config& cfg_;

    // --- lexer helpers ---

    // decode register bitfield from 2 bytes → (reg* n '= f)
    static AstNode lex_reg(uint8_t c1, uint8_t c2);

    // decode variable bitfield from 2 bytes → (var* (var C) 'op val)
    static AstNode lex_var(uint8_t c1, uint8_t c2);

    // decode variable bitfield from 3 bytes (extraop) → (var* (var C) 'op val)
    static AstNode lex_var2(uint8_t c1, uint8_t c2, uint8_t c3);

    // convert integer index to variable node: 0-25 → A-Z, 26 → a
    static AstNode integer_to_var(int i);

    // decode number tokens
    static AstNode lex_num0(uint8_t c);
    static AstNode lex_num1(uint8_t c1, uint8_t c2);
    static AstNode lex_num2(uint8_t c1, uint8_t c2, uint8_t c3);

    // decode character tokens
    static AstNode lex_chr1(uint8_t c);
    static AstNode lex_chr2(uint8_t c1, uint8_t c2);

    // --- token parsers ---

    // REG*: 0x00-0x0F + any byte → register bitfield
    AstNode parse_reg_star();

    // VAR*: 0x10-0x1F + bytes → variable bitfield (2 or 3 bytes depending on extraop)
    AstNode parse_var_star();

    // NUM: number token (NUM0 | NUM1 | NUM2)
    AstNode parse_num();

    // ARG: 0x22 + chars + 0x22 → (arg chr...)
    AstNode parse_arg();

    // CHRS!: 0x21 + chars + (0x00 | 0xFF) → (chrs! chr...)
    AstNode parse_chrs_escaped();

    // CHR: single character token (CHR1 | CHR2 | CHR2+)
    // respects the notFollowedBy CHR$ rule for 0x81 range
    AstNode parse_chr();

    // CHRS: (chrs chr1 chr2 ...) = many1 CHR
    AstNode parse_chrs();

    // CMD: 0xA5-0xDF → (cmd byte)
    AstNode parse_cmd();

    // --- special 2-byte sequences ---

    bool peek_chr_lbeg() const;
    bool peek_chr_lend() const;
    bool peek_chr_wait() const;
    bool peek_chr_nop() const;
    bool peek_chr_special() const;
    bool peek_eos() const;
    bool peek_eom() const;

    void consume_chr_lbeg();
    void consume_chr_lend();

    // --- grammar rules ---

    // reg!: register operation (in statements)
    AstNode parse_reg_bang();

    // var!: variable operation (in statements)
    AstNode parse_var_bang();

    // reg?: register condition (in segment conditions)
    AstNode parse_reg_question();

    // var?: variable condition (in segment conditions)
    AstNode parse_var_question();

    // conds: (? reg?... var?...)
    AstNode parse_conds();

    // param: NUM | ARG | block | block* | block/
    AstNode parse_param();

    // params: (params param...)
    AstNode parse_params();

    // text: CHRS! | CHRS
    AstNode parse_text();

    // block: (<> stmts)
    AstNode parse_block();

    // block*: (<*> items)
    AstNode parse_block_star();

    // block/: (</> branches)
    AstNode parse_block_slash();

    // block/?: (</> branches?)
    AstNode parse_block_slash_q();

    // loop: (loop stmts)
    AstNode parse_loop();

    // --- special operations ---

    AstNode parse_op_chr();
    AstNode parse_op_menu();
    AstNode parse_op_if_when();
    AstNode parse_op_branch_var();
    AstNode parse_op_execute_var();
    AstNode parse_print_var();
    AstNode parse_op_sound();
    AstNode parse_op_decrypt();

    // op-cmd*: special command operations
    AstNode parse_op_cmd_star();

    // op-cmd: CMD params (generic fallback)
    AstNode parse_op_cmd();

    // op: any operation
    AstNode parse_op();

    // stmt: loop | op | text
    AstNode parse_stmt();

    // stmts: many stmt
    std::vector<AstNode> parse_stmts();

    // seg: conds stmts [END] EOS
    AstNode parse_seg();

    // --- block sub-productions ---

    // item+: conds stmts
    AstNode parse_item_plus();

    // items+: sepBy(item+, CNT)
    std::vector<AstNode> parse_items_plus();

    // item: conds stmts BEG+ items+ END*
    AstNode parse_item();

    // branch: stmts → (/ stmts...)
    AstNode parse_branch();

    // branch?: conds stmts → (// conds stmts...)
    AstNode parse_branch_q();

    // --- utility ---

    bool at_end() const;
    uint8_t peek() const;
    uint8_t consume();
    void expect(uint8_t b);

    // check if next byte could start a CHR token
    bool peek_is_chr() const;

    // check if next byte could start a NUM token
    bool peek_is_num() const;

    // check if next byte could start text (CHRS! or CHRS)
    bool peek_is_text() const;

    // check if next byte could start an op
    bool peek_is_op() const;

    // check if next byte could start a stmt
    bool peek_is_stmt() const;

    // check if next byte could start a param
    bool peek_is_param() const;

    // block-end: END | lookAhead(EOS) | lookAhead(CHR-LEND)
    bool peek_is_block_end() const;

    // loop-end: CHR-LEND | lookAhead(EOS)
    bool peek_is_loop_end() const;

    // consume block-end (END byte if present, otherwise implicit)
    void consume_block_end();

    // consume loop-end (CHR-LEND if present, otherwise implicit at EOS)
    void consume_loop_end();

    // try to parse; on failure, restore position and return nullopt
    template<typename F>
    auto try_parse(F func) -> std::optional<decltype(func())>;
};

} // namespace adv
