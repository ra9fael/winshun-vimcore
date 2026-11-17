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
#include <regex>

#include <algorithm>
#include <cwchar>
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
    const bool pastEndOk = m_mode == Mode::Insert || m_mode == Mode::Replace;
    const std::size_t max = pastEndOk ? lineLength(m_cursor.line)
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
    if (m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock)
    {
        selection.active = true;
        selection.lineWise = m_mode == Mode::VisualLine;
        selection.blockWise = m_mode == Mode::VisualBlock;
        selection.anchor = m_visualAnchor;
        selection.head = m_cursor;
    }
    m_doc.setSelection(selection);
    switch (m_mode)
    {
    case Mode::Normal:     m_doc.setStatus(L"-- NORMAL --"); break;
    case Mode::Insert:     m_doc.setStatus(L"-- INSERT --"); break;
    case Mode::Replace:    m_doc.setStatus(L"-- REPLACE --"); break;
    case Mode::Visual:     m_doc.setStatus(L"-- VISUAL --"); break;
    case Mode::VisualLine: m_doc.setStatus(L"-- VISUAL LINE --"); break;
    case Mode::VisualBlock: m_doc.setStatus(L"-- VISUAL BLOCK --"); break;
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
    if (m_editing)
        m_batchClosed = true;
    m_editing = false;
}

// ---- '.' replay -------------------------------------------------------------

void VimEditor::commitReplay()
{
    if (m_replayBuffer.empty())
        return;
    m_lastChange = m_replayBuffer;
    m_replayBuffer.clear();
}

// ---- mode transitions ----------------------------------------------------------

void VimEditor::enterInsert(bool afterCursor, bool openLineBelow, bool openLineAbove, int repeat)
{
    beginEdit();
    m_sessionRepeat = repeat < 1 ? 1 : repeat;
    m_sessionTyped.clear();
    m_sessionOpenLine = openLineBelow || openLineAbove;
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
    if (m_blockInsert)
    {
        const auto current = lineText(m_blockFirst);
        const auto prefix = m_blockOriginalLine.substr(0, m_blockColumn);
        const auto suffix = m_blockOriginalLine.substr(m_blockColumn);
        const bool insertionOnly = current.size() >= m_blockOriginalLine.size()
            && current.starts_with(prefix) && current.ends_with(suffix);
        const auto inserted = insertionOnly
            ? current.substr(m_blockColumn, current.size() - m_blockOriginalLine.size()) : std::wstring();
        if (!inserted.empty() && lineCount() == static_cast<std::size_t>(std::count(m_undo.back().text.begin(), m_undo.back().text.end(), L'\n')) + 1)
        {
            for (std::size_t line = m_blockLast; line > m_blockFirst; --line)
            {
                const auto length = lineLength(line);
                if (length < m_blockColumn && !m_blockAppend)
                    continue;
                if (length < m_blockColumn)
                    m_text.insert(lineStart(line) + length, m_blockColumn - length, L' ');
                m_text.insert(lineStart(line) + m_blockColumn, inserted);
            }
        }
        m_blockInsert = false;
        flushEdit();
    }
    // {count}i repeats the typed text count times; {count}o opens count lines.
    // Both happen before the batch closes so the whole thing stays one undo.
    const std::wstring typed = m_sessionTyped;
    const int repeat = m_sessionRepeat;
    const bool openLine = m_sessionOpenLine;
    m_sessionTyped.clear();
    m_sessionRepeat = 1;
    m_sessionOpenLine = false;

    if (repeat > 1 && !typed.empty())
    {
        if (openLine)
        {
            for (int i = 1; i < repeat; ++i)
            {
                const std::size_t at = lineStart(m_cursor.line) + lineLength(m_cursor.line);
                m_text.insert(at, L"\n" + typed);
                ++m_cursor.line;
                m_cursor.col = typed.size();
            }
        }
        else
        {
            for (int i = 1; i < repeat; ++i)
            {
                for (const wchar_t ch : typed)
                {
                    m_text.insert(offset(m_cursor.line, m_cursor.col), 1, ch);
                    ++m_cursor.col;
                }
            }
        }
    }

    // Vim leaves the cursor on the last character the insert session produced.
    // Nothing typed means there is nothing to step back onto. The pre-session
    // text is the snapshot beginEdit() took, which is exactly "did the session
    // change anything" — native (IME/widget) typing counts too.
    const std::wstring preSession = m_undo.empty() ? std::wstring() : m_undo.back().text;
    const bool typedAnything = m_text != preSession;
    endEdit();
    if (typedAnything && m_cursor.col > 0)
        --m_cursor.col;
    m_mode = Mode::Normal;
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

// ---- Replace mode ----------------------------------------------------------------

void VimEditor::enterReplace(int repeat)
{
    beginEdit();
    m_sessionRepeat = repeat < 1 ? 1 : repeat;
    m_sessionTyped.clear();
    m_replaced.clear();
    m_mode = Mode::Replace;
    m_desiredCol = m_cursor.col;
    flushEdit();
    syncCursorAndSelection();
}

void VimEditor::overwriteAtCursor(wchar_t ch)
{
    const std::size_t at = offset(m_cursor.line, m_cursor.col);
    if (at < m_text.size() && !isLineBreak(m_text[at]))
    {
        m_replaced.push_back({at, m_text[at]});
        m_text[at] = ch;
    }
    else
        m_text.insert(at, 1, ch); // past end of line: append, like vim
    ++m_cursor.col;
    m_sessionTyped += ch;
    m_desiredCol = m_cursor.col;
}

void VimEditor::exitReplace()
{
    // {count}R repeats the typed text count times before the batch closes.
    const std::wstring typed = m_sessionTyped;
    const int repeat = m_sessionRepeat;
    m_sessionTyped.clear();
    m_sessionRepeat = 1;
    if (repeat > 1 && !typed.empty())
    {
        for (int i = 1; i < repeat; ++i)
        {
            for (const wchar_t ch : typed)
                overwriteAtCursor(ch);
        }
    }
    m_replaced.clear();

    const std::wstring preSession = m_undo.empty() ? std::wstring() : m_undo.back().text;
    const bool typedAnything = m_text != preSession;
    endEdit();
    if (typedAnything && m_cursor.col > 0)
        --m_cursor.col;
    m_mode = Mode::Normal;
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
}

void VimEditor::handleReplace(const Key& k)
{
    switch (k.name)
    {
    case KeyName::Esc:
        exitReplace();
        return;
    case KeyName::Left: case KeyName::Right: case KeyName::Up: case KeyName::Down:
    case KeyName::Home: case KeyName::End:
        // Same caret rules as insert mode.
        {
            Key insertKey = k;
            handleInsertMotion(insertKey);
        }
        return;
    case KeyName::Backspace:
    {
        if (!m_replaced.empty())
        {
            // Restore the character this session overwrote, newest first.
            const auto last = m_replaced.back();
            m_replaced.pop_back();
            m_text[last.first] = last.second;
            m_cursor = cursorFor(last.first);
            if (!m_sessionTyped.empty())
                m_sessionTyped.pop_back();
        }
        else if (m_cursor.col > 0)
        {
            --m_cursor.col; // nothing of ours to restore: just move left
        }
        m_desiredCol = m_cursor.col;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    case KeyName::Enter:
    {
        m_text.insert(offset(m_cursor.line, m_cursor.col), L"\n");
        ++m_cursor.line;
        m_cursor.col = 0;
        m_sessionTyped += L'\n';
        m_desiredCol = 0;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    case KeyName::Delete:
    {
        const std::size_t at = offset(m_cursor.line, m_cursor.col);
        if (at < m_text.size())
        {
            m_text.erase(at, 1);
            flushEdit();
            syncCursorAndSelection();
        }
        return;
    }
    case KeyName::Tab:
    {
        m_text.insert(offset(m_cursor.line, m_cursor.col), L"    ");
        m_cursor.col += 4;
        m_sessionTyped += L"    ";
        m_desiredCol = m_cursor.col;
        flushEdit();
        syncCursorAndSelection();
        return;
    }
    case KeyName::Char:
        if (!k.ctrl)
        {
            overwriteAtCursor(k.ch);
            flushEdit();
            syncCursorAndSelection();
        }
        return;
    default:
        return;
    }
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

// ---- helpers for the extended commands -------------------------------------------

bool VimEditor::adjustNumber(int delta, int count)
{
    const std::size_t ls = lineStart(m_cursor.line);
    const std::size_t len = lineLength(m_cursor.line);
    if (len == 0)
        return false;
    const std::wstring line = m_text.substr(ls, len);
    std::size_t col = std::min(m_cursor.col, len - 1);

    std::size_t digits = std::size_t(-1); // index of the first digit
    if (line[col] == L'-' && col + 1 < len && iswdigit(line[col + 1]))
        digits = col + 1; // cursor sits on the sign of the number
    else
    {
        for (std::size_t i = col; i < len; ++i)
        {
            if (iswdigit(line[i]))
            {
                digits = i;
                break;
            }
        }
    }
    if (digits == std::size_t(-1))
        return false;

    // A '-' belongs to this number only when it is not the tail of another.
    bool negative = false;
    std::size_t start = digits;
    if (digits > 0 && line[digits - 1] == L'-'
        && !(digits >= 2 && iswdigit(line[digits - 2])))
    {
        negative = true;
        start = digits - 1;
    }

    std::size_t end = digits;
    while (end < len && iswdigit(line[end]))
        ++end;
    long long value = 0;
    for (std::size_t i = digits; i < end; ++i)
        value = value * 10 + (line[i] - L'0');
    const std::size_t width = end - digits;
    const bool padded = width > 1 && line[digits] == L'0';

    const long long result =
        (negative ? -value : value) + static_cast<long long>(delta) * count;
    const bool newNegative = result < 0;
    const unsigned long long magnitude = newNegative
        ? static_cast<unsigned long long>(-result)
        : static_cast<unsigned long long>(result);
    std::wstring digitsText = std::to_wstring(magnitude);
    if (padded && digitsText.size() < width)
        digitsText.insert(0, width - digitsText.size(), L'0');
    const std::wstring number = (newNegative ? std::wstring(L"-") : std::wstring()) + digitsText;

    beginEdit();
    m_text.replace(ls + start, end - start, number);
    endEdit();
    m_cursor.col = start + number.size() - 1; // vim lands on the last digit
    m_desiredCol = m_cursor.col;
    syncCursorAndSelection();
    return true;
}

std::optional<std::size_t> VimEditor::bracketMatchCol(std::size_t line, std::size_t col) const
{
    // Same-line bracket matching: the rename buffer is line-oriented, so this
    // covers the realistic cases without a multi-line scan.
    const std::wstring text = lineText(line);
    if (col >= text.size())
        return std::nullopt;
    static const wchar_t* open = L"([{";
    static const wchar_t* close = L")]}";
    const wchar_t ch = text[col];
    const wchar_t* opener = std::wcschr(open, ch);
    const wchar_t* closer = std::wcschr(close, ch);
    if (!opener && !closer)
        return std::nullopt;
    if (opener)
    {
        const wchar_t want = close[opener - open];
        int depth = 0;
        for (std::size_t i = col; i < text.size(); ++i)
        {
            if (text[i] == ch)
                ++depth;
            else if (text[i] == want && --depth == 0)
                return i;
        }
        return std::nullopt;
    }
    const wchar_t want = open[closer - close];
    int depth = 0;
    for (std::size_t i = col + 1; i-- > 0;)
    {
        if (text[i] == ch)
            ++depth;
        else if (text[i] == want && --depth == 0)
            return i;
    }
    return std::nullopt;
}

bool VimEditor::wordRange(std::size_t& start, std::size_t& end, bool around) const
{
    const std::size_t ls = lineStart(m_cursor.line);
    const std::size_t len = lineLength(m_cursor.line);
    if (len == 0)
        return false;
    const std::size_t col = std::min(m_cursor.col, len - 1);
    const auto classify = [](wchar_t ch) {
        if (isSpaceChar(ch))
            return 0;
        return isWordChar(ch) ? 1 : 2; // word / punctuation
    };
    const int cls = classify(m_text[ls + col]);
    std::size_t b = col;
    while (b > 0 && classify(m_text[ls + b - 1]) == cls)
        --b;
    std::size_t e = col;
    while (e < len && classify(m_text[ls + e]) == cls)
        ++e;
    if (around)
    {
        // "aw" prefers the whitespace after the word; at the end of the line it
        // takes the run before it instead.
        std::size_t t = e;
        while (t < len && isSpaceChar(m_text[ls + t]))
            ++t;
        if (t > e)
            e = t;
        else
        {
            while (b > 0 && isSpaceChar(m_text[ls + b - 1]))
                --b;
        }
    }
    start = ls + b;
    end = ls + e;
    return true;
}

void VimEditor::applyCase(std::size_t start, std::size_t end, int op)
{
    if (end <= start)
        return;
    beginEdit();
    for (std::size_t i = start; i < end && i < m_text.size(); ++i)
    {
        wchar_t ch = m_text[i];
        if (op == 0)
            ch = static_cast<wchar_t>(towlower(ch));
        else if (op == 1)
            ch = static_cast<wchar_t>(towupper(ch));
        else
            ch = iswupper(ch) ? static_cast<wchar_t>(towlower(ch))
                              : static_cast<wchar_t>(towupper(ch));
        m_text[i] = ch;
    }
    endEdit();
}

void VimEditor::visualPaste()
{
    if (m_register.empty())
        return;
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
    if (end > m_text.size())
        end = m_text.size();

    const std::wstring taken = m_text.substr(start, end - start);
    std::wstring block = m_register;
    if (lineWise)
    {
        // A linewise selection is replaced by whole lines, newline included.
        if (!block.empty() && block.back() != L'\n')
            block += L'\n';
    }
    beginEdit();
    m_text.replace(start, end - start, block);
    m_register = taken; // vim swaps: the replaced text becomes the register
    m_lastYankLineWise = lineWise;
    if (lineWise)
    {
        m_cursor = cursorFor(start);
        m_cursor.col = firstNonBlankCol(m_cursor.line);
    }
    else
        m_cursor = cursorFor(block.empty() ? start : start + block.size() - 1);
    m_desiredCol = m_cursor.col;
    m_mode = Mode::Normal;
    endEdit();
    syncCursorAndSelection();
}

void VimEditor::visualJoin()
{
    const std::size_t anchor = offset(m_visualAnchor.line, m_visualAnchor.col);
    const std::size_t head = offset(m_cursor.line, m_cursor.col);
    const std::size_t fromLine = std::min(cursorFor(anchor).line, cursorFor(head).line);
    const std::size_t toLine = std::max(cursorFor(anchor).line, cursorFor(head).line);
    const std::size_t joins = toLine - fromLine;
    m_cursor = {fromLine, m_cursor.col};
    for (std::size_t i = 0; i < joins; ++i)
        joinWithNext();
    m_mode = Mode::Normal;
    syncCursorAndSelection();
}

// ---- undo / redo ----------------------------------------------------------------

void VimEditor::doUndo()
{
    if (m_undo.empty())
        return;
    m_suppressRecord = true;
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
    m_suppressRecord = true;
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

    const std::wstring oldText = m_text;
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

        // Remember what the host typed so {count}i repeats it and '.' can
        // replay the session.
        if (!m_replaying && !m_suppressRecord)
        {
            std::size_t prefix = 0;
            while (prefix < oldText.size() && prefix < m_text.size()
                   && oldText[prefix] == m_text[prefix])
                ++prefix;
            std::size_t suffix = 0;
            while (suffix < oldText.size() - prefix && suffix < m_text.size() - prefix
                   && oldText[oldText.size() - 1 - suffix]
                      == m_text[m_text.size() - 1 - suffix])
                ++suffix;
            const std::wstring typed =
                m_text.substr(prefix, m_text.size() - suffix - prefix);
            m_sessionTyped += typed;
            for (const wchar_t ch : typed)
                m_replayBuffer.push_back(makeChar(ch));
        }
    }
    else
    {
        // Outside an open batch there is no pre-change snapshot to infer; the
        // host remains the source of truth, and future vim edits are undoable.
        // A half-typed '.' replay is stale now, so drop it.
        m_replayBuffer.clear();
    }

    // The user typed in the widget; vim key state cannot carry over.
    m_operator = L'\0';
    m_object = Object::None;
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
    const std::size_t rows = m_options.viewportRows < 2 ? 2 : m_options.viewportRows;

    switch (k.name)
    {
    case KeyName::Left:
        // h and <Left> stay on the line; vim's whichwrap does not include them.
        if (c.col > 0)
            c.col -= std::min<std::size_t>(static_cast<std::size_t>(n), c.col);
        break;
    case KeyName::Backspace:
        // <BS> is the one backward motion that wraps (whichwrap 'b').
        for (int i = 0; i < n; ++i)
        {
            if (c.col > 0) --c.col;
            else if (c.line > 0) { --c.line; c.col = lastCharCol(c.line); }
        }
        break;
    case KeyName::Right:
        if (c.col < lastCharCol(c.line))
            c.col += std::min<std::size_t>(static_cast<std::size_t>(n),
                                           lastCharCol(c.line) - c.col);
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
        // <End> is $: count-1 lines down, then the last character.
        for (int i = 1; i < n && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = lastCharCol(c.line);
        break;
    case KeyName::PageUp:
        for (std::size_t i = 0; i < rows * static_cast<std::size_t>(n) && c.line > 0; ++i)
            --c.line;
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
        break;
    case KeyName::PageDown:
        for (std::size_t i = 0;
             i < rows * static_cast<std::size_t>(n) && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
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

    // Ctrl paging: D/U are half a page, F/B a full page.
    if (k.ctrl)
    {
        const std::size_t half = std::max<std::size_t>(1, rows / 2); // at least one line
        std::size_t step = 0;
        bool down = false;
        switch (k.ch)
        {
        case L'd': step = half * static_cast<std::size_t>(n); down = true; break;
        case L'u': step = half * static_cast<std::size_t>(n); break;
        case L'f': step = rows * static_cast<std::size_t>(n); down = true; break;
        case L'b': step = rows * static_cast<std::size_t>(n); break;
        default: return c;
        }
        for (std::size_t i = 0; i < step; ++i)
        {
            if (down && c.line + 1 < lineCount())
                ++c.line;
            else if (!down && c.line > 0)
                --c.line;
        }
        c.col = std::min(std::max(c.col, m_desiredCol), lastCharCol(c.line));
        if (c.col > lastCharCol(c.line)) c.col = lastCharCol(c.line);
        return c;
    }

    switch (k.ch)
    {
    case L'h':
        if (c.col > 0)
            c.col -= std::min<std::size_t>(static_cast<std::size_t>(n), c.col);
        break;
    case L' ':
        // <Space> is the one horizontal motion that wraps (whichwrap 's').
        for (int i = 0; i < n; ++i)
        {
            if (c.col < lastCharCol(c.line)) ++c.col;
            else if (c.line + 1 < lineCount()) { ++c.line; c.col = 0; }
            else
            {
                m_motionRanOut = true;
                break;
            }
        }
        break;
    case L'l':
        if (c.col < lastCharCol(c.line))
            c.col += std::min<std::size_t>(static_cast<std::size_t>(n),
                                           lastCharCol(c.line) - c.col);
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
        c.col = firstNonBlankCol(c.line);
        break;
    case L'$':
        for (int i = 1; i < n && c.line + 1 < lineCount(); ++i)
            ++c.line;
        c.col = lastCharCol(c.line);
        break;
    case L'%':
    {
        const std::optional<std::size_t> match = bracketMatchCol(c.line, c.col);
        if (match)
            c.col = *match;
        break;
    }
    case L'G':
        if (hasCount)
            c.line = std::min<std::size_t>(static_cast<std::size_t>(n) - 1, lineCount() - 1);
        else
            c.line = lineCount() - 1;
        c.col = firstNonBlankCol(c.line);
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

std::size_t VimEditor::firstNonBlankCol(std::size_t line) const
{
    const std::wstring text = lineText(line);
    std::size_t i = 0;
    while (i < text.size() && isSpaceChar(text[i]))
        ++i;
    return i < text.size() ? i : 0;
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

    if (motion.ch == L'%' && !bracketMatchCol(m_cursor.line, m_cursor.col))
        return false; // no match on this line: the operator is cancelled

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
                        || motion.ch == L'F' || motion.ch == L'%';
    const bool forwardMotion = motion.ch == L'l' || motion.ch == L'w' || motion.ch == L'e'
                            || motion.ch == L'$' || motion.ch == L'f' || motion.ch == L't'
                            || motion.ch == L' ' || motion.ch == L'%';
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
        if (pending == Pending::VisualReplace)
        {
            // "vllr<Esc>" abandons the replace and leaves Visual mode, leaving
            // the text untouched (vim does the same).
            m_mode = Mode::Normal;
            clampCursor();
            syncCursorAndSelection();
        }
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
        m_cursor.col = firstNonBlankCol(m_cursor.line);
        m_desiredCol = m_cursor.col;
        syncCursorAndSelection();
        return;
    }

    if (k.name != KeyName::Char && k.name != KeyName::Enter)
        return;

    const wchar_t arg = k.name == KeyName::Enter ? L'\n' : k.ch;
    m_pending = Pending::None;

    if (pending == Pending::Replace)
    {
        const int count = m_countActive ? m_count : 1;
        m_count = 1;
        m_countActive = false;
        const std::size_t lineLen = lineLength(m_cursor.line);
        if (lineLen == 0 || m_cursor.col + static_cast<std::size_t>(count) > lineLen)
            return; // vim fails a replace that runs off the line: no change
        beginEdit();
        const std::size_t at = offset(m_cursor.line, m_cursor.col);
        if (arg == L'\n')
        {
            m_text.replace(at, 1, L"\n"); // r<CR> breaks the line
            m_cursor.line += 1;
            m_cursor.col = 0;
        }
        else
        {
            for (int i = 0; i < count; ++i)
                m_text[at + static_cast<std::size_t>(i)] = arg;
            m_cursor.col += static_cast<std::size_t>(count) - 1; // land on the last one
        }
        m_desiredCol = m_cursor.col;
        endEdit();
        syncCursorAndSelection();
        return;
    }

    if (pending == Pending::VisualReplace)
    {
        m_count = 1;
        m_countActive = false;
        const auto finish = [this] {
            m_mode = Mode::Normal;
            clampCursor();
            syncCursorAndSelection();
        };
        if (arg == L'\n')
        {
            finish(); // r<CR> is not a usable replacement for a visual range
            return;
        }
        if (m_mode == Mode::VisualBlock)
        {
            const auto first = std::min(m_visualAnchor.line, m_cursor.line);
            const auto last = std::max(m_visualAnchor.line, m_cursor.line);
            const auto left = std::min(m_visualAnchor.col, m_cursor.col);
            const auto right = std::max(m_visualAnchor.col, m_cursor.col);
            beginEdit();
            for (auto line = first; line <= last; ++line)
            {
                // Only characters that are actually there: a short row is left
                // short instead of being space-padded, exactly like vim.
                const std::size_t stop = std::min(right + 1, lineLength(line));
                for (std::size_t col = left; col < stop; ++col)
                    m_text[lineStart(line) + col] = arg;
            }
            m_cursor = {first, left};
            endEdit();
            finish();
            return;
        }
        // Character- and line-wise: every character of the range is replaced.
        const std::size_t anchor = offset(m_visualAnchor.line, m_visualAnchor.col);
        const std::size_t head = offset(m_cursor.line, m_cursor.col);
        std::size_t start = std::min(anchor, head);
        std::size_t end = std::max(anchor, head) + 1;
        if (m_mode == Mode::VisualLine)
        {
            const std::size_t fromLine = cursorFor(start).line;
            const std::size_t toLine = cursorFor(std::max(anchor, head)).line;
            start = lineStart(fromLine);
            end = lineStart(toLine) + lineLength(toLine) + (toLine + 1 < lineCount() ? 1 : 0);
        }
        beginEdit();
        for (std::size_t i = start; i < end && i < m_text.size(); ++i)
            if (m_text[i] != L'\n')
                m_text[i] = arg;
        m_cursor = cursorFor(start);
        m_desiredCol = m_cursor.col;
        endEdit();
        finish();
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
    m_lastFindTill = till;

    std::size_t targetCol = *found;
    if (till)
        targetCol = forward ? targetCol - 1 : targetCol + 1;

    if (operatorPending)
    {
        const std::size_t from = offset(m_cursor.line, m_cursor.col);
        const std::size_t to = lineBegin + targetCol;
        // Operator + find follows vim's own rules, verified against nvim:
        // a forward motion includes the character it lands on, a backward one
        // stops before the cursor character, and a motion that does not move
        // (dT, with the target right next to the cursor) cancels.
        std::size_t start = 0;
        std::size_t end = 0;
        if (to > from)
        {
            start = from;
            end = to + 1;
        }
        else if (to < from)
        {
            start = to;
            end = from;
        }
        else
        {
            m_operator = L'\0';
            return;
        }
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

    // 2. Ctrl chords the core owns: numeric edit, paging, redo. Others are the
    //    host's (Ctrl+S/Z/Y...) and are ignored here.
    if (k.name == KeyName::Char && k.ctrl)
    {
        m_operator = L'\0';
        m_object = Object::None;
        switch (k.ch)
        {
        case L'a': resetCount(); adjustNumber(1, count); return;
        case L'x': resetCount(); adjustNumber(-1, count); return;
        case L'R':
            resetCount();
            for (int i = 0; i < count; ++i)
                doRedo();
            return;
        case L'd': case L'u': case L'f': case L'b':
        {
            resetCount();
            const Cursor target = motionApply(m_cursor, k, count, hasCount);
            if (target.line != m_cursor.line || target.col != m_cursor.col)
            {
                m_cursor = target; // vertical: desired column is preserved
                syncCursorAndSelection();
            }
            return;
        }
        default:
            resetCount();
            return;
        }
    }

    // 3. Operator pending: this key is its motion (or a doubled operator).
    if (m_operator != L'\0')
    {
        if (k.name == KeyName::Esc)
        {
            m_operator = L'\0';
            resetCount();
            return;
        }
        if (k.name == KeyName::Char && (k.ch == L'i' || k.ch == L'a'))
        {
            // iw/aw: wait for the object character.
            m_object = k.ch == L'i' ? Object::InnerWord : Object::AWord;
            return;
        }
        // Operator + find (df, dt] dF, dT, d; d,) — the character to search for
        // arrives on the next key, so hand it to the pending handler with the
        // operator and count still in place.
        if (k.name == KeyName::Char && !k.ctrl
            && (k.ch == L'f' || k.ch == L'F' || k.ch == L't' || k.ch == L'T'))
        {
            m_pending = k.ch == L'f' ? Pending::Find : k.ch == L'F' ? Pending::FindBack
                      : k.ch == L't' ? Pending::Till : Pending::TillBack;
            return;
        }
        if (k.name == KeyName::Char && !k.ctrl && (k.ch == L';' || k.ch == L','))
        {
            if (!m_lastFindChar)
            {
                m_operator = L'\0';
                resetCount();
                return;
            }
            const bool sameDirection = k.ch == L';';
            const bool back = sameDirection ? !m_lastFindForward : m_lastFindForward;
            m_pending = m_lastFindTill ? (back ? Pending::TillBack : Pending::Till)
                                       : (back ? Pending::FindBack : Pending::Find);
            handlePending(makeChar(*m_lastFindChar));
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

    // 4. Plain commands; the count resets once one has run.
    if (k.name == KeyName::Char)
    {
        switch (k.ch)
        {
        case L'i': resetCount(); enterInsert(false, false, false, count); return;
        case L'a': resetCount(); enterInsert(true, false, false, count); return;
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
            enterInsert(false, false, false, count);
            return;
        case L'A':
            resetCount();
            m_cursor.col = lineLength(m_cursor.line);
            enterInsert(false, false, false, count);
            return;
        case L'o': resetCount(); enterInsert(false, true, false, count); return;
        case L'O': resetCount(); enterInsert(false, false, true, count); return;
        case L'R':
            resetCount();
            enterReplace(count);
            return;
        case L'.':
        {
            resetCount();
            if (m_lastChange.empty())
                return;
            m_suppressRecord = true; // the replay is not a new "last change"
            m_replaying = true;
            for (int i = 0; i < count; ++i)
            {
                for (const Key& rk : m_lastChange)
                    key(rk);
            }
            m_replaying = false;
            m_replayBuffer.clear();
            return;
        }
        case L'%':
        {
            resetCount();
            const std::optional<std::size_t> match =
                bracketMatchCol(m_cursor.line, m_cursor.col);
            if (match)
            {
                m_cursor.col = *match;
                m_desiredCol = m_cursor.col;
                syncCursorAndSelection();
            }
            return;
        }
        case L'~':
        {
            resetCount();
            const std::size_t lineLen = lineLength(m_cursor.line);
            if (lineLen == 0)
                return;
            const std::size_t at = offset(m_cursor.line, m_cursor.col);
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(count),
                                                        lineLen - m_cursor.col);
            applyCase(at, at + n, 2);
            m_cursor.col = std::min(m_cursor.col + n, lastCharCol(m_cursor.line));
            m_desiredCol = m_cursor.col;
            syncCursorAndSelection();
            return;
        }
        case L'Y':
        {
            resetCount();
            const std::size_t fromLine = m_cursor.line;
            const std::size_t toLine =
                std::min(fromLine + static_cast<std::size_t>(count) - 1, lineCount() - 1);
            const std::size_t start = lineStart(fromLine);
            const std::size_t end =
                lineStart(toLine) + lineLength(toLine) + (toLine + 1 < lineCount() ? 1 : 0);
            operatorYank(start, end, true);
            return;
        }
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
            for (int i = 0; i < count; ++i)
                doUndo();
            return;
        case L';':
            resetCount();
            if (m_lastFindChar)
            {
                m_pending = m_lastFindTill
                    ? (m_lastFindForward ? Pending::Till : Pending::TillBack)
                    : (m_lastFindForward ? Pending::Find : Pending::FindBack);
                m_count = count;
                m_countActive = hasCount;
                handlePending(makeChar(*m_lastFindChar));
            }
            return;
        case L',':
            resetCount();
            if (m_lastFindChar)
            {
                m_pending = m_lastFindTill
                    ? (m_lastFindForward ? Pending::TillBack : Pending::Till)
                    : (m_lastFindForward ? Pending::FindBack : Pending::Find);
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

    // 5. Motions (h j k l w b e 0 ^ $ G %, space, arrows, Home/End, Enter,
    //    Backspace).
    if (k.name == KeyName::Backspace)
    {
        resetCount();
        const Cursor target = motionApply(m_cursor, k, count, hasCount);
        m_cursor = target;
        m_desiredCol = m_cursor.col;
        syncCursorAndSelection();
        return;
    }
    if (k.name == KeyName::Delete)
    {
        resetCount();
        removeCharsAtCursor(static_cast<std::size_t>(count), true); // <Del> == x
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

void VimEditor::handleInsertMotion(const Key& k)
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
}

void VimEditor::handleInsert(const Key& k)
{
    switch (k.name)
    {
    case KeyName::Esc:
        exitInsert();
        return;
    case KeyName::Left: case KeyName::Right: case KeyName::Up: case KeyName::Down:
    case KeyName::Home: case KeyName::End:
        handleInsertMotion(k);
        return;
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
        if (!m_sessionTyped.empty())
            m_sessionTyped.pop_back();
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
        m_sessionTyped += L'\n';
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
        m_sessionTyped += L"    ";
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
        m_sessionTyped += k.ch;
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

void VimEditor::handleBlock(const Key& k)
{
    const auto first = std::min(m_visualAnchor.line, m_cursor.line);
    const auto last = std::max(m_visualAnchor.line, m_cursor.line);
    const auto left = std::min(m_visualAnchor.col, m_cursor.col);
    const auto right = std::max(m_visualAnchor.col, m_cursor.col);
    if (k.name == KeyName::Char && !k.ctrl && (k.ch == L'I' || k.ch == L'A'))
    {
        m_blockFirst = first;
        m_blockLast = last;
        m_blockColumn = k.ch == L'I' ? left : right + 1;
        m_blockAppend = k.ch == L'A';
        beginEdit();
        m_cursor = {first, m_blockColumn};
        if (lineLength(first) < m_blockColumn)
            m_text.insert(lineStart(first) + lineLength(first), m_blockColumn - lineLength(first), L' ');
        m_blockOriginalLine = lineText(first);
        enterInsert(false);
        m_blockInsert = true;
        return;
    }
    if (k.name == KeyName::Char && !k.ctrl && k.ch == L'r')
    {
        // Block-wise r: replace every character inside the rectangle. Nothing is
        // applied yet; the replacement character is the next key.
        m_pending = Pending::VisualReplace;
        return;
    }
    if (k.name == KeyName::Char && !k.ctrl && (k.ch == L'd' || k.ch == L'x' || k.ch == L'y'))
    {
        beginEdit();
        m_register.clear();
        for (auto line = first; line <= last; ++line)
        {
            if (line > first) m_register += L'\n';
            if (left < lineLength(line))
                m_register += lineText(line).substr(left, right - left + 1);
        }
        m_lastYankLineWise = false;
        if (k.ch != L'y')
            for (auto line = last + 1; line-- > first;)
                if (left < lineLength(line))
                    m_text.erase(lineStart(line) + left, std::min(right + 1, lineLength(line)) - left);
        m_mode = Mode::Normal;
        m_cursor = {first, left};
        clampCursor();
        endEdit();
        syncCursorAndSelection();
        return;
    }
    if (k.name == KeyName::Esc || (k.name == KeyName::Char && (k.ch == L'v' || k.ch == L'V' || k.ch == L'o')))
    {
        handleVisual(k);
        return;
    }
    if (k.name == KeyName::Char && k.ch >= L'1' && k.ch <= L'9')
    {
        m_count = m_countActive ? std::min(9999, m_count * 10 + int(k.ch - L'0')) : int(k.ch - L'0');
        m_countActive = true;
        return;
    }
    auto target = motionApply(m_cursor, k, m_countActive ? m_count : 1, m_countActive);
    if (target.line != m_cursor.line)
        target.col = m_cursor.col;
    m_cursor = target;
    m_desiredCol = target.col;
    m_count = 1;
    m_countActive = false;
    syncCursorAndSelection();
}

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
            // Switching v<->V keeps the anchor, so the same range stays visible.
            m_mode = m_mode == Mode::Visual ? Mode::Normal : Mode::Visual;
            syncCursorAndSelection();
            return;
        case L'V':
            resetCount();
            m_mode = m_mode == Mode::VisualLine ? Mode::Normal : Mode::VisualLine;
            syncCursorAndSelection();
            return;
        case L'o':
            resetCount();
            std::swap(m_visualAnchor, m_cursor); // extend from the other end
            m_desiredCol = m_cursor.col;
            syncCursorAndSelection();
            return;
        case L'i': case L'a':
            resetCount();
            m_object = k.ch == L'i' ? Object::InnerWord : Object::AWord;
            return;
        case L'r':
            // v/V + r: the replacement character arrives on the next key, which
            // handlePending() applies over the whole range as one undo step. The
            // range is read back from the anchor and the head, neither of which
            // moves in between.
            m_count = count;
            m_countActive = count > 1;
            m_pending = Pending::VisualReplace;
            return;
        case L'%':
        {
            resetCount();
            const std::optional<std::size_t> match =
                bracketMatchCol(m_cursor.line, m_cursor.col);
            if (match)
            {
                m_cursor.col = *match;
                m_desiredCol = m_cursor.col;
                syncCursorAndSelection();
            }
            return;
        }
        case L'u': case L'U': case L'~':
            resetCount();
            applyCase(start, end, k.ch == L'u' ? 0 : k.ch == L'U' ? 1 : 2);
            m_mode = Mode::Normal;
            m_cursor = cursorFor(start);
            m_desiredCol = m_cursor.col;
            syncCursorAndSelection();
            return;
        case L'p':
            resetCount();
            visualPaste();
            return;
        case L'J':
            resetCount();
            visualJoin();
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
    if (m_replaying)
    {
        dispatch(k);
        return;
    }

    m_batchClosed = false;
    m_replayBuffer.push_back(k);
    const std::wstring textBefore = m_text;
    const Mode modeBefore = m_mode;
    dispatch(k);

    if (m_batchClosed && !m_suppressRecord)
    {
        // A visual-range change has no meaningful Normal-mode replay in this
        // core (the range would have to be remembered); keep '.' harmless.
        if (modeBefore == Mode::Visual || modeBefore == Mode::VisualLine)
            m_replayBuffer.clear();
        else
            commitReplay();
    }
    else if (!m_editing && !m_batchClosed && m_text == textBefore
             && m_operator == L'\0' && m_pending == Pending::None
             && !m_countActive && m_object == Object::None
             && m_mode != Mode::Insert && m_mode != Mode::Replace)
    {
        m_replayBuffer.clear(); // a motion or a cancel: nothing to repeat
    }
    m_suppressRecord = false;
}

void VimEditor::dispatch(const Key& k)
{
    if (m_commandMode)
    {
        if (k.name == KeyName::Esc)
        {
            m_commandMode = false;
            syncCursorAndSelection();
        }
        else if (k.name == KeyName::Enter)
        {
            m_commandMode = false;
            executeCommand();
        }
        else
        {
            if (k.name == KeyName::Backspace && !m_command.empty()) m_command.pop_back();
            else if (k.name == KeyName::Char && !k.ctrl) m_command += k.ch;
            m_doc.setStatus((L":" + m_command).c_str());
        }
        return;
    }
    if (k.name == KeyName::Char && k.ctrl && (k.ch == L'q' || k.ch == L'v')
        && m_mode != Mode::Insert && m_mode != Mode::Replace)
    {
        if (m_mode == Mode::Normal) m_visualAnchor = m_cursor;
        m_mode = m_mode == Mode::VisualBlock ? Mode::Normal : Mode::VisualBlock;
        syncCursorAndSelection();
        return;
    }
    // A pending f/F/t/T/r/gg eats the next key, ':' included: "f:" searches for
    // a colon, exactly like vim. Order matters, so this sits before the command
    // line check below.
    if (m_pending != Pending::None)
    {
        handlePending(k);
        return;
    }
    if (!m_runningCommand && k.name == KeyName::Char && k.ch == L':' && !k.ctrl
        && m_mode != Mode::Insert && m_mode != Mode::Replace)
    {
        m_commandFirst = m_commandLast = m_cursor.line;
        if (m_mode != Mode::Normal)
        {
            m_commandFirst = std::min(m_cursor.line, m_visualAnchor.line);
            m_commandLast = std::max(m_cursor.line, m_visualAnchor.line);
        }
        m_mode = Mode::Normal;
        m_commandMode = true;
        m_command.clear();
        syncCursorAndSelection();
        m_doc.setStatus(L":");
        return;
    }
    if (m_object != Object::None)
    {
        if (k.name == KeyName::Esc)
        {
            m_object = Object::None;
            m_operator = L'\0';
            m_count = 1;
            m_countActive = false;
            return;
        }
        if (k.name == KeyName::Char && k.ch == L'w')
        {
            std::size_t start = 0;
            std::size_t end = 0;
            if (m_mode == Mode::Visual || m_mode == Mode::VisualLine)
            {
                // viw / vaw: select the word under the cursor.
                if (wordRange(start, end, m_object == Object::AWord))
                {
                    m_visualAnchor = cursorFor(start);
                    m_cursor = cursorFor(end > start ? end - 1 : start);
                    m_desiredCol = m_cursor.col;
                }
                m_object = Object::None;
                syncCursorAndSelection();
                return;
            }
            if (wordRange(start, end, m_object == Object::AWord))
            {
                if (m_operator == L'd')
                    operatorDelete(start, end, false);
                else if (m_operator == L'c')
                    operatorChange(start, end, false);
                else if (m_operator == L'y')
                    operatorYank(start, end, false);
            }
            m_object = Object::None;
            m_operator = L'\0';
            m_count = 1;
            m_countActive = false;
            return;
        }
        // Not the object char: cancel the object and treat the key normally
        // (dj after a stray "di" still deletes two lines, as vim does).
        m_object = Object::None;
        dispatch(k);
        return;
    }
    switch (m_mode)
    {
    case Mode::Normal: handleNormal(k); break;
    case Mode::Insert: handleInsert(k); break;
    case Mode::Replace: handleReplace(k); break;
    case Mode::Visual:
    case Mode::VisualLine: handleVisual(k); break;
    case Mode::VisualBlock: handleBlock(k); break;
    }
}

void VimEditor::executeCommand()
{
    std::wstring command = m_command;
    {
        // :q / :q! — the core has no window, so it records the request and lets
        // the host decide (the rename plugin closes its own window).
        std::wstring trimmed = command;
        while (!trimmed.empty() && trimmed.front() == L' ') trimmed.erase(0, 1);
        if (trimmed == L"q" || trimmed == L"quit")
        {
            m_quitRequest = QuitRequest::Plain;
            m_doc.setStatus(L"");
            return;
        }
        if (trimmed == L"q!" || trimmed == L"quit!")
        {
            m_quitRequest = QuitRequest::Force;
            m_doc.setStatus(L"");
            return;
        }
    }
    auto first = m_commandFirst;
    auto last = m_commandLast;
    if (!command.empty() && command.front() == L'%')
    {
        first = 0;
        last = lineCount() - 1;
        command.erase(0, 1);
    }
    std::optional<std::wregex> pattern;
    bool inverse = false;
    if (command.starts_with(L"g/") || command.starts_with(L"v/"))
    {
        inverse = command.front() == L'v';
        const auto end = command.find(L'/', 2);
        if (end == std::wstring::npos) { m_doc.setStatus(L"Missing pattern delimiter"); return; }
        try { pattern.emplace(command.substr(2, end - 2)); }
        catch (const std::regex_error&) { m_doc.setStatus(L"Invalid pattern"); return; }
        first = 0;
        last = lineCount() - 1;
        command.erase(0, end + 1);
    }
    while (!command.empty() && command.front() == L' ') command.erase(0, 1);
    if (command.size() >= 4 && command.front() == L's')
    {
        const wchar_t delimiter = command[1];
        const auto patternEnd = command.find(delimiter, 2);
        const auto replacementEnd = patternEnd == std::wstring::npos
            ? std::wstring::npos : command.find(delimiter, patternEnd + 1);
        if (patternEnd == std::wstring::npos || patternEnd == 2
            || replacementEnd == std::wstring::npos)
        {
            m_doc.setStatus(L"Usage: :s/pattern/replacement/[gi]");
            return;
        }
        const std::wstring expression = command.substr(2, patternEnd - 2);
        const std::wstring replacement = command.substr(patternEnd + 1,
                                                         replacementEnd - patternEnd - 1);
        const std::wstring flags = command.substr(replacementEnd + 1);
        const bool global = flags.find(L'g') != std::wstring::npos;
        std::regex_constants::syntax_option_type options = std::regex_constants::ECMAScript;
        if (flags.find(L'i') != std::wstring::npos)
            options |= std::regex_constants::icase;
        std::wregex substitution;
        try { substitution = std::wregex(expression, options); }
        catch (const std::regex_error&) { m_doc.setStatus(L"Invalid pattern"); return; }

        std::wstring replacementFormat;
        for (std::size_t i = 0; i < replacement.size(); ++i)
        {
            if (replacement[i] == L'\\' && i + 1 < replacement.size()
                && replacement[i + 1] >= L'1' && replacement[i + 1] <= L'9')
            {
                replacementFormat += L'$';
                replacementFormat += replacement[++i];
            }
            else
                replacementFormat += replacement[i];
        }

        if (first > last || first >= lineCount())
        {
            m_doc.setStatus(L"No lines to substitute");
            return;
        }
        last = std::min(last, lineCount() - 1);
        std::vector<std::wstring> lines;
        std::size_t lineStart = 0;
        while (lineStart <= m_text.size())
        {
            const auto lineEnd = m_text.find(L'\n', lineStart);
            lines.push_back(m_text.substr(lineStart,
                                           lineEnd == std::wstring::npos
                                               ? std::wstring::npos : lineEnd - lineStart));
            if (lineEnd == std::wstring::npos)
                break;
            lineStart = lineEnd + 1;
        }

        std::size_t substitutions = 0;
        for (std::size_t line = first; line <= last; ++line)
        {
            const auto begin = std::wsregex_iterator(lines[line].begin(), lines[line].end(), substitution);
            const auto end = std::wsregex_iterator();
            const auto matches = static_cast<std::size_t>(std::distance(begin, end));
            substitutions += global ? matches : (matches > 0 ? 1u : 0u);
            if (begin != end)
                lines[line] = std::regex_replace(lines[line], substitution, replacementFormat,
                    global ? std::regex_constants::format_default
                           : std::regex_constants::format_first_only);
        }
        if (substitutions == 0)
        {
            m_doc.setStatus(L"Pattern not found");
            return;
        }

        std::wstring updated;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i != 0) updated += L'\n';
            updated += lines[i];
        }
        beginEdit();
        m_text = std::move(updated);
        flushEdit();
        endEdit();
        syncCursorAndSelection();
        m_doc.setStatus((std::to_wstring(substitutions) + L" substitutions").c_str());
        return;
    }
    std::wstring keys;
    if (command.starts_with(L"normal ")) keys = command.substr(7);
    else if (command.starts_with(L"norm ")) keys = command.substr(5);
    else if (command.starts_with(L"normal! ")) keys = command.substr(8);
    else if (command.starts_with(L"norm! ")) keys = command.substr(6);
    else if (command == L"d" || command == L"delete") keys = L"dd";
    else { m_doc.setStatus(L"Supported: s/pattern/replacement/g, norm, g/pattern/norm, g/pattern/d"); return; }
    const Snapshot before{m_text, m_cursor};
    const auto undoSize = m_undo.size();
    std::vector<std::size_t> rows;
    for (auto line = first; line <= last; ++line)
        if (!pattern || (std::regex_search(lineText(line), *pattern) != inverse)) rows.push_back(line);
    std::ptrdiff_t shift = 0;
    m_runningCommand = true;
    for (const auto row : rows)
    {
        const auto target = static_cast<std::ptrdiff_t>(row) + shift;
        if (target < 0 || static_cast<std::size_t>(target) >= lineCount()) continue;
        const auto oldCount = lineCount();
        m_cursor = {static_cast<std::size_t>(target), 0};
        m_mode = Mode::Normal;
        m_pending = Pending::None;
        m_operator = L'\0';
        m_countActive = false;
        for (const auto ch : keys) dispatch(makeChar(ch));
        dispatch(makeKey(KeyName::Esc));
        shift += static_cast<std::ptrdiff_t>(lineCount()) - static_cast<std::ptrdiff_t>(oldCount);
    }
    m_runningCommand = false;
    if (m_text != before.text)
    {
        if (m_undo.size() > undoSize) m_undo.resize(undoSize);
        m_undo.push_back(before);
        m_redo.clear();
    }
    syncCursorAndSelection();
}

QuitRequest VimEditor::takeQuitRequest()
{
    const QuitRequest request = m_quitRequest;
    m_quitRequest = QuitRequest::None;
    return request;
}

} // namespace vimcore
