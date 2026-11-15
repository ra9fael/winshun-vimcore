// document.hpp — the host-owned text buffer vimcore operates on.
//
// Lines are separated by '\n' and indexed from 0. The core reads the whole
// text, computes edits, and hands back one atomic batch per user key stroke;
// the host applies them however it likes (QPlainTextEdit, Win32 edit, its own
// buffer). The host also owns the visual cursor rendering: the core asks for
// cursor and selection updates through the same interface.
//
// Undo is NOT the host's undo stack. VimEditor keeps its own snapshots so that
// `u` and Ctrl+r behave like vim regardless of what the host supports.
#pragma once

#include <string>
#include <vector>

namespace vimcore {

struct Cursor {
    std::size_t line = 0;   // 0-based
    std::size_t col = 0;    // 0-based, in code units; may be on the '\n'
};

struct TextEdit {
    std::size_t start = 0;  // absolute offset into text(), inclusive
    std::size_t end = 0;    // exclusive; start == end means insert
    std::wstring replacement;
};

struct Selection {
    Cursor anchor;
    Cursor head;
    bool active = false;
    bool lineWise = false;  // V mode: whole lines
};

class Document {
public:
    virtual ~Document() = default;

    virtual std::wstring text() const = 0;
    virtual Cursor cursor() const = 0;

    // Apply edits atomically. Edits are sorted and non-overlapping. The core
    // always sends exactly one edit (the diff of this key stroke) or none.
    virtual void apply(const std::vector<TextEdit>& edits) = 0;
    virtual void setCursor(Cursor cursor) = 0;
    virtual void setSelection(const Selection& selection) = 0;
    virtual void setStatus(const wchar_t* status) = 0;   // "-- NORMAL --" etc; may be empty
};

} // namespace vimcore
