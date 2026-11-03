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

#include "auto_detect.h"
#include "auto_wrap.h"
#include "cli_utils.h"
#include "config.h"
#include "sexp_writer.h"
#include "sexp_reader.h"
#include "engine/ai5/loader.h"
#include "engine/ai1/loader.h"
#include "engine/adv/loader.h"
#include "engine/ai1/compiler.h"
#include "engine/ai5/compiler.h"
#include "engine/adv/compiler.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* TITLE = "juice";
static const char* VERSION = "v0.2.3 (lime-juice)";

// thresholds for detecting suspiciously small decompiler output
static const size_t SUSPECT_OUTPUT_MAX = 256;
static const size_t SUSPECT_INPUT_MIN  = 64;

enum class Command {
    None,
    Decompile,
    Compile,
    ShowPreset,
    ShowVersion,
};

static void print_usage() {
    std::cerr << "usage: " << TITLE << " <command> [options] <files...>" << std::endl;
    std::cerr << std::endl;
    std::cerr << "commands:" << std::endl;
    std::cerr << "  -d, --decompile     decompile MES bytecode into rkt source" << std::endl;
    std::cerr << "  -c, --compile       compile rkt source into MES bytecode" << std::endl;
    std::cerr << "  -P, --show-preset   show supported presets" << std::endl;
    std::cerr << "  -v, --version       show version" << std::endl;
    std::cerr << std::endl;
    std::cerr << "options:" << std::endl;
    std::cerr << "  -f, --force         force overwrite output files" << std::endl;
    std::cerr << "  -o, --output PATH   output file path (default: input with swapped extension)" << std::endl;
    std::cerr << "  -p, --preset NAME   preset for a specific game; see --show-preset" << std::endl;
    std::cerr << "  -e, --engine TYPE   engine type (AI5*, AI1, ADV)" << std::endl;
    std::cerr << "  -C, --charset NAME  charset encoding (pc98*, english, europe, korean-..)" << std::endl;
    std::cerr << "  -D, --dictbase HEX  [AI5] dictionary base (80*, D0)" << std::endl;
    std::cerr << "  -E, --extraop       [AI5/ADV] support newer opcodes" << std::endl;
    std::cerr << "  --protag SPEC       {decompile} proc/call(s) fused in text" << std::endl;
    std::cerr << "  --no-decode         {decompile} skip SJIS character decoding" << std::endl;
    std::cerr << "  --no-resolve        {decompile} skip cmd/sys name resolution" << std::endl;
    std::cerr << "  --no-compress       {compile} skip AI5 dictionary compression" << std::endl;
    std::cerr << "  --auto-engine       {decompile} auto-detect engine type from file" << std::endl;
    std::cerr << "  --auto-wrap         {compile} auto-wrap text to fit text-frame width" << std::endl;
}

static void show_presets() {
    auto presets = get_presets();

    for (const auto& p : presets) {
        // pad key to 6 chars
        std::string padded = p.key;

        while (padded.size() < 6) {
            padded += ' ';
        }

        print_color("b-white", padded);
        std::cout << " : " << p.title << std::endl;
    }
}

static bool decompile_file(const std::string& path, Config& cfg, bool force,
                           const std::string& output_override = "") {
    // output: use override or replace .mes with .rkt
    std::string outname;

    // non .mes-extension files are valid if for some reason the original dev renamed them (does this exist?)
    if (!output_override.empty()) {
        outname = output_override;
    } else if(has_extension(path, ".mes")) {
        outname = path.substr(0, path.size() - 4) + ".rkt";
    } else {
        outname = path + ".rkt";
    }

    // display the output filename stem, status suffix follows
    std::string out_stem = fs::path(outname).stem().string();
    print_color("b-white", out_stem);
    std::cout.flush();

    try {
        // check if output exists
        if (!force && fs::exists(outname)) {
            println_color("b-red", " ! output file already exists (use -f to overwrite)");
            return false;
        }

        AstNode ast = AstNode::make_list("mes", {});
        std::vector<std::string> parse_warnings;

        if (cfg.engine == EngineType::ADV) {
            bool should_retry = false;
            std::exception_ptr first_error;
            size_t input_bytes = fs::file_size(path);

            try {
                auto result = adv::load_mes(path, cfg);
                ast = std::move(result.ast);
                parse_warnings = std::move(result.warnings);

                // check if the first attempt looks wrong
                size_t first_seg_count = 0;

                for (const auto& child : ast.children) {

                    if (child.tag != "meta") {
                        first_seg_count++;
                    }
                }

                if (first_seg_count == 0) {
                    should_retry = true;
                }

                // also retry if output is suspiciously small relative
                // to input, which happens when D5 decrypt consumes
                // valid segment data
                if (!should_retry && input_bytes > SUSPECT_INPUT_MIN) {
                    SexpWriter probe_writer;
                    std::string probe_output = probe_writer.format(ast);

                    if (probe_output.size() < SUSPECT_OUTPUT_MAX) {
                        should_retry = true;
                    }
                }
            } catch (const std::exception&) {
                first_error = std::current_exception();
                should_retry = true;
            }

            // fallback: if ADV with extraop produced bad results or
            // threw an exception, the D5 decrypt opcode may be
            // consuming valid data. retry with decrypt disabled (some
            // games use 3-byte VAR encoding without the D5 decrypt
            // opcode).
            if (should_retry && cfg.extra_op && cfg.decrypt_op) {
                Config retry_cfg = cfg;
                retry_cfg.decrypt_op = false;
                auto retry = adv::load_mes(path, retry_cfg);
                size_t retry_segs = 0;

                for (const auto& child : retry.ast.children) {

                    if (child.tag != "meta") {
                        retry_segs++;
                    }
                }

                if (retry_segs > 0) {
                    first_error = nullptr;
                    ast = std::move(retry.ast);
                    parse_warnings = std::move(retry.warnings);
                }
            }
            if (first_error) {
                std::rethrow_exception(first_error);
            }
        } else if (cfg.engine == EngineType::AI1) {
            ast = ai1::load_mes(path, cfg);
        } else {
            ast = ai5::load_mes(path, cfg);
        }

        // check for suspiciously empty output, the mes node should
        // contain segments. if it only has the meta wrapper and nothing
        // else, the parser likely failed silently.
        size_t input_size = fs::file_size(path);
        size_t segment_count = 0;

        for (const auto& child : ast.children) {

            if (child.tag != "meta") {
                segment_count++;
            }
        }

        SexpWriter writer;
        std::string output = writer.format(ast);

        std::ofstream out(outname);

        if (!out.is_open()) {
            throw std::runtime_error("cannot write to: " + outname);
        }

        out << output;
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write to: " + outname);
        }

        if (segment_count == 0 && input_size > 4) {
            println_color("b-yellow", ".rkt (warning: no segments parsed from " +
                std::to_string(input_size) + " byte input - wrong --engine or missing --extraop?)");
        } else if (output.size() < SUSPECT_OUTPUT_MAX && input_size > SUSPECT_INPUT_MIN) {
            println_color("b-yellow", ".rkt (warning: output is suspiciously small. only " +
                std::to_string(output.size()) + " bytes from " +
                std::to_string(input_size) + " byte input)");
        // arbitrary threshold where it becomes an error
        } else if (parse_warnings.size() > 10) {
            println_color("b-yellow", ".rkt (warning: \033[31m" +
                std::to_string(parse_warnings.size()) +
                " recovery skip(s) during parse, likely wrong --engine or --extraop setting\033[93m)");
        } else if (!parse_warnings.empty()) {
            println_color("b-yellow", ".rkt (warning: " +
                std::to_string(parse_warnings.size()) + " recovery skip(s) during parse)");
        } else {
            println_color("b-green", ".rkt");
        }

        // print recovery warnings after the status line (to stderr so
        // they don't interfere with machine-parseable status output)
        for (const auto& w : parse_warnings) {
            std::cerr << "\033[93m  " << w << "\033[0m" << std::endl;
        }
        return true;
    } catch (const std::exception& e) {
        println_color("b-red", "!");
        std::cerr << "  " << e.what() << std::endl;
        return false;
    }
}

// extract engine/charset/extraop from the (meta ...) node in an RKT AST
static void apply_meta_config(const AstNode& ast, Config& cfg) {

    for (const auto& child : ast.children) {

        if (!child.is_list("meta")) {
            continue;
        }

        for (const auto& m : child.children) {

            // (engine 'ADV)
            if (m.is_list("engine") && !m.children.empty() &&
                m.children[0].is_quote() && !m.children[0].children.empty() &&
                m.children[0].children[0].is_symbol()) {
                cfg.set_engine(m.children[0].children[0].str_val);
            }

            // (charset "pc98")
            if (m.is_list("charset") && !m.children.empty() &&
                m.children[0].is_string()) {
                cfg.charset_name = m.children[0].str_val;
            }

            // (extraop #t)
            if (m.is_list("extraop") && !m.children.empty() &&
                m.children[0].is_boolean() && m.children[0].bool_val) {
                cfg.extra_op = true;
            }
        }

        break;
    }
}

static bool compile_file(const std::string& path, Config& cfg, bool force,
                         bool wrap, const std::string& output_override = "") {

    if (!has_extension(path, ".rkt")) {
        print_color("b-white", fs::path(path).filename().string());
        println_color("b-red", " ! expected an .rkt input file");
        return false;
    }

    // output: use override or replace .rkt with .mes
    std::string outname = output_override.empty()
        ? path.substr(0, path.size() - 4) + ".mes"
        : output_override;

    std::string out_stem = fs::path(outname).stem().string();
    print_color("b-white", out_stem);
    std::cout.flush();

    try {
        if (!force && fs::exists(outname)) {
            println_color("b-red", " ! output file already exists (use -f to overwrite)");
            return false;
        }

        // read rkt source
        std::ifstream f(path);

        if (!f.is_open()) {
            throw std::runtime_error("cannot open: " + path);
        }

        std::ostringstream ss;
        ss << f.rdbuf();
        std::string rkt_text = ss.str();

        // parse s-expression
        SexpReader reader;
        AstNode ast = reader.parse(rkt_text);

        // apply meta settings (engine, charset, extraop) from the rkt file
        // cli flags override these if explicitly set before the file args
        Config compile_cfg = cfg;
        apply_meta_config(ast, compile_cfg);

        // auto-wrap text to fit text-frame widths
        if (wrap) {
            auto_wrap_ast(ast);
        }

        // compile based on engine type
        std::vector<uint8_t> compiled;

        if (compile_cfg.engine == EngineType::ADV) {
            compiled = adv::compile_mes(ast, compile_cfg);
        } else if (compile_cfg.engine == EngineType::AI1) {
            compiled = ai1::compile_mes(ast, compile_cfg);
        } else if (compile_cfg.engine == EngineType::AI5) {
            compiled = ai5::compile_mes(ast, compile_cfg);
        } else {
            throw std::runtime_error("compiler not implemented for this engine");
        }

        // write output
        std::ofstream out(outname, std::ios::binary);

        if (!out.is_open()) {
            throw std::runtime_error("cannot write to: " + outname);
        }

        out.write(reinterpret_cast<const char*>(compiled.data()), compiled.size());
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write to: " + outname);
        }

        println_color("b-green", ".mes");
        return true;
    } catch (const std::exception& e) {
        println_color("b-red", "!");
        std::cerr << "  " << e.what() << std::endl;
        return false;
    }
}

static int run_cli(int argc, char* argv[]) {
    enable_ansi_console();

    Command command = Command::None;
    Config cfg;
    bool force = false;
    bool auto_engine = false;
    bool auto_wrap = false;
    bool explicit_engine = false;
    std::string output_path;
    std::vector<std::string> file_args;

    // parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-d" || arg == "--decompile") {
            command = Command::Decompile;
        } else if (arg == "-c" || arg == "--compile") {
            command = Command::Compile;
        } else if (arg == "-P" || arg == "--show-preset") {
            command = Command::ShowPreset;
        } else if (arg == "-v" || arg == "--version") {
            command = Command::ShowVersion;
        } else if (arg == "-f" || arg == "--force") {
            force = true;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            i++;
            output_path = argv[i];
        } else if ((arg == "-p" || arg == "--preset") && i + 1 < argc) {
            i++;
            cfg.use_preset(argv[i]);
        } else if ((arg == "-e" || arg == "--engine") && i + 1 < argc) {
            i++;
            cfg.set_engine(argv[i]);
            explicit_engine = true;
        } else if ((arg == "-C" || arg == "--charset") && i + 1 < argc) {
            i++;
            cfg.charset_name = argv[i];
        } else if ((arg == "-D" || arg == "--dictbase") && i + 1 < argc) {
            i++;
            cfg.dict_base = static_cast<uint8_t>(std::stoul(argv[i], nullptr, 16));
        } else if (arg == "-E" || arg == "--extraop") {
            cfg.extra_op = true;
        } else if (arg == "--no-decode") {
            cfg.decode = false;
        } else if (arg == "--no-resolve") {
            cfg.resolve = false;
        } else if (arg == "--no-compress") {
            cfg.compress = false;
        } else if (arg == "--auto-engine") {
            auto_engine = true;
        } else if (arg == "--auto-wrap") {
            auto_wrap = true;
        } else if (arg == "--protag" && i + 1 < argc) {
            i++;
            cfg.set_protag(argv[i]);
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (arg[0] == '-') {
            std::cerr << "unknown option: " << arg << std::endl;
            print_usage();
            return 1;
        } else {
            file_args.push_back(arg);
        }
    }

    switch (command) {
        case Command::ShowVersion:
            std::cout << TITLE << " " << VERSION << std::endl;
            return 0;

        case Command::ShowPreset:
            show_presets();
            return 0;

        case Command::Decompile: {

            if (auto_wrap) {
                std::cerr << "--auto-wrap is only valid with --compile" << std::endl;
                return 1;
            }

            auto paths = expand_globs(file_args);

            if (paths.empty()) {
                std::cerr << "no input files" << std::endl;
                return 1;
            }

            if (!output_path.empty() && paths.size() > 1) {
                std::cerr << "-o cannot be used with multiple input files" << std::endl;
                return 1;
            }

            // auto-detect engine from first file
            if (auto_engine && !explicit_engine) {
                std::ifstream probe(paths[0], std::ios::binary);

                if (!probe.is_open()) {
                    std::cerr << "cannot open: " << paths[0] << std::endl;
                    return 1;
                }

                std::vector<uint8_t> probe_bytes(
                    (std::istreambuf_iterator<char>(probe)),
                    std::istreambuf_iterator<char>());

                cfg.engine = detect_engine(probe_bytes);
                std::string engine_name;

                if (cfg.engine == EngineType::ADV) { engine_name = "ADV"; }
                else if (cfg.engine == EngineType::AI1) { engine_name = "AI1"; }
                else { engine_name = "AI5"; }

                print_color("b-cyan", "auto-engine: " + engine_name);
                std::cout << std::endl;
            }

            bool success = true;
            for (const auto& path : paths) {
                if (!decompile_file(path, cfg, force, output_path)) {
                    success = false;
                }
            }

            return success ? 0 : 1;
        }

        case Command::Compile: {

            if (auto_engine) {
                std::cerr << "--auto-engine is only valid with --decompile" << std::endl;
                return 1;
            }

            auto paths = expand_globs(file_args);

            if (paths.empty()) {
                std::cerr << "no input files" << std::endl;
                return 1;
            }

            if (!output_path.empty() && paths.size() > 1) {
                std::cerr << "-o cannot be used with multiple input files" << std::endl;
                return 1;
            }

            bool success = true;
            for (const auto& path : paths) {
                if (!compile_file(path, cfg, force, auto_wrap, output_path)) {
                    success = false;
                }
            }

            return success ? 0 : 1;
        }

        case Command::None:
        default:
            print_usage();
            return 1;
    }
}

int main(int argc, char* argv[]) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
