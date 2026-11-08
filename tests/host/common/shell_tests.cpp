#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "input/ps2_modifiers.hpp"
#include "shell/command.hpp"
#include "shell/program_path.hpp"
#include "shell/text_buffer.hpp"
using namespace os1::shell;
TEST(ShellCommand, QuotesOperatorsAndRawTypedArguments)
{
    Command c;
    ASSERT_TRUE(parse("echo 'hello > world'>>'/tmp/a b'", c));
    EXPECT_EQ(2u, c.argc);
    EXPECT_STREQ("hello > world", c.argv[1]);
    EXPECT_EQ(Output::Append, c.output);
    EXPECT_STREQ("/tmp/a b", c.output_name);
    EXPECT_STREQ("echo 'hello > world'", c.raw);
    ASSERT_TRUE(parse(
        "call paramdemo render source.text:'a\"b' options:{limit:5,suffix:'>'} > /tmp/out", c));
    EXPECT_STREQ("call paramdemo render source.text:'a\"b' options:{limit:5,suffix:'>'}", c.raw);
    EXPECT_EQ(5u, c.argc);
    ASSERT_TRUE(parse("cat<<END>/tmp/a", c));
    EXPECT_EQ(Input::Here, c.input);
    EXPECT_STREQ("END", c.input_name);
    EXPECT_STREQ("/tmp/a", c.output_name);
}
TEST(ShellCommand, RejectsIncompleteAmbiguousAndUnsupportedCommands)
{
    for(auto text : {"echo >",
                     "echo >a >b",
                     "cat <a <<B",
                     "echo 'bad",
                     "echo x | cat",
                     "echo x;touch y",
                     ">file",
                     "echo x\\",
                     "echo {x"})
    {
        Command c;
        EXPECT_FALSE(parse(text, c)) << text;
        EXPECT_NE(nullptr, c.error);
    }
    Command c;
    EXPECT_FALSE(parse(std::string(128, 'a').c_str(), c));
    EXPECT_FALSE(parse("x a b c d e f g h i j k l m n o p", c));
}
TEST(ShellCommand, EmptyQuotedArgumentsAndEscapedOperators)
{
    Command c;
    ASSERT_TRUE(parse("echo '' a\\>b \"x\\\"y\"", c));
    ASSERT_EQ(4u, c.argc);
    EXPECT_STREQ("", c.argv[1]);
    EXPECT_STREQ("a>b", c.argv[2]);
    EXPECT_STREQ("x\"y", c.argv[3]);
    EXPECT_TRUE(parse("   ", c));
    EXPECT_EQ(0u, c.argc);
}
TEST(TextEditor, InsertReplaceDeleteAndUnterminatedLastLine)
{
    TextBuffer b;
    EXPECT_EQ(0u, b.lines());
    ASSERT_TRUE(b.splice(0, 0, "one\ntwo", 7));
    EXPECT_EQ(2u, b.lines());
    size_t at = 0;
    EXPECT_TRUE(b.position(3, at));
    EXPECT_EQ(7u, at);
    EXPECT_FALSE(b.position(4, at));
    EXPECT_TRUE(b.position(2, at));
    EXPECT_EQ(4u, at);
    ASSERT_TRUE(b.splice(4, 7, "last\n", 5));
    EXPECT_EQ(std::string("one\nlast\n"), std::string(b.data, b.size));
    ASSERT_TRUE(b.splice(0, 4, nullptr, 0));
    EXPECT_EQ(std::string("last\n"), std::string(b.data, b.size));
    EXPECT_TRUE(b.dirty);
}
TEST(TextEditor, FullBufferFailureIsTransactional)
{
    TextBuffer b;
    std::string full(b.capacity, 'x');
    ASSERT_TRUE(b.splice(0, 0, full.data(), full.size()));
    EXPECT_FALSE(b.splice(1, 1, "y", 1));
    EXPECT_EQ(full, std::string(b.data, b.size));
    EXPECT_FALSE(b.splice(4, 3, nullptr, 0));
    EXPECT_EQ(full, std::string(b.data, b.size));
    ASSERT_TRUE(b.splice(0, 1, "z", 1));
    EXPECT_EQ('z', b.data[0]);
}
TEST(Ps2Modifiers, BothControlKeysReleaseIndependentlyAndIgnorePause)
{
    os1::input::Ps2Modifiers m;
    uint8_t key = 0;
    EXPECT_FALSE(m.press(0x1d, key));
    EXPECT_EQ(1, m.control);
    EXPECT_FALSE(m.press(0xe0, key));
    EXPECT_FALSE(m.press(0x1d, key));
    EXPECT_EQ(3, m.control);
    EXPECT_FALSE(m.press(0x9d, key));
    EXPECT_EQ(2, m.control);
    EXPECT_TRUE(m.press(0x20, key));
    EXPECT_EQ(0x20, key);
    EXPECT_FALSE(m.press(0xe0, key));
    EXPECT_FALSE(m.press(0x9d, key));
    EXPECT_EQ(0, m.control);
    for(auto code : {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5})
        EXPECT_FALSE(m.press(code, key));
    EXPECT_EQ(0, m.control);
    EXPECT_FALSE(m.press(0x2a, key));
    EXPECT_FALSE(m.press(0x36, key));
    EXPECT_FALSE(m.press(0xaa, key));
    EXPECT_EQ(2, m.shift);
}

TEST(Ps2Modifiers, ExtendedKeypadEnterAndSlashRemainPrintable)
{
    os1::input::Ps2Modifiers m;
    uint8_t key = 0;
    for(auto code : {0x1c, 0x35})
    {
        EXPECT_FALSE(m.press(0xe0, key));
        EXPECT_TRUE(m.press(code, key));
        EXPECT_EQ(code, key);
        EXPECT_TRUE(m.keypad);
        EXPECT_FALSE(m.press(0xe0, key));
        EXPECT_FALSE(m.press(code | 0x80, key));
    }
    EXPECT_FALSE(m.press(0xe0, key));
    EXPECT_TRUE(m.press(0x48, key));
    EXPECT_TRUE(m.keypad);
    EXPECT_EQ(OS1_CONSOLE_KEY_UP, os1::input::navigation_key(key));
    EXPECT_TRUE(m.press(0x20, key));
    EXPECT_FALSE(m.keypad);
}

TEST(ProgramPath, RecoveryPrecedesDiskAndOnlyMissingContinues)
{
    char path[64]{};
    int calls = 0;
    auto recovery = [&](const char* candidate) {
        ++calls;
        EXPECT_STREQ(candidate, "/bin/ls");
        return 0L;
    };
    EXPECT_EQ(os1::shell::resolve_program("ls", path, sizeof(path), recovery), 0);
    EXPECT_STREQ(path, "/bin/ls");
    EXPECT_EQ(calls, 1);
    calls = 0;
    auto disk = [&](const char* candidate) {
        ++calls;
        EXPECT_STREQ(candidate, calls == 1 ? "/bin/counter" : "/disk/bin/counter");
        return calls == 1 ? long(OS1_ERR_NOT_FOUND) : 0L;
    };
    EXPECT_EQ(os1::shell::resolve_program("counter", path, sizeof(path), disk), 0);
    EXPECT_EQ(calls, 2);
    for(long error : {long(OS1_ERR_ACCESS), long(OS1_ERR_BAD_IMAGE), long(OS1_ERR_WOULD_BLOCK)})
    {
        calls = 0;
        EXPECT_EQ(os1::shell::resolve_program("counter",
                                              path,
                                              sizeof(path),
                                              [&](const char*) {
                                                  ++calls;
                                                  return error;
                                              }),
                  error);
        EXPECT_EQ(calls, 1);
    }
}

TEST(ProgramPath, ExplicitPathsNeverSearchAndBoundsRejectTruncation)
{
    char path[64]{};
    int calls = 0;
    auto probe = [&](const char*) {
        ++calls;
        return long(OS1_ERR_NOT_FOUND);
    };
    for(const char* explicit_path : {"/disk/bin/counter", "disk/bin/counter", "./counter"})
    {
        EXPECT_EQ(os1::shell::resolve_program(explicit_path, path, sizeof(path), probe), 0);
        EXPECT_STREQ(path, explicit_path);
    }
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(os1::shell::resolve_program("unknown", path, sizeof(path), probe), OS1_ERR_NOT_FOUND);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(os1::shell::resolve_program("counter", path, 8, probe), OS1_ERR_RANGE);
    EXPECT_EQ(os1::shell::resolve_program("", path, sizeof(path), probe), OS1_ERR_INVALID);
    EXPECT_EQ(os1::shell::resolve_program(nullptr, path, sizeof(path), probe), OS1_ERR_INVALID);
}

#include "fs/name_pattern.hpp"
#include "shell/values.hpp"
TEST(ShellArguments, DeclaredBooleanOptionsMixWithNamesAndPositions)
{
    Parameter parameters[] = {
        {"source", true, true}, {"destination", true, true}, {"recursive", false, false, 'r'}};
    Command c;
    int slots[3];
    size_t starts[3];
    const char* error = nullptr;
    ASSERT_TRUE(parse("cp destination:'b' -r 'a'", c));
    ASSERT_TRUE(bind_arguments(c, parameters, 3, slots, starts, error));
    EXPECT_EQ(slots[0], 3);
    EXPECT_EQ(slots[1], 1);
    EXPECT_EQ(slots[2], 2);
    EXPECT_EQ(starts[2], SIZE_MAX);
    for(auto text : {"cp -r 'a' 'b' recursive:true",
                     "cp recursive:false -r 'a' 'b'",
                     "cp -r -r 'a' 'b'",
                     "cp -z 'a' 'b'"})
    {
        ASSERT_TRUE(parse(text, c));
        EXPECT_FALSE(bind_arguments(c, parameters, 3, slots, starts, error)) << text;
    }
    ASSERT_TRUE(parse("cp -- -r 'b'", c));
    EXPECT_TRUE(bind_arguments(c, parameters, 3, slots, starts, error));
    EXPECT_EQ(slots[2], -1);
}
TEST(FilePatterns, WildcardsAreBoundedAndCaseSensitive)
{
    using os1::fs::name_matches;
    EXPECT_TRUE(name_matches("*", ""));
    EXPECT_TRUE(name_matches("a?*.txt", "abc.txt"));
    EXPECT_TRUE(name_matches("**a*b*c", "aaabbbc"));
    EXPECT_FALSE(name_matches("*.txt", "a.TXT"));
    EXPECT_FALSE(name_matches("?", ""));
    EXPECT_FALSE(name_matches("a*b*c", "abbd"));
    EXPECT_TRUE(name_matches("a[b]", "a[b]"));
}
