// test_vimcore.cpp — the whole vimcore behaviour suite, no external dependency.
//
// A recording Document stands in for the host: it applies edits to a plain
// string, tracks cursor/selection/status, and exposes the final text so tests
// assert on what a host widget would show. Expectations follow real vim
// semantics, so a failure here is a real behaviour difference, not taste.
#include "vimcore/editor.hpp"
#include "vimcore/keys.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace vim_test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failures()
{
    static int count = 0;
    return count;
}

inline void reportFailure(const char* file, int line, const char* expr)
{
    ++failures();
    std::printf("[ FAIL ] %s:%d: %s\n", file, line, expr);
}

inline int runAll()
{
    for (const TestCase& test : registry())
    {
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        const int before = failures();
        test.fn();
        if (failures() == before)
            std::printf("[  OK  ] %s\n", test.name);
    }
    const int total = failures();
    if (total == 0)
        std::printf("[ PASS ] all %zu tests\n", registry().size());
    else
        std::printf("[ FAIL ] %d failed check(s) across %zu tests\n", total, registry().size());
    return total == 0 ? 0 : 1;
}

} // namespace vim_test

#define TEST_CASE_IMPL2(name, counter)                                                        \
    static void name##_##counter();                                                           \
    static const bool name##_##counter##_registered = [] {                                     \
        ::vim_test::registry().push_back({#name " #" #counter, name##_##counter});             \
        return true;                                                                           \
    }();                                                                                       \
    static void name##_##counter()

#define TEST_CASE_IMPL(name, counter) TEST_CASE_IMPL2(name, counter)
#define TEST_CASE(name) TEST_CASE_IMPL(name, __COUNTER__)

#define CHECK(expr)                                                                           \
    do {                                                                                      \
        if (!(expr))                                                                          \
            ::vim_test::reportFailure(__FILE__, __LINE__, #expr);                             \
    } while (false)

#define CHECK_EQ(actual, expected)                                                            \
    do {                                                                                      \
        const auto& a_ = (actual);                                                            \
        const auto& e_ = (expected);                                                          \
        if (!(a_ == e_))                                                                      \
            ::vim_test::reportFailure(__FILE__, __LINE__, #actual " == " #expected);          \
    } while (false)

// ---- the fake host -----------------------------------------------------------

class FakeDocument final : public vimcore::Document {
public:
    explicit FakeDocument(std::wstring initial, vimcore::Cursor cursor = {0, 0})
        : m_text(std::move(initial))
        , m_cursor(cursor)
    {
    }

    std::wstring text() const override { return m_text; }
    vimcore::Cursor cursor() const override { return m_cursor; }

    void apply(const std::vector<vimcore::TextEdit>& edits) override
    {
        ++applyCalls;
        for (const vimcore::TextEdit& edit : edits)
        {
            CHECK(edit.start <= edit.end);
            CHECK(edit.end <= m_text.size());
            m_text = m_text.substr(0, edit.start) + edit.replacement + m_text.substr(edit.end);
        }
    }

    void setCursor(vimcore::Cursor cursor) override
    {
        ++cursorCalls;
        m_cursor = cursor;
    }

    void setSelection(const vimcore::Selection& selection) override { m_selection = selection; }
    void setStatus(const wchar_t* status) override { m_status = status; }

    std::wstring m_text;
    vimcore::Cursor m_cursor;
    vimcore::Selection m_selection;
    std::wstring m_status;
    int applyCalls = 0;
    int cursorCalls = 0;
};

namespace vc = vimcore;

// Run a character sequence over a fresh document and return the final text.
static std::wstring run(std::wstring initial, const wchar_t* sequence,
                        vc::Cursor start = {0, 0})
{
    FakeDocument doc(std::move(initial), start);
    vc::VimEditor editor(doc);
    editor.keys(vc::chars(sequence));
    return doc.m_text;
}

static vc::Key esc() { return vc::makeKey(vc::KeyName::Esc); }
static vc::Key enter() { return vc::makeKey(vc::KeyName::Enter); }
static vc::Key backspace() { return vc::makeKey(vc::KeyName::Backspace); }
static vc::Key delKey() { return vc::makeKey(vc::KeyName::Delete); }
static vc::Key arrow(vc::KeyName name) { return vc::makeKey(name); }
static vc::Key ctrlR() { return vc::Key{vc::KeyName::Char, L'R', 1, true}; }

// ---- modes ------------------------------------------------------------------

TEST_CASE(StartsInNormalMode)
{
    FakeDocument doc(L"hello");
    vc::VimEditor editor(doc);
    CHECK(editor.mode() == vc::Mode::Normal);
    CHECK_EQ(doc.m_status, std::wstring(L"-- NORMAL --"));
}

TEST_CASE(InsertAtCursor)
{
    // "i" inserts before the char under the cursor.
    CHECK_EQ(run(L"hi", L"i!"), std::wstring(L"!hi"));
}

TEST_CASE(AppendAfterCursor)
{
    CHECK_EQ(run(L"hi", L"a!"), std::wstring(L"h!i"));
}

TEST_CASE(AppendEndOfLineAndInsertAtFirstNonBlank)
{
    CHECK_EQ(run(L"hi\nbye", L"A!"), std::wstring(L"hi!\nbye"));
    CHECK_EQ(run(L"  hi", L"I!"), std::wstring(L"  !hi"));
}

TEST_CASE(EscapeReturnsToNormal)
{
    FakeDocument doc(L"abc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    CHECK(editor.mode() == vc::Mode::Insert);
    editor.key(esc());
    CHECK(editor.mode() == vc::Mode::Normal);
    CHECK_EQ(doc.m_status, std::wstring(L"-- NORMAL --"));
}

TEST_CASE(OpenLineBelowAndAbove)
{
    CHECK_EQ(run(L"one\ntwo", L"oX"), std::wstring(L"one\nX\ntwo"));
    CHECK_EQ(run(L"one\ntwo", L"OX"), std::wstring(L"X\none\ntwo"));
}

// ---- motions ----------------------------------------------------------------

TEST_CASE(HorizontalMotionsClampToLastChar)
{
    FakeDocument doc(L"abc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'l'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{1});
    editor.key(vc::makeChar(L'l', 2));
    CHECK_EQ(doc.m_cursor.col, std::size_t{2});
    editor.key(vc::makeChar(L'l', 5)); // clamped: vim never rests past the last char
    CHECK_EQ(doc.m_cursor.col, std::size_t{2});
    editor.key(vc::makeChar(L'0'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{0});
}

TEST_CASE(WordMotionStopsAtPunctuationAndLines)
{
    // "foo bar, baz" — indexes: f0 o1 o2 _3 b4 a5 r6 ,7 _8 b9 a10 z11
    FakeDocument doc(L"foo bar, baz\nnext");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{4});  // "bar"
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{7});  // the ','
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{9});  // "baz"
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{1}); // "next"
    CHECK_EQ(doc.m_cursor.col, std::size_t{0});
}

TEST_CASE(BackwardWordMotion)
{
    FakeDocument doc(L"foo bar", {0, 4});
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'b'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{0});
    editor.key(vc::makeChar(L'b'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{0});
}

TEST_CASE(EndOfWordMotion)
{
    FakeDocument doc(L"foo bar");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'e'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{2});
    editor.key(vc::makeChar(L'e'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{6});
}

TEST_CASE(GoesToLastFirstAndCountedLine)
{
    FakeDocument doc(L"a\nb\nc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'G'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{2});
    editor.key(vc::makeChar(L'g'));
    editor.key(vc::makeChar(L'g'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{0});
    editor.key(vc::makeChar(L'2'));
    editor.key(vc::makeChar(L'G'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{1});
    editor.key(vc::makeChar(L'3'));
    editor.key(vc::makeChar(L'g'));
    editor.key(vc::makeChar(L'g'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{2});
}

TEST_CASE(UpDownKeepsDesiredColumn)
{
    FakeDocument doc(L"abcdef\nxy\nlonger line");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'3'));
    editor.key(vc::makeChar(L'l'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{3});
    editor.key(vc::makeChar(L'j'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{1});
    CHECK_EQ(doc.m_cursor.col, std::size_t{1}); // clamped to "xy"
    editor.key(vc::makeChar(L'j'));
    CHECK_EQ(doc.m_cursor.line, std::size_t{2});
    CHECK_EQ(doc.m_cursor.col, std::size_t{3}); // desired column restored
}

TEST_CASE(FindMotionAndRepeat)
{
    FakeDocument doc(L"a-b-a-b");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'f'));
    editor.key(vc::makeChar(L'-'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{1});
    editor.key(vc::makeChar(L'2'));
    editor.key(vc::makeChar(L'f'));
    editor.key(vc::makeChar(L'-'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{5}); // second '-' from the cursor
    editor.key(vc::makeChar(L';'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{5}); // nothing further forward
    editor.key(vc::makeChar(L','));
    CHECK_EQ(doc.m_cursor.col, std::size_t{3}); // backward repeat
}

TEST_CASE(TillMotion)
{
    FakeDocument doc(L"abc:de");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L't'));
    editor.key(vc::makeChar(L':'));
    CHECK_EQ(doc.m_cursor.col, std::size_t{2}); // one before ':'
}

// ---- operators ---------------------------------------------------------------

TEST_CASE(DeleteWithMotion)
{
    CHECK_EQ(run(L"hello", L"dl"), std::wstring(L"ello"));
    CHECK_EQ(run(L"hello", L"x"), std::wstring(L"ello"));
    CHECK_EQ(run(L"hello", L"3x"), std::wstring(L"lo"));
    CHECK_EQ(run(L"foo bar", L"dw"), std::wstring(L"bar"));
    // d$ from the first character takes the whole line.
    CHECK_EQ(run(L"foo bar", L"d$"), std::wstring(L""));
    CHECK_EQ(run(L"abcdef", L"d2l"), std::wstring(L"cdef"));
    CHECK_EQ(run(L"foo bar", L"de"), std::wstring(L" bar"));
    // Equal to x: an exclusive motion that cannot advance still covers the char.
    CHECK_EQ(run(L"x", L"dl"), std::wstring(L""));
}

TEST_CASE(DeleteToLineStart)
{
    FakeDocument doc(L"hello", {0, 3});
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'd'));
    editor.key(vc::makeChar(L'0'));
    CHECK_EQ(doc.m_text, std::wstring(L"lo"));
}

TEST_CASE(DeleteLineDoubled)
{
    CHECK_EQ(run(L"a\nb\nc", L"dd"), std::wstring(L"b\nc"));
    CHECK_EQ(run(L"a\nb\nc", L"2dd"), std::wstring(L"c"));
    // dG from the first line leaves nothing; from the second it leaves the first.
    // (The document model keeps a trailing newline, so "a" here is "a\n".)
    CHECK_EQ(run(L"a\nb\nc", L"dG"), std::wstring(L""));
    CHECK_EQ(run(L"a\nb\nc", L"jdG"), std::wstring(L"a\n"));
}

TEST_CASE(ChangeWithMotionEntersInsert)
{
    FakeDocument doc(L"foo bar");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'c'));
    editor.key(vc::makeChar(L'w'));
    CHECK(editor.mode() == vc::Mode::Insert);
    editor.key(vc::makeChar(L'X'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"Xbar"));
}

TEST_CASE(ChangeLineKeepsEmptyLine)
{
    FakeDocument doc(L"a\nb");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'c'));
    editor.key(vc::makeChar(L'c'));
    CHECK(editor.mode() == vc::Mode::Insert);
    editor.key(vc::makeChar(L'Z'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"Z\nb"));
}

TEST_CASE(YankLineAndPasteBelow)
{
    FakeDocument doc(L"foo bar");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'y'));
    editor.key(vc::makeChar(L'y'));
    CHECK_EQ(editor.registerText(), std::wstring(L"foo bar\n"));
    editor.key(vc::makeChar(L'p'));
    CHECK_EQ(doc.m_text, std::wstring(L"foo bar\nfoo bar\n"));
}

TEST_CASE(YankWordAndPasteAfterCursor)
{
    FakeDocument doc(L"foo");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'y'));
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(editor.registerText(), std::wstring(L"foo"));
    editor.key(vc::makeChar(L'p'));
    // yw leaves the cursor at the range start; p pastes after that char.
    CHECK_EQ(doc.m_text, std::wstring(L"ffoooo"));
}

TEST_CASE(PasteLineAbove)
{
    FakeDocument doc(L"a\nb");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'd'));
    editor.key(vc::makeChar(L'd'));
    CHECK_EQ(doc.m_text, std::wstring(L"b"));
    editor.key(vc::makeChar(L'P'));
    CHECK_EQ(doc.m_text, std::wstring(L"a\nb"));
}

TEST_CASE(DeleteStoresRegister)
{
    FakeDocument doc(L"hello");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'd'));
    editor.key(vc::makeChar(L'$'));
    CHECK_EQ(editor.registerText(), std::wstring(L"hello"));
}

TEST_CASE(JoinLines)
{
    CHECK_EQ(run(L"one\ntwo", L"J"), std::wstring(L"one two"));
    CHECK_EQ(run(L"one \ntwo", L"J"), std::wstring(L"one two"));
    CHECK_EQ(run(L"one\n  two", L"J"), std::wstring(L"one two"));
}

TEST_CASE(ReplaceCharacter)
{
    CHECK_EQ(run(L"abc", L"rZ"), std::wstring(L"Zbc"));
    CHECK_EQ(run(L"abcdef", L"3rX"), std::wstring(L"XXXdef"));
}

TEST_CASE(ReplaceNeedsCharArgument)
{
    FakeDocument doc(L"abc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'r'));
    CHECK_EQ(doc.m_text, std::wstring(L"abc"));
    editor.key(vc::makeChar(L'X'));
    CHECK_EQ(doc.m_text, std::wstring(L"Xbc"));
}

// ---- undo / redo ----------------------------------------------------------------

TEST_CASE(UndoRestoresText)
{
    FakeDocument doc(L"hello");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L"ello"));
    editor.key(vc::makeChar(L'u'));
    CHECK_EQ(doc.m_text, std::wstring(L"hello"));
    editor.key(ctrlR());
    CHECK_EQ(doc.m_text, std::wstring(L"ello"));
}

TEST_CASE(InsertBatchIsOneUndo)
{
    FakeDocument doc(L"a");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    editor.key(vc::makeChar(L'b'));
    editor.key(vc::makeChar(L'c'));
    editor.key(vc::makeChar(L'd'));
    editor.key(esc());
    // Inserting before 'a' pushes it right, as vim does.
    CHECK_EQ(doc.m_text, std::wstring(L"bcda"));
    editor.key(vc::makeChar(L'u'));
    CHECK_EQ(doc.m_text, std::wstring(L"a"));
}

TEST_CASE(UndoOfAppendAndJoin)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'a'));
    editor.key(vc::makeChar(L'X'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"aXb"));
    editor.key(vc::makeChar(L'u'));
    CHECK_EQ(doc.m_text, std::wstring(L"ab"));

    CHECK_EQ(run(L"one\ntwo", L"Ju"), std::wstring(L"one\ntwo"));
}

TEST_CASE(RedoAfterTwoUndos)
{
    FakeDocument doc(L"abc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'x'));
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L"c"));
    editor.key(vc::makeChar(L'u'));
    CHECK_EQ(doc.m_text, std::wstring(L"bc"));
    editor.key(vc::makeChar(L'u'));
    CHECK_EQ(doc.m_text, std::wstring(L"abc"));
    editor.key(ctrlR());
    CHECK_EQ(doc.m_text, std::wstring(L"bc"));
    editor.key(ctrlR());
    CHECK_EQ(doc.m_text, std::wstring(L"c"));
}

// ---- Visual mode ------------------------------------------------------------

TEST_CASE(VisualDeleteRange)
{
    FakeDocument doc(L"hello world");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'v'));
    CHECK(editor.mode() == vc::Mode::Visual);
    editor.key(vc::makeChar(L'l'));
    editor.key(vc::makeChar(L'l'));
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L"lo world"));
    CHECK(editor.mode() == vc::Mode::Normal);
}

TEST_CASE(VisualLineDelete)
{
    FakeDocument doc(L"a\nb\nc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'V'));
    CHECK(editor.mode() == vc::Mode::VisualLine);
    editor.key(vc::makeChar(L'j'));
    editor.key(vc::makeChar(L'd'));
    CHECK_EQ(doc.m_text, std::wstring(L"c"));
}

TEST_CASE(VisualYankThenPaste)
{
    FakeDocument doc(L"abc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'v'));
    editor.key(vc::makeChar(L'l'));
    editor.key(vc::makeChar(L'y'));
    CHECK_EQ(editor.registerText(), std::wstring(L"ab"));
    editor.key(vc::makeChar(L'$'));
    editor.key(vc::makeChar(L'p'));
    CHECK_EQ(doc.m_text, std::wstring(L"abcab"));
}

TEST_CASE(EscapesVisualMode)
{
    FakeDocument doc(L"abc");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'v'));
    editor.key(esc());
    CHECK(editor.mode() == vc::Mode::Normal);
    CHECK(!doc.m_selection.active);
}

TEST_CASE(VisualSelectionReported)
{
    FakeDocument doc(L"abcdef");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'v'));
    editor.key(vc::makeChar(L'l'));
    editor.key(vc::makeChar(L'l'));
    CHECK(doc.m_selection.active);
    CHECK_EQ(doc.m_selection.anchor.col, std::size_t{0});
    CHECK_EQ(doc.m_selection.head.col, std::size_t{2});
}

TEST_CASE(VisualChangeEntersInsert)
{
    FakeDocument doc(L"abcdef");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'v'));
    editor.key(vc::makeChar(L'l'));
    editor.key(vc::makeChar(L'c'));
    CHECK(editor.mode() == vc::Mode::Insert);
    editor.key(vc::makeChar(L'Z'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"Zcdef"));
}

// ---- counts ------------------------------------------------------------------

TEST_CASE(CountBeforeOperatorStacksMultiplicatively)
{
    CHECK_EQ(run(L"one two three", L"2dw"), std::wstring(L"three"));
    CHECK_EQ(run(L"one two three", L"d2w"), std::wstring(L"three"));
}

TEST_CASE(CountedLineDelete)
{
    CHECK_EQ(run(L"a\nb\nc\nd", L"3dd"), std::wstring(L"d"));
}

TEST_CASE(CountAppliesToYank)
{
    FakeDocument doc(L"a b c");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'2'));
    editor.key(vc::makeChar(L'y'));
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(editor.registerText(), std::wstring(L"a b "));
}

TEST_CASE(CountAppliesToPaste)
{
    FakeDocument doc(L"x");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'y'));
    editor.key(vc::makeChar(L'l'));
    CHECK_EQ(editor.registerText(), std::wstring(L"x"));
    editor.key(vc::makeChar(L'2'));
    editor.key(vc::makeChar(L'p'));
    CHECK_EQ(doc.m_text, std::wstring(L"xxx"));
}

// ---- insert-mode editing -------------------------------------------------------

TEST_CASE(InsertBackspaceAtLineStartJoins)
{
    FakeDocument doc(L"a\nb", {1, 0});
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    editor.key(backspace());
    CHECK_EQ(doc.m_text, std::wstring(L"ab"));
}

TEST_CASE(InsertDeletesForwardWithDeleteKey)
{
    FakeDocument doc(L"ab", {0, 0});
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    editor.key(delKey());
    CHECK_EQ(doc.m_text, std::wstring(L"b"));
}

TEST_CASE(InsertModeArrowKeys)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    editor.key(arrow(vc::KeyName::Right));
    editor.key(vc::makeChar(L'X'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"aXb"));
}

TEST_CASE(InsertEnterSplitsAndKeepsTyping)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'a'));
    editor.key(enter());
    editor.key(vc::makeChar(L'X'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"a\nXb"));
}

// ---- safety --------------------------------------------------------------------

TEST_CASE(DeletePastEndChangesNothing)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'x'));
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L""));
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L""));
}

TEST_CASE(EscInNormalIsHarmless)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(esc());
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"ab"));
    CHECK(editor.mode() == vc::Mode::Normal);
}

TEST_CASE(UnknownKeyDoesNotEdit)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'Z'));       // no Z command in v1
    editor.key(vc::makeChar(L'q'));
    CHECK_EQ(doc.m_text, std::wstring(L"ab"));
    CHECK_EQ(doc.applyCalls, 0);
}

TEST_CASE(OneApplyPerKeyStroke)
{
    FakeDocument doc(L"hello");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.applyCalls, 1);
    editor.key(vc::makeChar(L'l'));
    CHECK_EQ(doc.applyCalls, 1);          // motion: no edit
    editor.key(vc::makeChar(L'd'));
    editor.key(vc::makeChar(L'w'));
    CHECK_EQ(doc.applyCalls, 2);          // one edit for the whole operator
}

TEST_CASE(EmptyBufferIsSafe)
{
    FakeDocument doc(L"");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'x'));
    editor.key(vc::makeChar(L'd'));
    editor.key(vc::makeChar(L'd'));
    editor.key(vc::makeChar(L'p'));
    CHECK_EQ(doc.m_text, std::wstring(L""));
}

// ---- external changes (refreshFromDocument) ------------------------------------

TEST_CASE(RefreshFromDocumentKeepsInsertMode)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    CHECK(editor.mode() == vc::Mode::Insert);

    // The host (or IME) typed behind the core's back.
    doc.m_text = L"XYab";
    doc.m_cursor = {0, 2};
    editor.refreshFromDocument();

    CHECK(editor.mode() == vc::Mode::Insert); // still inserting, not reset
    editor.key(vc::makeChar(L'Z'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"XYZab")); // inserted at the caret
    CHECK(editor.mode() == vc::Mode::Normal);
}

TEST_CASE(NativeInsertIsOneUndoStep)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    doc.m_text = L"Xab";
    doc.m_cursor = {0, 1};
    editor.refreshFromDocument();
    doc.m_text = L"XYab";
    doc.m_cursor = {0, 2};
    editor.refreshFromDocument();
    editor.key(esc());

    CHECK_EQ(doc.m_text, std::wstring(L"XYab"));
    editor.key(vc::makeChar(L'u'));
    CHECK_EQ(doc.m_text, std::wstring(L"ab")); // both native chars undone together
    editor.key(ctrlR());
    CHECK_EQ(doc.m_text, std::wstring(L"XYab"));
}

TEST_CASE(NormalEditsUseTheRefreshedText)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    doc.m_text = L"hello ab";
    doc.m_cursor = {0, 8};
    editor.refreshFromDocument();
    editor.key(esc());
    editor.key(vc::makeChar(L'$'));
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L"hello a")); // deleted 'b', not a stale offset
}

TEST_CASE(RefreshDropsPendingOperator)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'd'));
    doc.m_text = L"abc";
    doc.m_cursor = {0, 1};
    editor.refreshFromDocument();
    // 'd' was pending; after a native change the next key must not be treated
    // as its motion (that would delete text the user did not ask for).
    editor.key(vc::makeChar(L'd'));
    CHECK_EQ(doc.m_text, std::wstring(L"abc"));
}

TEST_CASE(RefreshOutsideBatchSyncsText)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    doc.m_text = L"abX";
    doc.m_cursor = {0, 2};
    editor.refreshFromDocument(); // native change while in Normal mode
    editor.key(vc::makeChar(L'$'));
    editor.key(vc::makeChar(L'x'));
    CHECK_EQ(doc.m_text, std::wstring(L"ab"));
}

TEST_CASE(RefreshIsANoOpWhenTextMatches)
{
    FakeDocument doc(L"ab");
    vc::VimEditor editor(doc);
    editor.key(vc::makeChar(L'i'));
    editor.refreshFromDocument(); // nothing changed natively
    CHECK(editor.mode() == vc::Mode::Insert);
    editor.key(vc::makeChar(L'Z'));
    editor.key(esc());
    CHECK_EQ(doc.m_text, std::wstring(L"Zab"));
}

int main()
{
    return vim_test::runAll();
}
