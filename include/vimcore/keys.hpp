// keys.hpp — the key events vimcore consumes.
//
// The host converts its own key events into this shape, so the core never sees
// Qt, Win32, or any other toolkit. A Key is one key press, not a character
// stream: `ch` carries the character for KeyName::Char and for find/repeat
// arguments (f, F, t, T, r).
#pragma once

#include <string>
#include <vector>

namespace vimcore {

enum class KeyName {
    Char,        // printable character in `ch`
    Esc,
    Enter,
    Backspace,
    Delete,
    Tab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
};

struct Key {
    KeyName name = KeyName::Char;
    wchar_t ch = L'\0';
    int count = 1;       // 1 means "no count typed"; 0 is normalised to 1
    bool ctrl = false;
};

inline Key makeChar(wchar_t ch, int count = 1) { return Key{KeyName::Char, ch, count < 1 ? 1 : count, false}; }
inline Key makeKey(KeyName name, int count = 1, bool ctrl = false) { return Key{name, L'\0', count < 1 ? 1 : count, ctrl}; }

// Convenience: feed a whole sequence, one Key per code unit. Good for tests
// and for replaying a recorded macro; not a general decoder.
inline std::vector<Key> chars(const wchar_t* text)
{
    std::vector<Key> keys;
    for (const wchar_t* p = text; *p; ++p)
        keys.push_back(makeChar(*p));
    return keys;
}

} // namespace vimcore
