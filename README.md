# vimcore

A small modal (vim-style) editing **core** in portable C++20. No Qt, no Win32,
no OS headers — the library only manipulates text through an interface the host
implements, so the same core can drive a `QPlainTextEdit`, a Win32 edit control,
or a plain test buffer.

Built for [Win顺](https://github.com/LingCore/WinShun) plugins (批量重命名's text
mode is the first user), but it depends on nothing from Win顺.

## Integrate

```
git submodule add https://github.com/ra9fael/winshun-vimcore vendor/vimcore
```

```cmake
add_subdirectory(vendor/vimcore)
target_link_libraries(my_app PRIVATE vimcore)
```

Implement `vimcore::Document` over your text widget, then:

```cpp
vimcore::VimEditor editor(document);
for (const vimcore::Key& k : translatedKeys)
    editor.key(k);            // one key -> at most one atomic apply()
```

Lines are separated by `'\n'`. `QPlainTextEdit` already stores text that way,
which makes the adapter a few dozen lines.

## Supported (0.2)

- Modes: Normal, Insert, Replace (`R`), Visual (`v`), Visual Line (`V`)
- Motions: `h j k l w b e 0 ^ $ % gg G f{c} F{c} t{c} T{c}`, `Space`/`<BS>` (the
  two whichwrap defaults), arrow keys, Home/End, counts everywhere (`3w`, `2dd`,
  `5j`). `h l <Left> <Right>` never cross lines.
- Insert entries: `i a I A o O s`, `{count}` repeats the typed session
  (`3iab<Esc>` → `ababab`), exit with Esc
- Replace mode: overwrite typing, `<BS>` restores the original character
- Operators: `d c y` + motion, doubled (`dd cc yy`), `D C S x X Y p P J r{c}`
  and the text objects `iw aw` (`ciw`, `daw`, …)
- Numeric edit: `Ctrl+A` / `Ctrl+X` on the number under or after the cursor,
  with sign, zero padding (`007` → `008`) and counts
- Paging: `Ctrl+D/U` half a page, `Ctrl+F/B` and PageUp/PageDown a page, sized
  by `setViewportRows()`
- Visual: `o` swaps ends, `u U ~` case, `p` replaces the selection (registers
  swap, as vim does), `J` joins, `iw`/`aw` extend the selection, `v`/`V` switch
  kinds without losing the anchor
- Undo/redo: `u` and `Ctrl+R` with counts (`3u`), the core keeps its own
  snapshots independent of the host's undo stack; the register survives undo
- Repeat: `.` replays the last change, including insert sessions and native
  (host-typed) characters reported via `refreshFromDocument()`
- Repeat last find with `;` and `,`

## Not implemented on purpose

ex command line, named registers (only the unnamed register), macros, marks,
search (`/ ?` — hosts have their own), the `g` prefix beyond `gg`, multiple
buffers, folding. Additions are welcome as small, tested increments.

## Build & test

```
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
```

MIT — see [LICENSE](LICENSE).
