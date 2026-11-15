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

## Supported (v1)

- Modes: Normal, Insert, Visual (`v`), Visual Line (`V`)
- Motions: `h j k l w b e 0 ^ $ gg G f{c} F{c} t{c} T{c}`, arrow keys, Home/End,
  counts everywhere (`3w`, `2dd`, `5j`)
- Insert entries: `i a I A o O s`, exit with Esc
- Operators: `d c y` + motion, doubled (`dd cc yy`), `D C S x X p P J r{c}`
- Undo/redo: `u`, `Ctrl+R` — the core keeps its own snapshots, independent of
  the host's undo stack
- Repeat last find with `;` and `,`

## Not implemented on purpose

ex command line, named registers (only the unnamed register), macros, marks,
search (`/ ?` — hosts have their own), the `g` prefix, multiple buffers,
folding. Additions are welcome as small, tested increments.

## Build & test

```
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
```

MIT — see [LICENSE](LICENSE).
