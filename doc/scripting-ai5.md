# AI5 Engine Scripting Reference

This is a reference for the RKT script format used by AI5 engine games (also AI4, which is functionally identical). RKT files are Lisp-style S-expression source that juice decompiles from, and compiles back to, MES bytecode.

## File Structure

Every RKT file begins with a top-level `(mes ...)` form containing metadata, an optional dictionary, and the script body:

```racket
(mes
 (meta (engine 'AI5) (charset "pc98") (dictbase 128))
 (dict #\る #\装 #\備 #\を #\す)
 ;; script body follows
 (slot 3 (<> ...))
 (while (== 1 1) (<> ...)))
```

**Metadata** is read by the compiler to determine engine settings. You generally don't need to edit it.

**Dictionary** lists commonly-used characters that get compressed to single bytes in the compiled output. You can replace it with `(dict-build)` to have the compiler regenerate it automatically, which is useful after translation.

## Syntax Basics

RKT uses Racket/Lisp syntax. A quick primer:

- **Lists**: `(command arg1 arg2 ...)` calls a command with arguments.
- **Comments**: Everything after `;` on a line is ignored.
- **Strings**: `"text goes here"`.
- **Characters**: `#\A`, `#\る`, etc.
- **Numbers**: Plain integers like `0`, `42`, `65535`.
- **Booleans**: `#t` (true), `#f` (false).
- **Keywords**: `#:color`, `#:wrap`, etc. Used for optional named parameters.
- **Quoted values**: `'value` is a literal value (not evaluated).

## Variables

AI5 scripts have 27 single-letter variables: `@` and `A` through `Z` (max variable index: 25).

- `@` is the **system variable array**, used for engine-level settings like font width and display mode.
- `A`-`Z` are general-purpose variables, though some have conventional uses (e.g. `S` often stores menu selection results, `N` often stores counts or states).
- `P` and `I` are sometimes used for price and item index in shop/inventory systems.

### Setting Variables

```racket
(set-var S 0)
```

Equivalent to `S = 0`.

## Arrays and Registers

Variables can also be used as arrays (or "register files") that store multiple values at indexed positions.

### Writing to Arrays

```racket
(set-arr~ @ 21 (+ 512 16))    ; @[21] = 512 + 16
(set-arr~ M 15 (- S 1))       ; M[15] = S - 1
(set-arr~b A 69 0)            ; A[69] = 0  (byte-width variant)
```

`set-arr~` writes a word (16-bit) value. `set-arr~b` writes a byte (8-bit) value.
Why does it end in a tilde? I don't know! Ask tomyun!

### Reading from Arrays

```racket
(~ @ 20)                      ; read @[20]
(~ A 0)                       ; read A[0]
(~b @ 5)                      ; read @[5] as byte
```

### System Array (`@`)

The `@` array stores engine parameters. Common indices:

| Index | Purpose |
|-------|---------|
| `@ 1` | Display buffer settings |
| `@ 2` | Memory/resource address |
| `@ 6` | Display layer control |
| `@ 7` | Processing flags |
| `@ 9` | Input/system flags |
| `@ 13` | Text frame coordinates (set with 4 values: x1, y1, x2, y2) |
| `@ 20` | Display mode bitfield |
| `@ 21` | Font dimensions (width in upper byte, height in lower byte) |

Font width is packed into `@ 21` as `(width_chars << 8) + height_pixels`. A value of `(+ 512 16)` means width=2 (double-width), height=16. For half-width text (useful in translations), use `(+ 256 16)`:

```racket
;; double-width (default Japanese)
(set-arr~ @ 21 (+ 512 16))

;; single-width (for Western text)
(set-arr~ @ 21 (+ 256 16))
```

Setting the text frame (the area where text is displayed):

```racket
(set-arr~ @ 13 15 322 64 391)   ; text frame at (15, 322) to (64, 391)
```

## Expressions and Operators

Expressions appear inside conditionals, assignments, and anywhere a value is needed.

### Arithmetic

| Operator | Meaning |
|----------|---------|
| `+` | Addition |
| `-` | Subtraction |
| `*` | Multiplication |
| `/` | Division |
| `%` | Modulo |

### Comparison

| Operator | Meaning |
|----------|---------|
| `==` | Equal |
| `!=` | Not equal |
| `>` | Greater than |
| `<` | Less than |

### Logic and Bitwise

| Operator | Meaning |
|----------|---------|
| `&&` | Logical/bitwise AND |
| `//` | Logical/bitwise OR |

### Special

| Operator | Meaning |
|----------|---------|
| `~` | Array read: `(~ VAR INDEX)` |
| `~b` | Array read (byte): `(~b VAR INDEX)` |
| `:` | Register/field read: `(: INDEX)` |
| `::` | Extended field read |
| `?` | Conditional test |

### Examples

```racket
(+ (&& (~ @ 20) 65280) 0)     ; (@[20] AND 0xFF00) + 0
(// (&& (~ @ 20) 65399) 128)   ; (@[20] AND 0xFF97) OR 128
(- S 1)                        ; S - 1
(== N 2)                       ; N == 2
```

## Control Flow

### if / if-else

```racket
(if (== S 0) (<> (break)))
```

Equivalent to:
```
if (S == 0) {
    break
}
```

```racket
(if-else (== N 2)
         (<> (text "yes"))
         (<> (text "no")))
```

Equivalent to:
```
if (N == 2) {
    print("yes")
} else {
    print("no")
}
```

### cond (multi-branch)

```racket
(cond ((< (: 106) 6)
       (<> (text "Low affection")))
      ((> (: 106) 5)
       (<> (text "High affection")))
      (else (<>)))
```

Equivalent to:
```
if (reg[106] < 6) {
    print("Low affection")
} else if (reg[106] > 5) {
    print("High affection")
}
```

### while / break / continue

```racket
(while (== 1 1)
       (<>
        ;; loop body
        (cond ((== S 0) (<> (break)))
              ((== S 1) (<> ... (continue))))))
```

Equivalent to:
```
while (true) {
    if (S == 0) break
    if (S == 1) { ...; continue }
}
```

### slot

`slot` switches execution to a different script segment. It takes a slot number and either a body block or a variant identifier:

```racket
(slot 3
      (<>
       (set-arr~ @ 20 ...)
       (color 7)
       (clear)))

(slot 1 2)    ; jump to slot 1, variant 2
(slot 1 5)    ; jump to slot 1, variant 5
```

This is different from `define-proc`/`proc`: `slot` switches the engine's execution context to a different segment (like changing scenes), while `proc` calls a locally-defined subroutine and returns.

## Blocks

`(<> ...)` is a sequential block that groups multiple statements:

```racket
(<> (set-var S 0)
    (text "Hello")
    (wait))
```

`(<*> ...)` is a block that can contain both operations and text characters.

`(<.> ...)` is used specifically for menu item lists (see [Menus](#menus)).

## Text and Display

### Displaying Text

```racket
(text "【景】こんにちは。")
```

Text is displayed in the current text frame at the current cursor position.

### Text Color

```racket
(text-color 7)    ; set text color to 7 (white)
```

Text color is **persistent**: once set, all subsequent text will use that color until it is changed again. The `#:color` keyword is shorthand for setting the color on a specific text command:

```racket
(text #:color 7 "【景】Hello.")       ; sets color to 7
(text "This is still color 7.")       ; color persists
(text #:color 2 "【亜子】Hi there.")  ; now changes to color 2
(text "Still color 2.")               ; persists
```

### Protagonist Name Fusion

When `--protag` is used during decompilation, proc/call instructions get fused into text:

```racket
(text "【" 0 "】Hello.")
```

The `0` between strings represents `(proc 0)`, which inserts the protagonist's name. When compiled, this expands back to separate instructions.

A variable in the same spot represents a call instead: `(text "【" Z "】Hello.")` compiles to `(call Z)`. A proc on a variable is instead written out as `(proc Z)`, which would take Z's value and run the proc that matches.

### Displaying Numbers in Text

The `number` command inserts a numeric value inline within text output:

```racket
(text "バブ・・・・・・" (number (~ B 2)) "個")   ; "Bab...... [value of B[2]] pieces"
(text (number (- Y (~ S 1))))                    ; display Y - S[1] as text
```

### Waiting and Clearing

```racket
(text "First line of dialogue.")
(wait)                            ; wait for player to press a key
(text "Second line after input.")
(clear)                           ; clear the text display area
```

### Drawing Color

```racket
(color 8)    ; set the drawing color (used by box, box-inv, etc.)
```

This is separate from text-color. It sets the color used by drawing commands like `box`.

## Menus

### Displaying a Menu

```racket
(menu-show
 (<.>
  (text "装備をする")
  (text "移動速度の変更")
  (text "やめる")))
```

This displays a menu with three options. The player's selection is stored in `S`:
- `S = 0`: cancelled / no selection
- `S = 1`: first option
- `S = 2`: second option
- etc.

### Conditional Menu Items

Menu items can be conditionally included using `if` with `str`:

```racket
(menu-show
 (<.>
  (if (!= (: 930) 0) (str "STICK         "))
  (if (!= (: 931) 0) (str "KNIFE         "))
  (if (!= (: 932) 0) (str "DAGGAR        "))))
```

Items only appear if their condition is true. `str` is used instead of `text` for menu item content.

### Menu Initialization

```racket
(menu-init)    ; reset menu state before displaying a new menu
```

## Procedures

### Defining Procedures

```racket
(define-proc 13
 (<>
  (set-arr~ @ 7 0)
  (set-arr~ @ 20 (// (&& (~ @ 20) 4095) 4096))
  (set-arr~ @ 21 (+ 256 16))))
```

Procedures are numbered (typically 0-63). They are defined inline in the current script and can be called from anywhere within it.

### Calling Procedures

```racket
(proc 13)       ; call procedure 13
(call Z)        ; call the procedure whose number is stored in variable Z
```

## Graphics

### Loading and Displaying Images

```racket
(image "19.pd8")    ; load and display an image file
```

### Blitting (Copying Regions)

`blit` copies a rectangular region from one display plane to another:

```racket
(blit src-x1 src-y1 src-x2 src-y2 src-plane dst-x dst-y mode)
```

- `src-x1, src-y1, src-x2, src-y2`: source rectangle coordinates
- `src-plane`: source display plane number
- `dst-x, dst-y`: destination position
- `mode`: transfer mode (0 = normal copy, 1 = XOR, 2 = OR, etc.)

```racket
(blit L M (+ L 39) (+ M 127) 0 12 272 1)    ; copy a 40x128 region from plane 0 to (12, 272)
```

### Blit-Swap (Exchanging Regions)

`blit-swap` exchanges two rectangular regions between planes (useful for double-buffering):

```racket
(blit-swap x1 y1 x2 y2 plane1 dst-x dst-y plane2)
```

```racket
(blit-swap 0 0 79 399 1 0 0 2)    ; swap an 80x400 region between planes 1 and 2
```

### Blit-Mask (Transparent Copy)

`blit-mask` copies a region with transparency masking (transparent pixels are not drawn):

```racket
(blit-mask src-x1 src-y1 src-x2 src-y2 src-plane dst-x dst-y mode)
```

```racket
(blit-mask 12 144 (+ 12 39) (+ 144 127) 1 L M 0)    ; masked copy from plane 1
```

### Drawing Rectangles

`box` draws a filled rectangle in the current drawing color:

```racket
(box x1 y1 x2 y2)
```

```racket
(color 8)                              ; set drawing color
(box 67 370 (+ 67 11) (+ 370 17))     ; draw filled rectangle
```

`box-inv` draws an inverted rectangle (XORs the region, commonly used for selection highlights):

```racket
(box-inv x1 y1 x2 y2)
```

```racket
(box-inv (+ 10 (* (: 101) 46)) (+ (* I 16) 8 8)
         (+ 23 (* (: 101) 46)) (+ (* I 16) 24 8))
```

### Animation

`animate` controls sprite animation layers:

```racket
(animate layer frame flags)
```

- `layer`: animation layer number (0, 1, etc.)
- `frame`: frame index to display
- `flags`: optional control flags

```racket
(animate 0 0 0)    ; set layer 0 to frame 0, flags 0
(animate 1 0)      ; set layer 1 to frame 0 (no flags)
(animate 0 8 0)    ; set layer 0 to frame 8
```

## Sound

`sound` plays or controls music and sound effects:

```racket
(sound track)
(sound track channel)
```

- `track`: track/sound ID number
- `channel`: optional channel or playback mode

```racket
(sound 1)       ; play track 1
(sound 3 1)     ; play track 3 on channel 1
```

## File Operations

### Jumping to Another Script

```racket
(mes-jump "NEXT.MES")    ; jump to another MES file (does not return)
(mes-call "SUB.MES")     ; call another MES file (returns when done)
```

### Loading Data

`load` loads an external data file into memory at a specified address:

```racket
(load filename address)
```

```racket
(load "mouse.dat" 49152)     ; load mouse cursor data at address 49152
(load "t.s4" (~ @ 2))        ; load animation data at address stored in @[2]
```

### Field Access

`field` reads or writes structured game state data. The first argument is the field ID, followed by field-specific parameters:

```racket
(field field-id args...)
```

```racket
(field 267 (: 1020))    ; access field 267, read register 1020
(field 14 1 1)          ; set field 14 with parameters 1, 1
(field 13 0 0)          ; set field 13 with parameters 0, 0
(field 15 4096)         ; set field 15 to 4096
```

The meaning of field IDs and parameters depends on the specific game.

## System Functions

### Mouse

`mouse` controls mouse cursor visibility and behavior:

```racket
(mouse state)
(mouse state address)
```

```racket
(mouse 0)           ; disable/hide mouse cursor
(mouse 1)           ; enable/show mouse cursor
(mouse 4 49152)     ; set mouse mode 4 with cursor data at address 49152
```

### Click Detection

`click` checks for or waits for mouse clicks at a position:

```racket
(click address x y)
```

```racket
(click 47104 X Y)    ; check for click using data at address 47104, at position (X, Y)
```

### Palette

`palette` loads or applies color palettes:

```racket
(palette palette-id)
(palette palette-id mode)
```

```racket
(palette 3)       ; apply palette 3
(palette 3 0)     ; apply palette 3 with mode 0
```

### Save/Load Flags

`flag` saves or loads game state flags:

```racket
(flag operation value)
```

```racket
(flag 1 0)           ; set flag operation 1 to value 0
(flag 2 (- N 1))     ; set flag operation 2 to N - 1
```

### Delay

`delay` pauses execution for a duration (in milliseconds):

```racket
(delay milliseconds)
```

```racket
(delay 500)     ; pause for 500ms
(delay 1000)    ; pause for 1 second
(delay W)       ; pause for W milliseconds (variable)
```

### Wait

```racket
(wait)    ; pause until the player presses a key
```

### Utility

`util` is a catch-all for miscellaneous engine operations. The first argument is a sub-opcode that selects the operation:

```racket
(util opcode)
(util opcode parameter)
```

```racket
(util 7)      ; utility operation 7
(util 1)      ; utility operation 1
(util 0 0)    ; utility operation 0, parameter 0
(util 0 1)    ; utility operation 0, parameter 1
```

The specific meaning of each opcode depends on the game and engine version.

## Word Wrapping

For translation projects, long text can be automatically wrapped to fit the text box. Add `--auto-wrap` when compiling, or set a wrap width per string:

```racket
(text "A very long translated string that needs to be wrapped." #:wrap 16)
```

The compiler will insert spaces to break lines at word boundaries.

## Unresolved Commands

Not all commands have been identified. Commands whose purpose is unknown appear with their raw opcode number:

```racket
(cmd:206 arg1 arg2 ...)
```

If you figure out what an unresolved command does, please let me know. Contributions and suggestions are welcome.

## Tips for Editing Scripts

- **Translate text in `(text ...)` and `(str ...)` nodes.** Leave everything else alone unless you know what you're doing.
- **Use `#:color` to set colors per line** rather than inserting separate `(text-color)` commands.
- **Replace `(dict ...)` with `(dict-build)`** after translation so the dictionary is regenerated for your new character set.
- **Set font width to single-width** (`(set-arr~ @ 21 (+ 256 16))`) for Western text.
- **Enable syntax highlighting for Racket, Scheme, or Lisp** in your editor for a much better editing experience.
- **Comments with `;`** are your friend for annotating what each section does.
