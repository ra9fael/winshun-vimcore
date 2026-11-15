// editor.hpp — the modal editing state machine over a Document.
//
// One VimEditor drives one Document. Feed keys with key(); every key either
// mutates the document through exactly one apply() call or only moves the
// cursor / changes mode. The class owns: mode, pending operator, pending
// find (f/F/t/T), pending replace (r), counts, yank register, undo/redo.
//
// Not implemented on purpose (README documents this): ex commands, named
// registers, macros, marks, search, the g prefix beyond gg, multiple buffers.
#pragma once

#include "vimcore/document.hpp"
#include "vimcore/keys.hpp"

#include <optional>
#include <string>
#include <vector>

namespace vimcore {

enum class Mode { Normal, Insert, Visual, VisualLine };

struct EditorOptions {
    std::size_t undoDepth = 1000;
};

class VimEditor {
public:
    explicit VimEditor(Document& document, EditorOptions options = {});

    void key(const Key& k);
    void keys(const std::vector<Key>& ks) { for (const Key& k : ks) key(k); }

    // Re-read text and cursor from the Document after it changed behind the
    // editor's back — native insert-mode typing, IME commits, pastes. Mode and
    // the open insert batch survive; operator/find pending state is dropped,
    // since the user's fingers were on the widget, not on vim keys. A no-op
    // when the document already matches the editor's copy.
    void refreshFromDocument();

    Mode mode() const { return m_mode; }
    // The yank register contents; exposed for tests and hosts that want a
    // system-clipboard bridge.
    const std::wstring& registerText() const { return m_register; }

private:
    // --- text model ----------------------------------------------------------
    // The editor works on its own copy and syncs to the Document as one diff
    // per key stroke. That keeps motion math free of callback churn and makes
    // undo a plain snapshot.
    std::size_t lineCount() const;
    std::size_t lineStart(std::size_t line) const;
    std::size_t lineLength(std::size_t line) const;    // without the '\n'
    std::wstring lineText(std::size_t line) const;
    std::size_t offset(std::size_t line, std::size_t col) const;
    Cursor cursorFor(std::size_t offsetIn) const;
    std::size_t lastCharCol(std::size_t line) const;   // vim's "last column"
    void clampCursor();

    // --- key handling stages -------------------------------------------------
    void handleNormal(const Key& k);
    void handleInsert(const Key& k);
    void handleVisual(const Key& k);
    void handlePending(const Key& k);      // f/F/t/T/r/gg waiting for their char

    // A batch is one undo step. beginEdit() opens one (and snapshots), each
    // mutating key flushes its diff, and endEdit() closes it.
    void beginEdit();
    void flushEdit();
    void endEdit();
    void emitDiff(const std::wstring& from);

    void moveCursor(std::size_t line, std::size_t col, bool clampToChar = true);
    void syncCursorAndSelection();

    // Motions: `count` steps; `hasCount` distinguishes bare G (last line) from
    // 1G (line 1). Non-const: it records whether the motion ran out of text.
    Cursor motionApply(Cursor from, const Key& k, int count, bool hasCount);

    void enterInsert(bool afterCursor, bool openLineBelow = false, bool openLineAbove = false);
    void exitInsert();

    void operatorDelete(std::size_t start, std::size_t end, bool linewise);
    void operatorChange(std::size_t start, std::size_t end, bool linewise);
    void operatorYank(std::size_t start, std::size_t end, bool linewise);
    void pasteAtCursor(bool after, int count);
    void removeCharsAtCursor(std::size_t count, bool yankFirst);
    void replaceCharAtCursor(wchar_t with);
    void joinWithNext();
    void adjustCursorAfterDelete();
    void doUndo();
    void doRedo();

    // Range for operator + motion, absolute offsets. `total` already combines
    // the operator count and the motion count.
    bool resolveRange(std::size_t& start, std::size_t& end, bool& linewise,
                      const Key& motion, int total);

    Document& m_doc;
    EditorOptions m_options;
    Mode m_mode = Mode::Normal;
    std::wstring m_text;
    Cursor m_cursor;

    std::wstring m_savedText;              // text the open batch started from
    Cursor m_savedCursor;                  // cursor the open batch started from
    bool m_editing = false;                // a batch is open

    enum class Pending { None, Find, FindBack, Till, TillBack, Replace, GG };
    Pending m_pending = Pending::None;

    Cursor m_visualAnchor;                 // where v/V was pressed

    wchar_t m_operator = L'\0';            // 'd'/'c'/'y' waiting for a motion
    int m_operatorCount = 1;               // count typed before the operator

    int m_count = 1;                       // digits accumulated for the next command
    bool m_countActive = false;

    std::optional<wchar_t> m_lastFindChar; // ; / , repeat
    bool m_lastFindForward = false;

    std::wstring m_register;
    bool m_lastYankLineWise = false;
    std::size_t m_desiredCol = 0;          // for j/k over shorter lines
    bool m_motionRanOut = false;           // last motion hit the end of the text

    struct Snapshot {
        std::wstring text;
        Cursor cursor;
    };
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
};

} // namespace vimcore
