# lime-juice

A decompiler and compiler for MES bytecode, the scripting format used by visual novel engines from Elf, Alice Soft, Silky's, Fairytale, and other software houses throughout the late 80s and into the 90s. Ported from [tomyun's "juice" tool](https://github.com/tomyun/juice) written in Racket. Yes, the title is a bad pun.

Juice converts MES binary scripts into human-readable RKT (Lisp-style S-expression) source files, and compiles them back. This enables translation, modding, and analysis of games built on these engines.

Not all engine commands have been identified yet. Unresolved commands appear in decompiled scripts as `(cmd:N ...)` with their raw opcode number. If you figure out what one does, contributions are welcome.

A companion tool, **juice-img**, handles the image formats used by the same games (GP4, GPC, GPA), converting them to and from PNG/GIF.

## Supported Engines

Six engine versions were used across these games. Functionally, they fall into three distinct families:

| Family | Versions | Notes | Game list |
|--------|----------|-------|-----------|
| **AI1** | AI1, AI2 | Older engine with simpler bytecode. AI2 is functionally identical to AI1. | [AI1/AI2 games](https://vndb.org/r?q=&o=a&s=title&f=12fgAIfoAI2) |
| **AI5** | AI4, AI5 | The most common engine. AI4 is functionally identical to AI5. | [AI4/AI5 games](https://vndb.org/r?q=&o=a&s=title&f=12foAI4foAI5) |
| **ADV** | AI3, ADV | Advanced engine with segment-based structure. ADV forked from AI3; they are functionally equivalent. | [AI3/ADV games](https://vndb.org/r?f=fwADV98V-) |

**AI5WIN**, a Windows port of AI5, also exists but is not currently supported by juice. It shares most of its bytecode format with AI5 but has some Windows-specific differences.

## Quick Start

Decompile all MES scripts in a directory:

```
juice -d *.MES
```

This produces `.rkt` source files alongside the originals. Edit them as needed, then compile back:

```
juice -c *.rkt
```

For games that need specific settings, use a preset:

```
juice -d -p yuno *.MES
```

Settings from the preset are stored in the RKT file's `(meta ...)` block, so you only need to specify the preset when decompiling. Compilation reads the settings from the file automatically.

## juice CLI Reference

### Commands

| Flag | Description |
|------|-------------|
| `-d`, `--decompile` | Decompile MES bytecode into RKT source |
| `-c`, `--compile` | Compile RKT source into MES bytecode |
| `-P`, `--show-preset` | Show supported game presets |
| `-v`, `--version` | Show version |
| `-h`, `--help` | Show usage help |

### General Options

| Flag | Description |
|------|-------------|
| `-f`, `--force` | Overwrite existing output files |
| `-o`, `--output PATH` | Specify output file path. Cannot be used with multiple input files. |

### Engine Selection

| Flag | Description |
|------|-------------|
| `-e`, `--engine TYPE` | Engine type: `AI5` (default), `AI1`, `ADV`. Aliases: `AI2` maps to `AI1`, `AI4` maps to `AI5`, `AI5X` maps to `AI5` with `--dictbase D0 --extraop`. |
| `-p`, `--preset NAME` | Apply a named game preset. Overrides engine, charset, and other defaults. See `--show-preset` for the full list. |

### Character Encoding

| Flag | Description |
|------|-------------|
| `-C`, `--charset NAME` | Character set for SJIS mapping. Default: `pc98`. Options: `pc98`, `english`, `europe`, `korean-gamebox`, `korean-hannuri`, `korean-kk`, `korean-parangsae`, `chinese`. |

Charsets define the mapping between Shift-JIS byte pairs in the binary and Unicode characters in the RKT source. The default `pc98` charset covers standard Japanese SJIS plus PC-98-exclusive characters. The other charsets extend this with mappings for translation projects targeting other languages (Korean syllables, European diacritics, etc.).

### AI5 Engine Parameters

| Flag | Description |
|------|-------------|
| `-D`, `--dictbase HEX` | Dictionary base offset in hex. Default: `80`. Some later games use `D0`. |
| `-E`, `--extraop` | Enable extended opcodes used by some later AI5/ADV games. |

The dictionary base determines how many byte values are reserved for dictionary references vs. inline characters. Games like Isaku, YU-NO, Kakyuusei, Jack, and Mobius Roid use `D0` instead of the default `80`. If you see errors like "dict index >= dict size" during decompilation, try `--dictbase D0`.

The `--extraop` flag changes how certain opcodes are decoded. It's required for games like X-Girl and YU-NO that introduced slightly different instruction layouts for a few opcodes. Most later (1994+) ADV games require extraop. If decompilation produces empty or garbled output, this flag (possibly combined with `--dictbase D0`) is often the fix.

### Decompile Options

| Flag | Description |
|------|-------------|
| `--auto-engine` | Auto-detect engine type from the first input file. Prints the detected engine before processing. |
| `--no-decode` | Skip SJIS character decoding. Leaves characters as raw byte pairs instead of converting to Unicode. |
| `--no-resolve` | Skip command/function name resolution. Leaves raw opcode numbers instead of symbolic names like `wait` or `mes-jump`. |
| `--protag SPEC` | Control fusion of protagonist proc/call instructions into text nodes. See below. |

#### Protagonist Fusion (`--protag`)

Many games use a `(proc N)` or `(call VAR)` instruction between text fragments to insert the protagonist's name. By default, these appear as separate instructions:

```racket
(text "【")
(proc 0)
(text "】Hello.")
```

With `--protag 0`, the decompiler fuses them into the text:

```racket
(text "【" 0 "】Hello.")
```

Values for `--protag`:
- `none` (default): no fusion
- `all`: fuse all proc/call instructions found between text
- A number (e.g. `0`, `3`, `51`): fuse only the specified proc ID
- A letter (e.g. `Z`): fuse only the specified call variable
- A comma-separated list (e.g. `0,Z` or `0,3`): fuse multiple

### Compile Options

| Flag | Description |
|------|-------------|
| `--auto-wrap` | Auto-wrap text nodes to fit the text-frame width defined in the script. Useful when translated text is longer than the original. |
| `--no-compress` | Skip AI5 dictionary compression. Produces larger output files. |

#### Auto-Wrap

When `--auto-wrap` is enabled, the compiler scans the script for `(text-frame X1 Y1 X2 Y2)` commands to determine the display width, then automatically inserts line breaks in `(text "...")` strings that would overflow. This is especially useful for translation projects to Western languages where the translated script is longer than the Japanese.

### Game Presets

Presets bundle the correct engine, dictionary base, extraop, and protagonist settings for specific games. Use `-P` or `--show-preset` to list them. You only need the preset when decompiling; the settings are saved in the RKT file's metadata.

<details>
<summary>Full preset list</summary>

| Preset | Game | Engine | Special Settings |
|--------|------|--------|------------------|
| `aishi` | Ai Shimai | AI5 | defaults |
| `angel` | Angel Hearts | AI1 | |
| `coc` | Curse of Castle | AI5 | defaults |
| `cre` | Crescent | AI5 | defaults |
| `deja` | De-Ja | AI1 | protag: variable Y |
| `deja2` | De-Ja 2 | AI5 | defaults |
| `dk` | Dragon Knight | AI1 | |
| `dk2` | Dragon Knight 2 | AI1 | protag: variable G |
| `dk3` | Dragon Knight 3 | AI5 | protag: proc 26 |
| `dk4` | Dragon Knight 4 | AI5 | defaults |
| `elle` | ELLE | AI5 | protag: variable Z |
| `foxy` | Foxy | AI5 | defaults |
| `foxy2` | Foxy 2 | AI5 | defaults |
| `isaku` | Isaku | AI5 | dictbase D0, extraop |
| `jack` | Jack | AI5 | dictbase D0 |
| `jan` | Jan Jaka Jan | AI5 | protag: proc 51 |
| `kakyu` | Kakyuusei | AI5 | dictbase D0, protag: proc 3 |
| `kawa` | Kawarazaki-ke no Ichizoku | AI5 | defaults |
| `metal` | Metal Eye | AI5 | defaults |
| `metal2` | Metal Eye 2 | AI5 | defaults |
| `mobius` | Mobius Roid | AI5 | dictbase D0 |
| `nanpa` | Doukyuusei | AI5 | protag: proc 0 |
| `nanpa2` | Doukyuusei 2 | AI5 | protag: proc 0 |
| `nono` | Nonomura Byouin no Hitobito | AI5 | defaults |
| `pinky` | Pinky Ponky 1/2/3 | AI1 | protag: variable N |
| `pre` | Premium | AI5 | defaults |
| `pre2` | Premium 2 | AI5 | defaults |
| `raygun` | RAY-GUN | AI1 | |
| `reira` | Reira Slave Doll | AI5 | defaults |
| `shima` | Ushinawareta Rakuen | AI5 | defaults |
| `syan` | Shangrlia | AI5 | defaults |
| `syan2` | Shangrlia 2 | AI5 | defaults |
| `ten` | Tenshin Ranma | AI5 | defaults |
| `ww` | Words Worth | AI5 | protag: proc 0 |
| `xgirl` | X-Girl | ADV | extraop |
| `yuno` | YU-NO | AI5 | dictbase D0, extraop, protag: proc 0 |

</details>

### RKT Metadata

When compiling, juice reads settings from the `(meta ...)` block at the top of the RKT file. These override CLI defaults, so you generally don't need to repeat flags when compiling:

```racket
(meta (engine 'AI5) (charset "pc98") (extraop #t))
```

### Input/Output Handling

- Accepts multiple input files. Glob patterns (`*`, `?`) are expanded automatically.
- Output filenames are generated by swapping the extension: `.mes` becomes `.rkt` (decompile), `.rkt` becomes `.mes` (compile).
- The `-o` flag overrides the output path but can only be used with a single input file.
- Both tools exit `0` on success and `1` for invalid usage or any failed input; batches continue after failures.

---

## juice-img CLI Reference

### Commands

| Flag | Description |
|------|-------------|
| `-d`, `--decode` | Decode image files to PNG (GP4, GPC), GIF (GPA), or a folder of PNGs (CSR) |
| `-c`, `--compile` | Compile PNG to GP4/GPC, GIF to GPA, or a decoded CSR folder back to CSR |
| `-v`, `--version` | Show version |
| `-h`, `--help` | Show usage help |

### Options

| Flag | Description |
|------|-------------|
| `-f`, `--force` | Overwrite existing output files |
| `-o`, `--output PATH` | Specify output file (or CSR folder) path. Single input file only. |
| `-F`, `--format FMT` | Target format when compiling PNG: `gp4` or `gpc`. Default: `gpc`. Ignored if the output file extension already specifies a format. |
| `-p`, `--palette PATH` | Palette source (a GPC file) for GPA and CSR decode. If not specified, juice-img searches the parent directory for a matching GPC file. |
| `-W`, `--force-width N` | Override canvas width for GP4 decode. Default: 640. Only applies to GP4 files. |

### Supported Formats

| Format | Input | Output | Notes |
|--------|-------|--------|-------|
| **GP4** | `.gp4` | `.png` | 4-pixel column format with move-to-front + RLE compression. Width must be a multiple of 4; non-conforming images are truncated with a warning during encode. |
| **GPC** | `.gpc` | `.png` | Indexed color with palette, XOR + interlacing compression. |
| **GPA** | `.gpa` | `.gif` | Multi-frame animation. Requires a palette (embedded or from a separate GPC file). |
| **CSR** | `.csr`, `.dat` | `.png` folder | Mouse cursors and icons. Requires a palette from a separate GPC file. |

#### GPA Notes

- When decoding GPA files, if the animation has no embedded palette, juice-img searches for a GPC file with the same name in the parent directory tree. Use `-p` to specify one manually.
- If no palette is found at all, the resulting GIF will come out entirely black. If your decoded GPA looks all black, this is why.
- The last frame of a decoded GIF may contain metadata (original negative offsets) used for lossless re-encoding. Do not edit this frame if you plan to re-encode the GPA.

#### CSR Notes

- Decoding `TJ.CSR` creates a folder `TJ/` with one PNG per cursor plus a `csr.txt` manifest. Compiling the folder (`juice-img -c TJ`) will rebuild `TJ.CSR`.
- Hotspots can be changed in `csr.txt`. Don't change the entry or plane numbers.
- Cursors can be resized, but the width must be a multiple of 8. Cursors with 1 or 2 planes can only use colors 0-1 or 0-3.
- CSR files have no palette of their own. juice-img searches for a GPC file with the same name in the parent directory tree, which will usually find nothing, so use `-p` to pick one of the game's GPC files. Without one, the PNGs will come out grayscale.
- AI5 `mouse.dat` files are a different format and aren't supported.

---

## Building from Source

### Requirements

- CMake 3.16 or higher
- A C++17 compiler (Clang, GCC, or MSVC)
- iconv (optional, provides fallback for unmapped characters during SJIS conversion)
- Willingness to suffer and fight with `cmake` flags for hours on end.

### Build Steps

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

Assuming you have your system set up perfectly like mine, this produces two executables: `juice` and `juice-img`.
If you don't, well - good luck.

### Platform Notes

- **Windows (MinGW):** The build statically links the C++ runtime so the executables can run standalone. If system iconv is not available, a vendored win-iconv is used automatically.
- **Linux/macOS:** iconv is typically provided by the system's libc. If CMake cannot find it, the build proceeds without it, but some uncommon character mappings may not be available.

---

## Troubleshooting

### Empty or garbled decompiler output

The most common cause is using the wrong engine settings. Try these in order:

1. **Use `--auto-engine`** to let juice detect the engine type from the file.
2. **Try a preset** if one exists for your game (`-P` lists them all).
3. **Try `--extraop`** if you're working with a later AI5 or ADV game.
4. **Try `--dictbase D0`** if you see dictionary index errors.

### "output file already exists"

Use `-f` to force overwrite.

### Cannot recompile existing .rkt files from a different version of Juice

lime-juice formats its RKT script slightly differently, which may result in being unable to compile RKT files that were created by tomyun or dantecsm's juice.exe.
Try re-decompiling the .MES from the original file, fixing any non-text differences, and then compiling your RKT script file again.

### Wrong characters in output

Make sure you're using the right charset (`-C`). The default `pc98` is correct for most Japanese games, but English translations and other localizations need their specific charset.

### GPA decoding shows wrong colors or all black

The GPA file may not have an embedded palette. Either provide one with `-p palette.gpc` or place the matching GPC file in a sibling directory where juice-img can find it.

---

## Further Documentation

- [MES Binary Format Specification](doc/mes-format.md)
- [AI5 Scripting Reference](doc/scripting-ai5.md)
- [AI1 Scripting Reference](doc/scripting-ai1.md)
- [ADV Scripting Reference](doc/scripting-adv.md)
- [GP4 Image Format](doc/gp4-ada.md)
- [GPC/GPA Image Format](doc/gpc-gpa.md)

## License

```
lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
Copyright (C) 2026 Fuzion

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
```

---

Most of this project's files are licensed under the GNU Public License, version 3 or later (SPDX: `GPL-3.0-or-later`). For the full text of this license, see the included [`/LICENSE.md`](LICENSE.md).

Exempt from this license are:

- all files in `/docs/` (not code)
- the external projects included in `/third_party/`
  - `iconv` ([win-iconv](https://github.com/win-iconv/win-iconv)): Public Domain
  - `lecram` ([gifenc](https://github.com/lecram/gifenc) & [gifdec](https://github.com/lecram/gifdec)): Public Domain
  - `lodepng` ([LodePNG](https://github.com/lvandeve/lodepng)): Zlib
