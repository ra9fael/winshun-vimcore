// test_conformance.cpp — behaviour conformance against real vim semantics.
//
// Every expectation here is what stock vim does, not what the core happens to
// do today. The file drove the 0.2.0 fixes: h/l line clamping, r cursor and
// failure rules, Replace mode, Ctrl-A/X numeric edit, viewport paging, the
// undo matrix, visual o/u/U/~/p/J, Y, ~, iw/aw text objects, %, and '.'.
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
        for (const vimcore::TextEdit& edit : edits)
            m_text = m_text.substr(0, edit.start) + edit.replacement + m_text.substr(edit.end);
    }

    void setCursor(vimcore::Cursor cursor) override { m_cursor = cursor; }
    void setSelection(const vimcore::Selection& selection) override { m_selection = selection; }
    void setStatus(const wchar_t* status) override { m_status = status; }

    std::wstring m_text;
    vimcore::Cursor m_cursor;
    vimcore::Selection m_selection;
    std::wstring m_status;
};

namespace vc = vimcore;

// A document plus its editor, ready for key sequences.
struct Env {
    FakeDocument doc;
    vc::VimEditor editor;

    explicit Env(std::wstring text, vc::Cursor start = {0, 0}, std::size_t rows = 20)
        : doc(std::move(text), start)
        , editor(doc, vc::EditorOptions{1000, rows})
    {
    }
};

static vc::Key esc() { return vc::makeKey(vc::KeyName::Esc); }
static vc::Key enter() { return vc::makeKey(vc::KeyName::Enter); }
static vc::Key bsKey() { return vc::makeKey(vc::KeyName::Backspace); }
static vc::Key delKey() { return vc::makeKey(vc::KeyName::Delete); }
static vc::Key pageUp() { return vc::makeKey(vc::KeyName::PageUp); }
static vc::Key pageDown() { return vc::makeKey(vc::KeyName::PageDown); }
static vc::Key ctrl(wchar_t ch, int count = 1) { return vc::Key{vc::KeyName::Char, ch, count, true}; }

static std::vector<vc::Key> keys(std::initializer_list<vc::Key> list)
{
    return std::vector<vc::Key>(list);
}

static void appendOne(std::vector<vc::Key>& all, const vc::Key& k) { all.push_back(k); }
static void appendOne(std::vector<vc::Key>& all, const std::vector<vc::Key>& ks)
{
    all.insert(all.end(), ks.begin(), ks.end());
}

// Concatenate char runs and single keys into one key sequence.
template <typename... Parts>
static std::vector<vc::Key> seq(Parts... parts)
{
    std::vector<vc::Key> all;
    (appendOne(all, parts), ...);
    return all;
}

// ---- motions: h/l and the arrow keys must not cross lines ----------------------

TEST_CASE(HStopsAtLineStart)
{
    Env e(L"ab\ncd", {1, 1});
    e.editor.keys(vc::chars(L"hh"));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{1});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0}); // stays on 'c', no wrap to "ab"
}

TEST_CASE(LStopsAtLineEnd)
{
    Env e(L"ab\ncd", {0, 1});
    e.editor.keys(vc::chars(L"lll"));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{1}); // last char of "ab"
}

TEST_CASE(ArrowKeysDoNotWrapLines)
{
    Env e(L"ab\ncd", {1, 0});
    e.editor.key(vc::makeKey(vc::KeyName::Left));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{1});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});

    Env e2(L"ab\ncd", {0, 1});
    e2.editor.key(vc::makeKey(vc::KeyName::Right));
    CHECK_EQ(e2.doc.m_cursor.line, std::size_t{0});
    CHECK_EQ(e2.doc.m_cursor.col, std::size_t{1});
}

TEST_CASE(SpaceMovesRightAndWraps)
{
    Env e(L"ab\ncd", {0, 1});
    e.editor.key(vc::makeChar(L' '));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{1}); // whichwrap has 's'
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});

    Env last(L"ab\ncd", {1, 1});
    last.editor.key(vc::makeChar(L' '));
    CHECK_EQ(last.doc.m_cursor.line, std::size_t{1}); // nothing after: stays
    CHECK_EQ(last.doc.m_cursor.col, std::size_t{1});
}

TEST_CASE(SpaceAsOperatorMotion)
{
    Env e(L"ab\ncd", {0, 0});
    e.editor.keys(vc::chars(L"d "));
    CHECK_EQ(e.doc.m_text, std::wstring(L"b\ncd")); // d<Space> == dl
}

TEST_CASE(BackspaceWrapsBackward)
{
    Env e(L"ab\ncd", {1, 0});
    e.editor.key(bsKey());
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{1});
}

TEST_CASE(DollarCountEndsOneLineDown)
{
    Env e(L"aa\nbb\ncc", {0, 0});
    e.editor.key(vc::makeChar(L'$'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{1});
    e.editor.keys(vc::chars(L"0"));
    e.editor.keys(vc::chars(L"2$"));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{1}); // 2$ = end of the NEXT line
    e.editor.keys(vc::chars(L"0"));
    e.editor.keys(vc::chars(L"3$"));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{2});
}

TEST_CASE(EndKeyMatchesDollar)
{
    Env e(L"aa\nbb\ncc", {0, 0});
    e.editor.key(vc::makeKey(vc::KeyName::End));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{1});
    e.editor.keys(vc::chars(L"0"));
    e.editor.keys(vc::chars(L"2"));
    e.editor.key(vc::makeKey(vc::KeyName::End));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{1});
}

TEST_CASE(GAndGgLandOnFirstNonBlank)
{
    Env e(L"  ab\ncd\n  ef", {0, 0});
    e.editor.key(vc::makeChar(L'G'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{2});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{2}); // first non-blank of the last line
    e.editor.keys(vc::chars(L"2G"));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{1});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});
    e.editor.keys(vc::chars(L"gg"));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0});
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{2});
}

// ---- r -----------------------------------------------------------------------

TEST_CASE(ReplaceCursorLandsOnLastReplaced)
{
    Env e(L"abcdef", {0, 0});
    e.editor.keys(vc::chars(L"3rX"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"XXXdef"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{2});
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

TEST_CASE(ReplaceCountLongerThanLineFails)
{
    Env e(L"ab", {0, 0});
    e.editor.keys(vc::chars(L"3rX"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"ab")); // vim fails the command, no change
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

TEST_CASE(ReplaceWithEnterSplitsLine)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(keys({vc::makeChar(L'r'), enter()}));
    // r<CR> replaces the character under the cursor with a line break.
    CHECK_EQ(e.doc.m_text, std::wstring(L"\nbc"));
}

TEST_CASE(ReplaceEscapeCancels)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(keys({vc::makeChar(L'r'), esc()}));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abc"));
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

TEST_CASE(ReplaceOnEmptyLineDoesNothing)
{
    Env e(L"\na", {0, 0});
    e.editor.keys(vc::chars(L"rX"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"\na"));
}

// ---- R (Replace mode) --------------------------------------------------------

TEST_CASE(ReplaceModeOverwrites)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(vc::chars(L"R"));
    CHECK(e.editor.mode() == vc::Mode::Replace);
    e.editor.keys(vc::chars(L"XY"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"XYc"));
    e.editor.key(esc());
    CHECK(e.editor.mode() == vc::Mode::Normal);
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{1}); // on the last typed char
}

TEST_CASE(ReplaceModeBackspaceRestores)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(vc::chars(L"R"));
    e.editor.key(vc::makeChar(L'X'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"Xbc"));
    e.editor.key(bsKey());
    CHECK_EQ(e.doc.m_text, std::wstring(L"abc")); // the original 'a' is back
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});
    e.editor.key(vc::makeChar(L'Z'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"Zbc"));
}

TEST_CASE(ReplaceModeCountRepeatsTypedText)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(seq(vc::chars(L"2R"), vc::chars(L"xy"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"xyxy"));
}

TEST_CASE(ReplaceModeEnterAddsLineBreak)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(keys({vc::makeChar(L'R'), enter()}));
    // <CR> in Replace mode breaks the line like in Insert mode.
    CHECK_EQ(e.doc.m_text, std::wstring(L"\nabc"));
    e.editor.key(esc());
    CHECK_EQ(e.doc.m_text, std::wstring(L"\nabc"));
}

TEST_CASE(ReplaceModeOverwritesThenAppends)
{
    // The cursor always sits on a character in Normal mode, so R starts by
    // replacing it; typing past the end of the line appends.
    Env e(L"ab", {0, 1});
    e.editor.keys(seq(vc::chars(L"RXY"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"aXY"));
}

TEST_CASE(ReplaceModeSessionIsOneUndo)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(seq(vc::chars(L"RXY"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"XYc"));
    e.editor.key(vc::makeChar(L'u'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abc"));
}

TEST_CASE(ReplaceModeAtLineEndReplaces)
{
    Env e(L"ab", {0, 1});
    e.editor.keys(seq(vc::chars(L"RX"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"aX")); // X replaced 'b', the last char
}

// ---- Ctrl-A / Ctrl-X ---------------------------------------------------------

TEST_CASE(CtrlAIncrementsNumberUnderCursor)
{
    Env e(L"x123y", {0, 0});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"x124y"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{3}); // on the last digit
}

TEST_CASE(CtrlAFindsNextNumberOnLine)
{
    Env e(L"abc 42", {0, 0});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abc 43"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{5});
}

TEST_CASE(CtrlXDecrements)
{
    Env e(L"10", {0, 0});
    e.editor.key(ctrl(L'x'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"9"));
}

TEST_CASE(CtrlAHandlesSignedNumbers)
{
    Env e(L"-5", {0, 0});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"-4"));
    Env d(L"0", {0, 0});
    d.editor.key(ctrl(L'x'));
    CHECK_EQ(d.doc.m_text, std::wstring(L"-1"));
}

TEST_CASE(CtrlAPreservesZeroPadding)
{
    Env e(L"007", {0, 0});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"008"));
    Env e2(L"009", {0, 0});
    e2.editor.key(ctrl(L'a'));
    CHECK_EQ(e2.doc.m_text, std::wstring(L"010"));
    Env e3(L"099", {0, 0});
    e3.editor.key(ctrl(L'a'));
    CHECK_EQ(e3.doc.m_text, std::wstring(L"100"));
}

TEST_CASE(CtrlAWithCount)
{
    Env e(L"10", {0, 0});
    e.editor.keys(vc::chars(L"5"));
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"15"));
}

TEST_CASE(CtrlAWithoutNumberIsNoop)
{
    Env e(L"abc", {0, 1});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abc"));
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

TEST_CASE(CtrlAIgnoresNumberBeforeCursor)
{
    Env e(L"12 xy", {0, 3}); // cursor after the number
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"12 xy"));
}

TEST_CASE(CtrlAOnMiddleDigit)
{
    Env e(L"123", {0, 1});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"124"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{2});
}

// ---- viewport paging ---------------------------------------------------------

TEST_CASE(ViewportPaging)
{
    std::wstring text;
    for (int i = 0; i < 20; ++i)
    {
        if (i)
            text += L'\n';
        text += L"line" + std::to_wstring(i);
    }
    Env e(text, {0, 0}, /*rows*/ 10);

    e.editor.key(ctrl(L'd'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{5}); // half page
    e.editor.keys(vc::chars(L"2"));
    e.editor.key(ctrl(L'd'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{15});
    e.editor.key(ctrl(L'u'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{10});
    e.editor.key(ctrl(L'b'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0}); // full page up, clamped
    e.editor.key(pageDown());
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{10});
    e.editor.key(pageUp());
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{0});
    e.editor.key(ctrl(L'f'));
    CHECK_EQ(e.doc.m_cursor.line, std::size_t{10});
}

// ---- undo / redo matrix ------------------------------------------------------

TEST_CASE(UndoStepsPerCommand)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(vc::chars(L"xxx"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"d"));
    e.editor.keys(vc::chars(L"uu"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"bcd"));
    e.editor.key(vc::makeChar(L'u'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abcd"));
}

TEST_CASE(UndoOfOpenLine)
{
    Env e(L"one\ntwo", {0, 0});
    e.editor.keys(seq(vc::chars(L"oX"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"one\nX\ntwo"));
    e.editor.key(vc::makeChar(L'u'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"one\ntwo"));
}

TEST_CASE(UndoRestoresCursor)
{
    Env e(L"hello world", {0, 6});
    e.editor.keys(vc::chars(L"dw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"hello "));
    e.editor.key(vc::makeChar(L'u'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"hello world"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{6});
}

TEST_CASE(CountedUndo)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(vc::chars(L"xxx"));
    e.editor.keys(vc::chars(L"3u"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abcd"));
}

TEST_CASE(RedoMatchesUndo)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(vc::chars(L"xxuu"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"abcd"));
    e.editor.key(ctrl(L'R'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"bcd"));
    e.editor.key(ctrl(L'R'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"cd"));
}

TEST_CASE(NewEditClearsRedo)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(vc::chars(L"xux"));
    e.editor.key(ctrl(L'R'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"bcd")); // redo stack was dropped by x
}

TEST_CASE(UndoKeepsRegister)
{
    Env e(L"a\nb", {0, 0});
    e.editor.keys(vc::chars(L"ddu"));
    e.editor.key(vc::makeChar(L'p'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"a\na\nb")); // dd's register survived u
}

TEST_CASE(UndoNoopWhenStackEmpty)
{
    Env e(L"ab", {0, 0});
    e.editor.key(vc::makeChar(L'u'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"ab"));
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

TEST_CASE(ExternalChangeThenVimEdits)
{
    Env e(L"hello world", {0, 0});
    // The host undid its own buffer (Ctrl+Z) while the core sat in Normal.
    e.doc.m_text = L"goodbye world";
    e.doc.m_cursor = {0, 0};
    e.editor.refreshFromDocument();
    e.editor.keys(vc::chars(L"dw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"world")); // operated on the new text
}

// ---- visual ------------------------------------------------------------------

TEST_CASE(VisualUppercaseLowercaseTilde)
{
    Env up(L"aBc", {0, 0});
    up.editor.keys(vc::chars(L"vllU"));
    CHECK_EQ(up.doc.m_text, std::wstring(L"ABC"));

    Env low(L"aBc", {0, 0});
    low.editor.keys(vc::chars(L"vllu"));
    CHECK_EQ(low.doc.m_text, std::wstring(L"abc"));

    Env tilde(L"aBc", {0, 0});
    tilde.editor.keys(vc::chars(L"vll~"));
    CHECK_EQ(tilde.doc.m_text, std::wstring(L"AbC"));
}

TEST_CASE(VisualJoinSelectedLines)
{
    Env e(L"one\ntwo\nthree", {0, 0});
    e.editor.keys(vc::chars(L"VjJ"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"one two\nthree"));
}

TEST_CASE(VisualPasteReplacesSelection)
{
    Env e(L"foo bar baz", {0, 0});
    e.editor.keys(vc::chars(L"yiw")); // register "foo"
    e.editor.keys(vc::chars(L"w"));   // on 'b' of bar
    e.editor.keys(vc::chars(L"vep"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"foo foo baz"));
    CHECK_EQ(e.editor.registerText(), std::wstring(L"bar")); // vim swaps the registers
}

TEST_CASE(VisualLinewisePaste)
{
    Env e(L"a\nb\nc", {0, 0});
    e.editor.keys(vc::chars(L"dd")); // register "a\n", cursor line 0 ("b")
    e.editor.keys(vc::chars(L"Vp"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"a\nc"));
}

TEST_CASE(VisualOSwapsEnds)
{
    Env e(L"abcdef", {0, 0});
    e.editor.keys(vc::chars(L"vlllo"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0}); // cursor jumped to the anchor
    e.editor.keys(vc::chars(L"ld"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"aef")); // extended from the other end
}

TEST_CASE(VisualSwitchKeepsAnchor)
{
    Env e(L"abc\ndef", {0, 0});
    e.editor.keys(seq(vc::chars(L"vll"), vc::makeChar(L'V')));
    CHECK(e.editor.mode() == vc::Mode::VisualLine);
    CHECK_EQ(e.doc.m_selection.anchor.col, std::size_t{0}); // anchor survived the switch
    e.editor.keys(vc::chars(L"d"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"def"));
}

TEST_CASE(VisualYankCursorToStart)
{
    Env e(L"abcdef", {0, 0});
    e.editor.keys(vc::chars(L"vlly"));
    CHECK_EQ(e.editor.registerText(), std::wstring(L"abc"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

// ---- text objects ------------------------------------------------------------

TEST_CASE(DiwDeletesInnerWord)
{
    Env e(L"foo bar", {0, 0});
    e.editor.keys(vc::chars(L"diw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L" bar"));
}

TEST_CASE(DawDeletesWordAndTrailingSpace)
{
    Env e(L"foo bar", {0, 0});
    e.editor.keys(vc::chars(L"daw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"bar"));
}

TEST_CASE(DawTakesLeadingSpaceAtLineEnd)
{
    Env e(L"foo   bar", {0, 6});
    e.editor.keys(vc::chars(L"daw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"foo")); // no trailing space: take the leading run
}

TEST_CASE(CiwEntersInsert)
{
    Env e(L"foo bar", {0, 0});
    e.editor.keys(seq(vc::chars(L"ciw"), vc::chars(L"X"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"X bar"));
}

TEST_CASE(DiwOnPunctuationRun)
{
    Env e(L"foo.bar", {0, 3});
    e.editor.keys(vc::chars(L"diw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"foobar"));
}

TEST_CASE(DiwOnWhitespaceDeletesRun)
{
    Env e(L"foo   bar", {0, 4});
    e.editor.keys(vc::chars(L"diw"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"foobar"));
}

TEST_CASE(YiwRegistersWord)
{
    Env e(L"foo bar", {0, 0});
    e.editor.keys(vc::chars(L"yiw"));
    CHECK_EQ(e.editor.registerText(), std::wstring(L"foo"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});
    CHECK(e.editor.mode() == vc::Mode::Normal);
}

// ---- % -----------------------------------------------------------------------

TEST_CASE(PercentJumpsToMatch)
{
    Env e(L"(a)b", {0, 0}); // ')' is at index 2
    e.editor.key(vc::makeChar(L'%'));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{2});
    e.editor.key(vc::makeChar(L'%'));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});

    Env nest(L"((a))", {0, 0});
    nest.editor.key(vc::makeChar(L'%'));
    CHECK_EQ(nest.doc.m_cursor.col, std::size_t{4});

    Env mixed(L"f(a,(b))g", {0, 1});
    mixed.editor.key(vc::makeChar(L'%'));
    CHECK_EQ(mixed.doc.m_cursor.col, std::size_t{7});
}

TEST_CASE(PercentUnmatchedIsNoop)
{
    Env e(L"(ab", {0, 0});
    e.editor.key(vc::makeChar(L'%'));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{0});
}

TEST_CASE(PercentAsOperatorRange)
{
    Env e(L"(ab)x", {0, 0});
    e.editor.keys(vc::chars(L"d%"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"x"));
    Env closing(L"(ab)", {0, 3});
    closing.editor.keys(vc::chars(L"d%"));
    CHECK_EQ(closing.doc.m_text, std::wstring(L""));
}

// ---- dot repeat --------------------------------------------------------------

TEST_CASE(DotRepeatsX)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(vc::chars(L"x."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"cd"));
    e.editor.keys(vc::chars(L"."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"d"));
}

TEST_CASE(DotRepeatsCountedCommand)
{
    Env e(L"abcdef", {0, 0});
    e.editor.keys(vc::chars(L"3x."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"")); // 3x ate "abc", . ate "def"
}

TEST_CASE(DotRepeatsReplaceChar)
{
    Env e(L"abc", {0, 0});
    e.editor.keys(vc::chars(L"rZl."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"ZZc"));
}

TEST_CASE(DotRepeatsDw)
{
    Env e(L"one two three", {0, 0});
    e.editor.keys(vc::chars(L"dw"));
    e.editor.keys(vc::chars(L"w."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"two ")); // . repeated dw on "three"
}

TEST_CASE(DotRepeatsInsertSession)
{
    Env e(L"ab", {0, 0});
    e.editor.keys(seq(vc::chars(L"iXY"), esc()));
    CHECK_EQ(e.doc.m_text, std::wstring(L"XYab"));
    e.editor.keys(vc::chars(L"ww."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"XYaXYb")); // XY inserted before 'b'
}

TEST_CASE(DotRepeatsOpenLine)
{
    Env e(L"one", {0, 0});
    e.editor.keys(seq(vc::chars(L"oX"), esc()));
    e.editor.keys(vc::chars(L"."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"one\nX\nX"));
}

TEST_CASE(DotRepeatsCtrlA)
{
    Env e(L"5", {0, 0});
    e.editor.key(ctrl(L'a'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"6"));
    e.editor.keys(vc::chars(L"."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"7"));

    Env two(L"1 5", {0, 0});
    two.editor.key(ctrl(L'a'));
    two.editor.keys(vc::chars(L"w."));
    CHECK_EQ(two.doc.m_text, std::wstring(L"2 6"));
}

TEST_CASE(UndoDoesNotClearDot)
{
    Env e(L"abcd", {0, 0});
    e.editor.keys(vc::chars(L"xu."));
    CHECK_EQ(e.doc.m_text, std::wstring(L"bcd")); // . replays x after the undo
}

TEST_CASE(DotAfterNativeTyping)
{
    Env e(L"ab", {0, 0});
    e.editor.key(vc::makeChar(L'i'));
    // The host typed behind the core's back (native insert).
    e.doc.m_text = L"Xab";
    e.doc.m_cursor = {0, 1};
    e.editor.refreshFromDocument();
    e.editor.key(esc());
    e.editor.keys(vc::chars(L"$.")); // cursor before 'b', repeat the insert
    CHECK_EQ(e.doc.m_text, std::wstring(L"XaXb"));
}

// ---- misc --------------------------------------------------------------------

TEST_CASE(DeleteKeyYanksInNormal)
{
    Env e(L"abc", {0, 0});
    e.editor.key(delKey());
    CHECK_EQ(e.doc.m_text, std::wstring(L"bc"));
    CHECK_EQ(e.editor.registerText(), std::wstring(L"a"));
}

TEST_CASE(CapitalYYanksLine)
{
    Env e(L"a\nb", {0, 0});
    e.editor.key(vc::makeChar(L'Y'));
    CHECK_EQ(e.editor.registerText(), std::wstring(L"a\n"));
    e.editor.key(vc::makeChar(L'p'));
    CHECK_EQ(e.doc.m_text, std::wstring(L"a\na\nb"));
}

TEST_CASE(TildeTogglesAndAdvances)
{
    Env e(L"ab", {0, 0});
    e.editor.keys(vc::chars(L"~~"));
    CHECK_EQ(e.doc.m_text, std::wstring(L"AB"));
    CHECK_EQ(e.doc.m_cursor.col, std::size_t{1}); // advanced, no wrap
}

int main()
{
    return vim_test::runAll();
}
