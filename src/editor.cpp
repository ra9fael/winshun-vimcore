// editor.cpp — the modal editing state machine. See editor.hpp for the shape.
//
// Invariants this file must not break:
//   1. One user key stroke produces at most one Document::apply() call, so the
//      host never sees a half-applied batch.
//   2. Normal and Visual mode cursors always sit on an existing character;
//      only Insert mode may sit past the last character of a line.
//   3. Undo snapshots are taken when a *batch* opens, so a whole insert session
//      is one undo even though each typed character reaches the host.
#include "vimcore/editor.hpp"

#include <algorithm>
#include <cwctype>

namespace vimcore {

namespace {

bool isWordChar(wchar_t ch)
{
    return std::iswalnum(static_cast<wint_t>(ch)) != 0 || ch == L'_';
}

bool isSpaceChar(wchar_t ch)
{
    return ch == L' ' || ch == L'\t';
}

bool isLineBreak(wchar_t ch)
{
    return ch == L'\n';
}

} // namespace

VimEditor::VimEditor(Document& document, EditorOptions options)
    : m_doc(document)
    , m_options(options)
{
    m_text = m_doc.text();
    m_cursor = m_doc.cursor();
    clampCursor();
    m_desiredCol = m_cursor.col;
    m_doc.setStatus(L"-- NORMAL --");
    syncCursorAndSelection();
}

// ---- text model ---------------------------------------------------------------

std::size_t VimEditor::lineCount() const
{
    std::size_t count = 1;
    for (const wchar_t ch : m_text)
    {
        if (isLineBreak(ch))
            ++count;
    }
    return count;
}

std::size_t VimEditor::lineStart(std::size_t line) const
{
    std::size_t pos = 0;
    for (std::size_t i = 0; i < line; ++i)
    {
        while (pos < m_text.size() && !isLineBreak(m_text[pos]))
            ++pos;
        if (pos < m_text.size())
            ++pos; // step over the '\n'
    }
    return pos;
}

std::size_t VimEditor::lineLength(std::size_t line) const
{
    const std::size_t start = lineStart(line);
    std::size_t end = start;
    while (end < m_text.size() && !isLineBreak(m_text[end]))
        ++end;
    return end - start;
}

std::wstring VimEditor::lineText(std::size_t line) const
{
    return m_text.substr(lineStart(line), lineLength(line));
}

std::size_t VimEditor::offset(std::size_t line, std::size_t col) const
{
    return lineStart(line) + col;
}

Cursor VimEditor::cursorFor(std::size_t offsetIn) const
{
    Cursor cursor;
    const std::size_t limit = std::min(offsetIn, m_text.size());
    for (std::size_t i = 0; i < limit; ++i)
    {
        if (isLineBreak(m_text[i]))
        {
            ++cursor.line;
            cursor.col = 0;
        }
        else
            ++cursor.col;
    }
    return cursor;
}

std::size_t VimEditor::lastCharCol(std::size_t line) const
{
    const std::size_t len = lineLength(line);
    return len == 0 ? 0 : len - 1;
}

void VimEditor::clampCursor()
{
    if (m_cursor.line >= lineCount())
        m_cursor.line = lineCount() - 1;
    const std::size_t max = m_mode == Mode::Insert ? lineLength(m_cursor.line)
                                                   : lastCharCol(m_cursor.line);
    if (m_cursor.col > max)
        m_cursor.col = max;
}

void VimEditor::moveCursor(std::size_t line, std::size_t col, bool clampToChar)
{
    if (line >= lineCount())
        line = lineCount() - 1;
    const std::size_t max = clampToChar ? lastCharCol(line) : lineLength(line);
    m_cursor.line = line;
    m_cursor.col = std::min(col, max);
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

void VimEditor::syncCursorAndSelection()
{
    m_doc.setCursor(m_cursor);
    Selection selection;
    if (m_mode == Mode::Visual || m_mode == Mode::VisualLine)
    {
        selection.active = true;
        selection.lineWise = m_mode == Mode::VisualLine;
        selection.anchor = m_visualAnchor;
        selection.head = m_cursor;
    }
    m_doc.setSelection(selection);
    switch (m_mode)
    {
    case Mode::Normal:     m_doc.setStatus(L"-- NORMAL --"); break;
    case Mode::Insert:     m_doc.setStatus(L"-- INSERT --"); break;
    case Mode::Visual:     m_doc.setStatus(L"-- VISUAL --"); break;
    case Mode::VisualLine: m_doc.setStatus(L"-- VISUAL LINE --"); break;
    }
}

// ---- edit batching --------------------------------------------------------------

void VimEditor::beginEdit()
{
    if (m_editing)
        return;
    m_undo.push_back({m_text, m_cursor});
    while (m_undo.size() > m_options.undoDepth)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_savedText = m_text;
    m_savedCursor = m_cursor;
    m_editing = true;
}

void VimEditor::emitDiff(const std::wstring& from)
{
    if (from == m_text)
        return;

    // One edit spanning the common prefix and suffix: the host sees the whole
    // key stroke as one change.
    std::size_t prefix = 0;
    while (prefix < from.size() && prefix < m_text.size() && from[prefix] == m_text[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < from.size() - prefix && suffix < m_text.size() - prefix
           && from[from.size() - 1 - suffix] == m_text[m_text.size() - 1 - suffix])
        ++suffix;

    std::vector<TextEdit> edits(1);
    edits[0].start = prefix;
    edits[0].end = from.size() - suffix;
    edits[0].replacement = m_text.substr(prefix, m_text.size() - suffix - prefix);
    m_doc.apply(edits);
}

void VimEditor::flushEdit()
{
    if (!m_editing)
        return;
    if (m_text != m_savedText)
    {
        emitDiff(m_savedText);
        m_savedText = m_text;
        m_savedCursor = m_cursor;
    }
}

void VimEditor::endEdit()
{
    flushEdit();
    m_editing = false;
}

// ---- mode transitions ----------------------------------------------------------

void VimEditor::enterInsert(bool afterCursor, bool openLineBelow, bool openLineAbove)
{
    beginEdit();
    if (openLineBelow)
    {
        const std::size_t at = lineStart(m_cursor.line) + lineLength(m_cursor.line);
        m_text.insert(at, L"\n");
        ++m_cursor.line;
        m_cursor.col = 0;
    }
    else if (openLineAbove)
    {
        const std::size_t at = lineStart(m_cursor.line);
        m_text.insert(at, L"\n");
        m_cursor.col = 0;
    }
    else if (afterCursor)
    {
        // 'a' steps past the current char; on an empty line it stays at 0.
        if (lineLength(m_cursor.line) > 0)
            ++m_cursor.col;
    }
    m_mode = Mode::Insert;
    m_desiredCol = m_cursor.col;
    flushEdit();
    syncCursorAndSelection();
}

void VimEditor::exitInsert()
{
    endEdit();
    if (m_cursor.col > 0 && m_cursor.col > lastCharCol(m_cursor.line))
        --m_cursor.col; // Esc steps back onto the last char, as vim does
    m_mode = Mode::Normal;
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

// ---- operators ------------------------------------------------------------------

void VimEditor::operatorDelete(std::size_t start, std::size_t end, bool linewise)
{
    if (end < start)
        std::swap(start, end);
    if (start >= m_text.size() && start != 0)
        return;
    end = std::min(end, m_text.size());
    beginEdit();
    m_register = m_text.substr(start, end - start);
    m_lastYankLineWise = linewise;
    // A line-wise register always ends with a newline, as vim's does: it is what
    // makes "dd" followed by "p" on the last line put the line back.
    if (linewise && !m_register.empty() && m_register.back() != L'\n')
        m_register += L'\n';
    m_text.erase(start, end - start);
    m_cursor = cursorFor(start);
    m_mode = Mode::Normal;
    adjustCursorAfterDelete();
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::operatorChange(std::size_t start, std::size_t end, bool linewise)
{
    if (end < start)
        std::swap(start, end);
    end = std::min(end, m_text.size());
    beginEdit();
    m_register = m_text.substr(start, end - start);
    m_lastYankLineWise = linewise;
    if (linewise)
    {
        // cc leaves the line empty and enters insert on it.
        const std::size_t line = cursorFor(start).line;
        m_text.erase(start, lineLength(line));
        m_cursor = {line, 0};
    }
    else
    {
        m_text.erase(start, end - start);
        m_cursor = cursorFor(start);
    }
    m_mode = Mode::Insert;
    m_desiredCol = m_cursor.col;
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::operatorYank(std::size_t start, std::size_t end, bool linewise)
{
    if (end < start)
        std::swap(start, end);
    end = std::min(end, m_text.size());
    m_register = m_text.substr(start, end - start);
    m_lastYankLineWise = linewise;
    if (linewise && !m_register.empty() && m_register.back() != L'\n')
        m_register += L'\n';
    // A yank never changes the text; vim leaves the cursor at the range start.
    m_cursor = cursorFor(start);
    m_mode = Mode::Normal;
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

void VimEditor::pasteAtCursor(bool after, int count)
{
    if (m_register.empty() || count < 1)
        return;

    std::wstring block;
    for (int i = 0; i < count; ++i)
        block += m_register;

    if (m_lastYankLineWise)
    {
        // A line-wise register is a run of whole lines, newline-terminated.
        if (block.back() != L'\n')
            block += L'\n';
        beginEdit();
        std::size_t at = 0;
        if (after)
        {
            if (m_cursor.line + 1 < lineCount())
                at = lineStart(m_cursor.line + 1);   // between this line and the next
            else
            {
                // After the last line: a missing trailing newline would glue the
                // pasted block onto the current text, so add the separator.
                at = m_text.size();
                if (!m_text.empty() && m_text.back() != L'\n')
                    m_text.insert(at, L"\n"), ++at;
            }
        }
        else
            at = lineStart(m_cursor.line);           // above the current line

        m_text.insert(at, block);
        m_cursor = cursorFor(at);
        m_desiredCol = m_cursor.col;
        endEdit();
        syncCursorAndSelection();
        return;
    }

    beginEdit();
    const std::size_t at = offset(m_cursor.line, m_cursor.col)
                         + (after && lineLength(m_cursor.line) > 0 ? 1 : 0);
    m_text.insert(at, block);
    m_cursor = cursorFor(at + block.size() - 1); // land on the last pasted char
    m_desiredCol = m_cursor.col;
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::removeCharsAtCursor(std::size_t count, bool yankFirst)
{
    const std::size_t lineLen = lineLength(m_cursor.line);
    if (lineLen == 0)
        return;
    const std::size_t start = offset(m_cursor.line, m_cursor.col);
    const std::size_t lineEnd = lineStart(m_cursor.line) + lineLen;
    const std::size_t end = std::min(start + count, lineEnd);
    beginEdit();
    if (yankFirst)
    {
        m_register = m_text.substr(start, end - start);
        m_lastYankLineWise = false;
    }
    m_text.erase(start, end - start);
    m_cursor = cursorFor(start);
    adjustCursorAfterDelete();
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::replaceCharAtCursor(wchar_t with)
{
    const std::size_t at = offset(m_cursor.line, m_cursor.col);
    if (at >= m_text.size() || isLineBreak(m_text[at]))
        return;
    beginEdit();
    m_text[at] = with;
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::joinWithNext()
{
    if (m_cursor.line + 1 >= lineCount())
        return;
    beginEdit();
    const std::size_t currentStart = lineStart(m_cursor.line);
    const std::size_t currentLen = lineLength(m_cursor.line);
    const std::size_t newlineAt = currentStart + currentLen;
    std::size_t nextContent = newlineAt + 1;
    while (nextContent < m_text.size() && isSpaceChar(m_text[nextContent]))
        ++nextContent;
    const bool currentEndsInSpace = currentLen > 0 && m_text[newlineAt - 1] == L' ';
    const bool nextEmpty = (nextContent >= m_text.size()) || isLineBreak(m_text[nextContent]);
    const bool glue = !currentEndsInSpace && !nextEmpty && currentLen > 0;
    const std::size_t removed = nextContent - newlineAt;
    if (glue)
        m_text.replace(newlineAt, removed, L" ");
    else
        m_text.erase(newlineAt, removed);
    // The cursor lands on the joining space (or the last char of the line).
    m_cursor = {m_cursor.line, currentLen > 0 ? currentLen : 0};
    if (m_cursor.col > lastCharCol(m_cursor.line))
        m_cursor.col = lastCharCol(m_cursor.line);
    m_desiredCol = m_cursor.col;
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::adjustCursorAfterDelete()
{
    const std::size_t len = lineLength(m_cursor.line);
    const std::size_t max = len == 0 ? 0 : len - 1;
    if (m_cursor.col > max)
        m_cursor.col = max;
    m_desiredCol = m_cursor.col;
}

// ---- undo / redo ----------------------------------------------------------------

void VimEditor::doUndo()
{
    if (m_undo.empty())
        return;
    endEdit(); // close any open batch first
    m_redo.push_back({m_text, m_cursor});
    const Snapshot snap = m_undo.back();
    m_undo.pop_back();
    const std::wstring from = m_text;
    m_text = snap.text;
    m_cursor = snap.cursor;
    m_mode = Mode::Normal;
    clampCursor();
    emitDiff(from);
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

void VimEditor::doRedo()
{
    if (m_redo.empty())
        return;
    endEdit();
    m_undo.push_back({m_text, m_cursor});
    const Snapshot snap = m_redo.back();
    m_redo.pop_back();
    const std::wstring from = m_text;
    m_text = snap.text;
    m_cursor = snap.cursor;
    m_mode = Mode::Normal;
    clampCursor();
    emitDiff(from);
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

// ---- external changes -----------------------------------------------------------

void VimEditor::refreshFromDocument()
{
    const std::wstring external = m_doc.text();
    if (external == m_text)
        return; // our own apply already echoed back; keep cursor and state

    const Mode keepMode = m_mode;
    const bool keepEditing = m_editing;

    m_text = external;
    m_cursor = m_doc.cursor();
    m_mode = keepMode;
    clampCursor();
    m_desiredCol = m_cursor.col;

    if (keepEditing)
    {
        // The open insert batch continues, but its diff baseline moves to the
        // text as it actually is now: the native typing is part of this batch,
        // and the snapshot taken at beginEdit() still undoes all of it.
        m_savedText = m_text;
        m_savedCursor = m_cursor;
    }
    else
        ; // Outside an open batch there is no pre-change snapshot to infer; the
          // host remains the source of truth, and future vim edits are undoable.

    // The user typed in the widget; vim key state cannot carry over.
    m_operator = L'\0';
    m_pending = Pending::None;
    m_count = 1;
    m_countActive = false;
    syncCursorAndSelection();
}

// ---- motions --------------------------------------------------------------------

Cursor VimEditor::motionApply(Cursor from, const Key& k, int count, bool hasCount)
{
    Cursor c = from;
    const int n = count < 1 ? 1 : count;
    m_motionRanOut = false;

    switch (k.name)
    {
    case KeyName::Left:
        for (int i = 0; i < n; ++i)
        {
            if (c.col > 0) --c.col;
            else if (c.line > 0) { --c.line; c.col = lastCharCol(c.line); }
        }
        break;
    case KeyName::Right:
        for (int i = 0; i < n; ++i)
        {
            if (c.col < lastCharCol(c.line)) ++c.col;
            else if (c.line + 1 < lineCount()) { ++c.line; c.col = 0; }
        }
        break;
    case KeyName::Up:
        for (int i = 0; i < n && c.line > 0; ++i)
            --c.line;
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
        break;
    case KeyName::Down:
        for (int i = 0; i < n && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
        break;
    case KeyName::Home:
        c.col = 0;
        break;
    case KeyName::End:
        for (int i = 0; i < n && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = lastCharCol(c.line);
        break;
    case KeyName::PageUp:
        for (int i = 0; i < n * 10 && c.line > 0; ++i)
            --c.line;
        c.col = std::min(m_desiredCol, lastCharCol(c.line));
        break;
    case KeyName::PageDown:
        for (int i = 0; i < n * 10 && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = std::min(m_desiredCol, lastCharCol(c.line));
        break;
    default:
        break;
    }
    if (k.name != KeyName::Char)
    {
        if (c.line >= lineCount()) c.line = lineCount() - 1;
        if (c.col > lastCharCol(c.line)) c.col = lastCharCol(c.line);
        return c;
    }

    switch (k.ch)
    {
    case L'h':
        for (int i = 0; i < n; ++i)
        {
            if (c.col > 0) --c.col;
            else if (c.line > 0) { --c.line; c.col = lastCharCol(c.line); }
        }
        break;
    case L'l':
        for (int i = 0; i < n; ++i)
        {
            if (c.col < lastCharCol(c.line)) ++c.col;
            else if (c.line + 1 < lineCount()) { ++c.line; c.col = 0; }
        }
        break;
    case L'j':
        for (int i = 0; i < n && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
        break;
    case L'k':
        for (int i = 0; i < n && c.line > 0; ++i)
            --c.line;
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
        break;
    case L'0':
        c.col = 0;
        break;
    case L'^':
    {
        const std::wstring line = lineText(c.line);
        std::size_t i = 0;
        while (i < line.size() && isSpaceChar(line[i]))
            ++i;
        c.col = i < line.size() ? i : 0;
        break;
    }
    case L'$':
        for (int i = 0; i < n && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = lastCharCol(c.line);
        break;
    case L'G':
        if (hasCount)
            c.line = std::min<std::size_t>(static_cast<std::size_t>(n) - 1, lineCount() - 1);
        else
            c.line = lineCount() - 1;
        c.col = std::min(from.col, lastCharCol(c.line));
        break;
    case L'w':
        for (int step = 0; step < n; ++step)
        {
            std::size_t pos = offset(c.line, c.col);
            if (pos >= m_text.size())
                break;
            // Skip the run the cursor is on, then the whitespace after it.
            const bool onWord = isWordChar(m_text[pos]);
            while (pos < m_text.size() && !isSpaceChar(m_text[pos]) && !isLineBreak(m_text[pos])
                   && isWordChar(m_text[pos]) == onWord)
                ++pos;
            while (pos < m_text.size() && (isSpaceChar(m_text[pos]) || isLineBreak(m_text[pos])))
                ++pos;
            if (pos >= m_text.size())
            {
                // Nothing after: vim rests on the last character. Remember it so
                // an operator knows "w" ran out of text (vw's yw on the last word).
                m_motionRanOut = true;
                c = cursorFor(m_text.size() == 0 ? 0 : m_text.size() - 1);
                break;
            }
            c = cursorFor(pos);
        }
        break;
    case L'b':
        for (int step = 0; step < n; ++step)
        {
            std::size_t pos = offset(c.line, c.col);
            if (pos == 0)
                break;
            --pos;
            while (pos > 0 && (isSpaceChar(m_text[pos]) || isLineBreak(m_text[pos])))
                --pos;
            const bool wordChar = isWordChar(m_text[pos]);
            while (pos > 0)
            {
                const std::size_t prev = pos - 1;
                if (isSpaceChar(m_text[prev]) || isLineBreak(m_text[prev]))
                    break;
                if (isWordChar(m_text[prev]) != wordChar)
                    break;
                --pos;
            }
            c = cursorFor(pos);
        }
        break;
    case L'e':
        for (int step = 0; step < n; ++step)
        {
            std::size_t pos = offset(c.line, c.col);
            if (pos < m_text.size())
                ++pos;
            while (pos < m_text.size() && (isSpaceChar(m_text[pos]) || isLineBreak(m_text[pos])))
                ++pos;
            if (pos >= m_text.size())
            {
                c = cursorFor(m_text.size() == 0 ? 0 : m_text.size() - 1);
                break;
            }
            // Walk to the end of this word run.
            while (pos + 1 < m_text.size() && isWordChar(m_text[pos])
                   && isWordChar(m_text[pos + 1]))
                ++pos;
            c = cursorFor(pos);
        }
        break;
    default:
        break;
    }

    if (c.line >= lineCount()) c.line = lineCount() - 1;
    if (c.col > lastCharCol(c.line)) c.col = lastCharCol(c.line);
    return c;
}

bool VimEditor::resolveRange(std::size_t& start, std::size_t& end, bool& linewise,
                             const Key& motion, int total)
{
    if (motion.name != KeyName::Char)
    {
        // Arrow/Home/End as an operator target.
        const Cursor target = motionApply(m_cursor, motion, total, total > 1);
        start = std::min(offset(m_cursor.line, m_cursor.col), offset(target.line, target.col));
        end = std::max(offset(m_cursor.line, m_cursor.col), offset(target.line, target.col)) + 1;
        linewise = target.line != m_cursor.line;
        if (linewise)
        {
            const std::size_t fromLine = cursorFor(start).line;
            const std::size_t toLine = cursorFor(end - 1).line;
            start = lineStart(fromLine);
            end = lineStart(toLine) + lineLength(toLine) + (toLine + 1 < lineCount() ? 1 : 0);
        }
        return true;
    }

    if (motion.ch == L'0' || motion.ch == L'^')
    {
        const std::size_t lineStartAt = lineStart(m_cursor.line);
        std::size_t target = lineStartAt;
        if (motion.ch == L'^')
        {
            const std::wstring line = lineText(m_cursor.line);
            std::size_t i = 0;
            while (i < line.size() && isSpaceChar(line[i]))
                ++i;
            target = lineStartAt + (i < line.size() ? i : 0);
        }
        start = target;
        end = offset(m_cursor.line, m_cursor.col);
        linewise = false;
        return true;
    }

    const Cursor target = motionApply(m_cursor, motion, total, total > 1);
    const std::size_t from = offset(m_cursor.line, m_cursor.col);
    const std::size_t to = offset(target.line, target.col);

    linewise = motion.ch == L'j' || motion.ch == L'k' || motion.ch == L'G';
    if (linewise)
    {
        const std::size_t fromLine = std::min(m_cursor.line, target.line);
        const std::size_t toLine = std::max(m_cursor.line, target.line);
        start = lineStart(fromLine);
        end = lineStart(toLine) + lineLength(toLine) + (toLine + 1 < lineCount() ? 1 : 0);
        return true;
    }

    // Inclusive motions swallow the target character; exclusive ones stop in
    // front of it. This is what makes dw delete "foo " while de hits the last
    // character of the line deletes it too.
    const bool inclusive = motion.ch == L'$' || motion.ch == L'e' || motion.ch == L'f'
                        || motion.ch == L'F';
    const bool forwardMotion = motion.ch == L'l' || motion.ch == L'w' || motion.ch == L'e'
                            || motion.ch == L'$' || motion.ch == L'f' || motion.ch == L't';
    start = std::min(from, to);
    end = std::max(from, to) + (inclusive ? 1 : 0);
    // An exclusive motion that cannot advance still covers the character under
    // the cursor: "dl" on a one-character line deletes it, exactly like vim.
    if (!inclusive && forwardMotion && to == from)
        end = from + 1;
    // 'w' is exclusive, except where it ran out of text: "yw" on the last word
    // must include it, since there is no next word in front of it.
    if (motion.ch == L'w' && m_motionRanOut)
    {
        const Cursor targetCursor = cursorFor(to);
        end = std::max(end, lineStart(targetCursor.line) + lineLength(targetCursor.line));
    }
    end = std::min(end, m_text.size());
    return true;
}

// ---- pending (f/F/t/T/r/gg) -----------------------------------------------------

void VimEditor::handlePending(const Key& k)
{
    const Pending pending = m_pending;
    if (k.name == KeyName::Esc)
    {
        m_pending = Pending::None;
        m_operator = L'\0';
        m_count = 1;
        m_countActive = false;
        return;
    }

    if (pending == Pending::GG)
    {
        m_pending = Pending::None;
        if (!(k.name == KeyName::Char && k.ch == L'g'))
            return;
        const bool hasCount = m_countActive;
        const int count = hasCount ? m_count : 1;
        m_count = 1;
        m_countActive = false;
        m_cursor.line = hasCount
            ? std::min<std::size_t>(static_cast<std::size_t>(count) - 1, lineCount() - 1)
            : 0;
        m_cursor.col = 0;
        m_desiredCol = 0;
        syncCursorAndSelection();
        return;
    }

    if (k.name != KeyName::Char)
        return;

    const wchar_t arg = k.ch;
    m_pending = Pending::None;

    if (pending == Pending::Replace)
    {
        const int count = m_countActive ? m_count : 1;
        m_count = 1;
        m_countActive = false;
        beginEdit();
        std::size_t at = offset(m_cursor.line, m_cursor.col);
        for (int i = 0; i < count && at < m_text.size() && !isLineBreak(m_text[at]); ++i, ++at)
            m_text[at] = arg;
        endEdit();
        syncCursorAndSelection();
        return;
    }

    const bool forward = pending == Pending::Find || pending == Pending::Till;
    const bool till = pending == Pending::Till || pending == Pending::TillBack;
    const bool operatorPending = m_operator != L'\0';

    const int count = operatorPending
        ? m_operatorCount * (m_countActive ? m_count : 1)
        : (m_countActive ? m_count : 1);
    m_count = 1;
    m_countActive = false;

    // Resolve the target column of the count'th match on this line.
    const std::size_t lineBegin = lineStart(m_cursor.line);
    const std::size_t lineLen = lineLength(m_cursor.line);
    const std::size_t col = m_cursor.col;
    std::optional<std::size_t> found;
    if (forward)
    {
        int hits = 0;
        for (std::size_t i = col + 1; i < lineLen; ++i)
        {
            if (m_text[lineBegin + i] == arg && ++hits == count)
            {
                found = i;
                break;
            }
        }
    }
    else
    {
        // Backward: the nearest match to the left is the first one found, and
        // each further hit is one step further away.
        int hits = 0;
        for (std::size_t i = col; i > 0; --i)
        {
            if (m_text[lineBegin + i - 1] == arg && ++hits == count)
            {
                found = i - 1;
                break;
            }
        }
    }
    if (!found)
    {
        m_operator = L'\0';
        return;
    }
    m_lastFindChar = arg;
    m_lastFindForward = forward;

    std::size_t targetCol = *found;
    if (till)
        targetCol = forward ? targetCol - 1 : targetCol + 1;

    if (operatorPending)
    {
        const std::size_t from = offset(m_cursor.line, m_cursor.col);
        const std::size_t to = lineBegin + targetCol;
        std::size_t start = std::min(from, to);
        std::size_t end = std::max(from, to) + (till ? 0 : 1);
        end = std::min(end, lineBegin + lineLen);
        if (m_operator == L'd')
            operatorDelete(start, end, false);
        else if (m_operator == L'c')
            operatorChange(start, end, false);
        else
            operatorYank(start, end, false);
        m_operator = L'\0';
        return;
    }

    m_cursor.col = targetCol;
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

// ---- Normal mode ----------------------------------------------------------------

void VimEditor::handleNormal(const Key& k)
{
    // 1. Digits accumulate a count ("0" alone is the line-start motion).
    if (k.name == KeyName::Char && k.ch >= L'0' && k.ch <= L'9'
        && !(k.ch == L'0' && !m_countActive))
    {
        m_count = m_countActive ? std::min(9999, m_count * 10 + int(k.ch - L'0'))
                                : int(k.ch - L'0');
        m_countActive = true;
        return;
    }

    const int count = m_countActive ? m_count : 1;
    const bool hasCount = m_countActive;
    const auto resetCount = [this] {
        m_count = 1;
        m_countActive = false;
    };

    // 2. Operator pending: this key is its motion (or a doubled operator).
    if (m_operator != L'\0')
    {
        if (k.name == KeyName::Esc)
        {
            m_operator = L'\0';
            resetCount();
            return;
        }
        if (k.name == KeyName::Char && k.ch == m_operator)
        {
            // dd / cc / yy
            const int total = m_operatorCount * count;
            const std::size_t fromLine = m_cursor.line;
            const std::size_t toLine =
                std::min(fromLine + static_cast<std::size_t>(total) - 1, lineCount() - 1);
            const std::size_t start = lineStart(fromLine);
            const std::size_t end =
                lineStart(toLine) + lineLength(toLine) + (toLine + 1 < lineCount() ? 1 : 0);
            if (m_operator == L'd')
                operatorDelete(start, end, true);
            else if (m_operator == L'c')
                operatorChange(start, end, true);
            else
                operatorYank(start, end, true);
            m_operator = L'\0';
            resetCount();
            return;
        }

        std::size_t start = 0;
        std::size_t end = 0;
        bool linewise = false;
        if (resolveRange(start, end, linewise, k, m_operatorCount * count))
        {
            if (m_operator == L'd')
                operatorDelete(start, end, linewise);
            else if (m_operator == L'c')
                operatorChange(start, end, linewise);
            else
                operatorYank(start, end, linewise);
        }
        m_operator = L'\0';
        resetCount();
        return;
    }

    // 3. Plain commands; the count resets once one has run.
    if (k.name == KeyName::Char)
    {
        switch (k.ch)
        {
        case L'i': resetCount(); enterInsert(false); return;
        case L'a': resetCount(); enterInsert(true); return;
        case L'I':
            resetCount();
            m_cursor.col = 0;
            {
                const std::wstring line = lineText(m_cursor.line);
                std::size_t i = 0;
                while (i < line.size() && isSpaceChar(line[i]))
                    ++i;
                m_cursor.col = i < line.size() ? i : 0;
            }
            enterInsert(false);
            return;
        case L'A':
            resetCount();
            m_cursor.col = lineLength(m_cursor.line);
            enterInsert(false);
            return;
        case L'o': resetCount(); enterInsert(false, true, false); return;
        case L'O': resetCount(); enterInsert(false, false, true); return;
        case L'v':
            resetCount();
            m_mode = Mode::Visual;
            m_visualAnchor = m_cursor;
            syncCursorAndSelection();
            return;
        case L'V':
            resetCount();
            m_mode = Mode::VisualLine;
            m_visualAnchor = m_cursor;
            syncCursorAndSelection();
            return;
        case L'g':
            // Keep the count: "3gg" means line 3 and the pending handler reads it.
            m_pending = Pending::GG;
            return;
        case L'f': case L'F': case L't': case L'T':
            m_pending = k.ch == L'f' ? Pending::Find : k.ch == L'F' ? Pending::FindBack
                      : k.ch == L't' ? Pending::Till : Pending::TillBack;
            return;
        case L'r':
            m_pending = Pending::Replace;
            return;
        case L'x':
            resetCount();
            removeCharsAtCursor(static_cast<std::size_t>(count), true);
            return;
        case L'X':
        {
            resetCount();
            const std::size_t at = offset(m_cursor.line, m_cursor.col);
            const std::size_t from = at >= static_cast<std::size_t>(count) ? at - count : 0;
            beginEdit();
            m_register = m_text.substr(from, at - from);
            m_lastYankLineWise = false;
            m_text.erase(from, at - from);
            m_cursor = cursorFor(from);
            adjustCursorAfterDelete();
            endEdit();
            syncCursorAndSelection();
            return;
        }
        case L'D':
        {
            resetCount();
            const std::size_t start = offset(m_cursor.line, m_cursor.col);
            const std::size_t lineEnd = lineStart(m_cursor.line) + lineLength(m_cursor.line);
            operatorDelete(start, lineEnd, false);
            return;
        }
        case L'C':
        {
            resetCount();
            const std::size_t start = offset(m_cursor.line, m_cursor.col);
            const std::size_t lineEnd = lineStart(m_cursor.line) + lineLength(m_cursor.line);
            operatorChange(start, lineEnd, false);
            return;
        }
        case L's':
        {
            resetCount();
            const std::size_t start = offset(m_cursor.line, m_cursor.col);
            const std::size_t lineEnd = lineStart(m_cursor.line) + lineLength(m_cursor.line);
            operatorChange(start, std::min(start + count, lineEnd), false);
            return;
        }
        case L'S':
        {
            resetCount();
            const std::size_t start = lineStart(m_cursor.line);
            operatorChange(start, start + lineLength(m_cursor.line), true);
            return;
        }
        case L'p':
            resetCount();
            pasteAtCursor(true, count);
            return;
        case L'P':
            resetCount();
            pasteAtCursor(false, count);
            return;
        case L'J':
            resetCount();
            for (int i = 0; i < count; ++i)
                joinWithNext();
            return;
        case L'y': case L'd': case L'c':
            m_operator = k.ch;
            m_operatorCount = count;
            resetCount();
            return;
        case L'u':
            resetCount();
            doUndo();
            return;
        case L';':
            resetCount();
            if (m_lastFindChar)
            {
                m_pending = m_lastFindForward ? Pending::Find : Pending::FindBack;
                m_count = count;
                m_countActive = hasCount;
                handlePending(makeChar(*m_lastFindChar));
            }
            return;
        case L',':
            resetCount();
            if (m_lastFindChar)
            {
                m_pending = m_lastFindForward ? Pending::FindBack : Pending::Find;
                m_count = count;
                m_countActive = hasCount;
                handlePending(makeChar(*m_lastFindChar));
            }
            return;
        default:
            break;
        }
    }

    if (k.name == KeyName::Esc)
    {
        resetCount();
        return;
    }

    if (k.name == KeyName::Char && k.ch == L'R' && k.ctrl)
    {
        resetCount();
        doRedo();
        return;
    }

    // 4. Motions (h j k l w b e 0 ^ $ G, arrows, Home/End, Enter, Backspace).
    if (k.name == KeyName::Backspace)
    {
        resetCount();
        const Cursor target = motionApply(m_cursor, makeChar(L'h'), 1, false);
        m_cursor = target;
        m_desiredCol = m_cursor.col;
        syncCursorAndSelection();
        return;
    }
    if (k.name == KeyName::Delete)
    {
        resetCount();
        removeCharsAtCursor(static_cast<std::size_t>(count), false);
        return;
    }
    if (k.name == KeyName::Enter)
    {
        resetCount();
        if (m_cursor.line + 1 < lineCount())
        {
            m_cursor.line += 1;
            m_cursor.col = 0;
            const std::wstring line = lineText(m_cursor.line);
            std::size_t i = 0;
            while (i < line.size() && isSpaceChar(line[i]))
                ++i;
            m_cursor.col = i < line.size() ? i : 0;
            m_desiredCol = m_cursor.col;
            syncCursorAndSelection();
        }
        return;
    }

    // Vertical motions keep the desired column so j/k over a short line and back
    // returns to where the user was, as vim does.
    const bool vertical = k.name == KeyName::Up || k.name == KeyName::Down
                       || k.name == KeyName::PageUp || k.name == KeyName::PageDown
                       || (k.name == KeyName::Char && (k.ch == L'j' || k.ch == L'k'));
    const Cursor target = motionApply(m_cursor, k, count, hasCount);
    if (target.line != m_cursor.line || target.col != m_cursor.col)
    {
        m_cursor = target;
        if (!vertical)
            m_desiredCol = m_cursor.col;
        syncCursorAndSelection();
    }
    resetCount();
}

// ---- Insert mode ----------------------------------------------------------------

void VimEditor::handleInsert(const Key& k)
{
    switch (k.name)
    {
    case KeyName::Esc:
        exitInsert();
        return;
    case KeyName::Left: case KeyName::Right: case KeyName::Up: case KeyName::Down:
    case KeyName::Home: case KeyName::End:
    {
        switch (k.name)
        {
        case KeyName::Left:
            if (m_cursor.col > 0) --m_cursor.col;
            else if (m_cursor.line > 0) { --m_cursor.line; m_cursor.col = lineLength(m_cursor.line); }
            break;
        case KeyName::Right:
            if (m_cursor.col < lineLength(m_cursor.line)) ++m_cursor.col;
            else if (m_cursor.line + 1 < lineCount()) { ++m_cursor.line; m_cursor.col = 0; }
            break;
        case KeyName::Up:
            if (m_cursor.line > 0) --m_cursor.line;
            m_cursor.col = std::min(m_cursor.col, lineLength(m_cursor.line));
            break;
        case KeyName::Down:
            if (m_cursor.line + 1 < lineCount()) ++m_cursor.line;
            m_cursor.col = std::min(m_cursor.col, lineLength(m_cursor.line));
            break;
        case KeyName::Home: m_cursor.col = 0; break;
        default: m_cursor.col = lineLength(m_cursor.line); break;
        }
        m_desiredCol = m_cursor.col;
        syncCursorAndSelection();
        return;
    }
    case KeyName::Backspace:
    {
        if (m_cursor.col == 0 && m_cursor.line == 0)
            return;
        beginEdit();
        if (m_cursor.col > 0)
        {
            m_text.erase(offset(m_cursor.line, m_cursor.col) - 1, 1);
            --m_cursor.col;
        }
        else
        {
            m_text.erase(lineStart(m_cursor.line) - 1, 1);
            --m_cursor.line;
            m_cursor.col = lineLength(m_cursor.line);
        }
        m_desiredCol = m_cursor.col;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    case KeyName::Delete:
    {
        const std::size_t at = offset(m_cursor.line, m_cursor.col);
        if (at < m_text.size())
        {
            beginEdit();
            m_text.erase(at, 1);
            flushEdit();
            syncCursorAndSelection();
        }
        return;
    }
    case KeyName::Enter:
    {
        beginEdit();
        m_text.insert(offset(m_cursor.line, m_cursor.col), L"\n");
        ++m_cursor.line;
        m_cursor.col = 0;
        m_desiredCol = 0;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    case KeyName::Tab:
    {
        beginEdit();
        m_text.insert(offset(m_cursor.line, m_cursor.col), L"    ");
        m_cursor.col += 4;
        m_desiredCol = m_cursor.col;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    case KeyName::Char:
    {
        if (k.ctrl)
            return; // Ctrl chords are the host's, not text
        beginEdit();
        m_text.insert(offset(m_cursor.line, m_cursor.col), 1, k.ch);
        ++m_cursor.col;
        m_desiredCol = m_cursor.col;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    default:
        return;
    }
}

// ---- Visual mode ----------------------------------------------------------------

void VimEditor::handleVisual(const Key& k)
{
    if (k.name == KeyName::Esc)
    {
        m_mode = Mode::Normal;
        m_count = 1;
        m_countActive = false;
        syncCursorAndSelection();
        return;
    }

    if (k.name == KeyName::Char && k.ch >= L'0' && k.ch <= L'9'
        && !(k.ch == L'0' && !m_countActive))
    {
        m_count = m_countActive ? std::min(9999, m_count * 10 + int(k.ch - L'0'))
                                : int(k.ch - L'0');
        m_countActive = true;
        return;
    }

    const int count = m_countActive ? m_count : 1;
    const auto resetCount = [this] {
        m_count = 1;
        m_countActive = false;
    };

    if (k.name == KeyName::Char)
    {
        const std::size_t anchor = offset(m_visualAnchor.line, m_visualAnchor.col);
        const std::size_t head = offset(m_cursor.line, m_cursor.col);
        const bool lineWise = m_mode == Mode::VisualLine;

        std::size_t start = std::min(anchor, head);
        std::size_t end = std::max(anchor, head) + 1;
        if (lineWise)
        {
            const std::size_t fromLine = cursorFor(start).line;
            const std::size_t toLine = cursorFor(std::max(anchor, head)).line;
            start = lineStart(fromLine);
            end = lineStart(toLine) + lineLength(toLine) + (toLine + 1 < lineCount() ? 1 : 0);
        }

        switch (k.ch)
        {
        case L'v':
            resetCount();
            m_mode = m_mode == Mode::Visual ? Mode::Normal : Mode::Visual;
            m_visualAnchor = m_cursor;
            syncCursorAndSelection();
            return;
        case L'V':
            resetCount();
            m_mode = m_mode == Mode::VisualLine ? Mode::Normal : Mode::VisualLine;
            m_visualAnchor = m_cursor;
            syncCursorAndSelection();
            return;
        case L'd': case L'x':
            resetCount();
            if (end > start)
                operatorDelete(start, end, lineWise);
            m_mode = Mode::Normal;
            syncCursorAndSelection();
            return;
        case L'y':
            resetCount();
            operatorYank(start, end, lineWise);
            m_mode = Mode::Normal;
            syncCursorAndSelection();
            return;
        case L'c':
            resetCount();
            operatorChange(start, end, lineWise);
            m_mode = Mode::Insert;
            syncCursorAndSelection();
            return;
        default:
            break;
        }
    }

    // Any other key is a motion that extends the visual range.
    const Cursor target = motionApply(m_cursor, k, count, m_countActive);
    if (target.line != m_cursor.line || target.col != m_cursor.col)
    {
        m_cursor = target;
        m_desiredCol = m_cursor.col;
        syncCursorAndSelection();
    }
    resetCount();
}

// ---- dispatch -------------------------------------------------------------------

void VimEditor::key(const Key& k)
{
    if (m_pending != Pending::None)
    {
        handlePending(k);
        return;
    }
    switch (m_mode)
    {
    case Mode::Normal: handleNormal(k); break;
    case Mode::Insert: handleInsert(k); break;
    case Mode::Visual:
    case Mode::VisualLine: handleVisual(k); break;
    }
}

} // namespace vimcore
