#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "context/data.hpp"
#include "shell/values.hpp"
using namespace os1;
TEST(TypedContext, RejectsCyclesUnknownKindsDuplicateKeysAndNoncanonicalPadding)
{
    context::Data d;
    context::defaults(d);
    ASSERT_TRUE(context::validate(d));
    auto bad = d;
    bad.nodes[1].parent = 1;
    EXPECT_FALSE(context::validate(bad));
    bad = d;
    bad.nodes[1].kind = 99;
    EXPECT_FALSE(context::validate(bad));
    bad = d;
    bad.nodes[1].reserved = 1;
    EXPECT_FALSE(context::validate(bad));
    bad = d;
    bad.text[OS1_CONTEXT_TEXT_BYTES - 1] = 1;
    EXPECT_FALSE(context::validate(bad));
    bad = d;
    context::add(bad, 0, "cwd", 3, OS1_VALUE_STRING, "/", 1);
    EXPECT_FALSE(context::validate(bad));
    bad = d;
    bad.nodes[1].text_bytes = 65535;
    EXPECT_FALSE(context::validate(bad));
    bad = {};
    EXPECT_TRUE(context::validate(bad));
    bad.text[0] = 1;
    EXPECT_FALSE(context::validate(bad));
}
TEST(TypedContext, ChildCopyAndRemovalAreIndependentAndCompact)
{
    context::Data parent;
    context::defaults(parent);
    auto child = parent;
    int path = context::child(child, 0, "path");
    context::Data changed;
    ASSERT_TRUE(context::without(child, path, changed));
    EXPECT_EQ(-1, context::child(changed, 0, "path"));
    EXPECT_GE(context::child(parent, 0, "path"), 0);
    EXPECT_TRUE(context::validate(changed));
}
TEST(ShellValues, TypedTreesStrictFallbackAndResolutionErrors)
{
    int warnings = 0;
    auto warn = [&](const char*, size_t) { ++warnings; };
    auto resolve = [](const char* text,
                      size_t size,
                      context::Data& out,
                      unsigned parent,
                      const char* name,
                      size_t names) {
        if(context::same(text, size, "fs.cwd", 6))
        {
            context::add(out, parent, name, names, OS1_VALUE_STRING, "/tmp", 4);
            return shell::Resolution::Found;
        }
        if(context::same(text, size, "denied", 6))
            return shell::Resolution::Failed;
        return shell::Resolution::Absent;
    };
    auto parse = [&](const char* text, bool strict, context::Data& out) {
        context::init(out);
        shell::ValueParser parser{text, strlen(text), 0, strict, resolve, warn};
        return parser.parse(out, 0, "value", 5);
    };
    context::Data d;
    ASSERT_TRUE(parse("{cwd:fs.cwd, options:[true, -9223372036854775808, \"a b\"]}", true, d));
    EXPECT_TRUE(context::validate(d));
    EXPECT_FALSE(parse("a.txt", true, d));
    EXPECT_EQ(0, warnings);
    EXPECT_TRUE(parse("a.txt", false, d));
    EXPECT_EQ(1, warnings);
    EXPECT_FALSE(parse("denied", false, d));
    EXPECT_EQ(1, warnings);
    for(auto text : {"{a:1,a:2}",
                     "[1,]",
                     "9223372036854775808",
                     "-9223372036854775809",
                     "fmt(1)",
                     "\"a\"+fs.cwd"})
        EXPECT_FALSE(parse(text, false, d)) << text;
    ASSERT_TRUE(parse("-9223372036854775808", true, d));
    char number[24];
    ASSERT_TRUE(shell::argv_string(d, 1, number, sizeof(number)));
    EXPECT_STREQ("-9223372036854775808", number);
}
TEST(ShellBinding, NamedPositionsDoNotConsumePositionalSlotsAndErrorsPrecedeEvaluation)
{
    shell::Parameter params[] = {
        {"source", true, true}, {"destination", true, true}, {"mode", false, false}};
    shell::Command c;
    int slots[3];
    size_t values[3];
    const char* error = nullptr;
    ASSERT_TRUE(shell::parse("cp destination:'b' 'a' mode:replace", c));
    ASSERT_TRUE(shell::bind_arguments(c, params, 3, slots, values, error));
    EXPECT_EQ(2, slots[0]);
    EXPECT_EQ(1, slots[1]);
    EXPECT_EQ(3, slots[2]);
    for(auto text :
        {"cp source:'a' 'b'", "cp 'a'", "cp 'a' 'b' 'c'", "cp other:'a' 'b'", "cp -r 'a' 'b'"})
    {
        ASSERT_TRUE(shell::parse(text, c));
        EXPECT_FALSE(shell::bind_arguments(c, params, 3, slots, values, error)) << text;
    }
    ASSERT_TRUE(shell::parse("cp -- '-a' 'b'", c));
    EXPECT_TRUE(shell::bind_arguments(c, params, 3, slots, values, error));
}
